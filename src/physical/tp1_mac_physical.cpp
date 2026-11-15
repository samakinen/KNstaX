// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_mac_physical.cpp
 * @brief The TP1 physical layer: frame-oriented MAC over an owned Tp1MediumBackend
 */

#include "knx/physical/tp1_mac_physical.hpp"

#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_dl_common.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/physical/tp1_ack_utils.hpp"
#include "knx/util/hex.hpp"
#include "knx/util/log.hpp"
#include "knx/util/timing_utils.hpp"

#include <cinttypes>
#include <cstring>
#include <utility>

static const char* TAG = "KNX.TP1.MACPHY";

namespace knx {
namespace physical {

Tp1MacPhysical::Tp1MacPhysical(std::unique_ptr<Tp1MediumBackend> backend)
    : _initialized(false)
    , _busMonitorMode(false)
    , _backend(std::move(backend))
    , _config()
    , _timingPlatform(nullptr)
    , _rxCallback(nullptr)
    , _rxCallbackContext(nullptr)
    , _txOutcomeTimeoutMs(DEFAULT_TX_OUTCOME_TIMEOUT_MS)
    , _rxMutex()
    , _rxFrames()
    , _rxHead(0)
    , _rxCount(0)
    , _rxOverflowDrops(0)
{
}

Tp1MacPhysical::~Tp1MacPhysical() {
    close();
}

util::Result<void> Tp1MacPhysical::init() {
    if (_initialized) {
        return util::Result<void>::ok();
    }

    _config.ownIndividualAddressRaw = initialIndividualAddress().raw;
    _config.busMonitorMode = _busMonitorMode;

    _backend->setEventCallback(onTp1Event, this);
    auto initResult = _backend->init(_config);
    if (initResult.isError()) {
        _backend->setEventCallback(nullptr, nullptr);
        return initResult;
    }

    _initialized = true;
    return util::Result<void>::ok();
}

util::Result<void> Tp1MacPhysical::setOwnAddress(uint16_t addressRaw) {
    _config.ownIndividualAddressRaw = addressRaw;
    return _backend->setOwnAddress(addressRaw);
}

void Tp1MacPhysical::setAckGroupAddresses(std::span<const uint16_t> addresses) {
    _backend->setAckGroupAddresses(addresses);
}

void Tp1MacPhysical::setAckIndividualAddresses(std::span<const uint16_t> addresses) {
    _backend->setAckIndividualAddresses(addresses);
}

#if KNX_FEATURE_COUPLER
void Tp1MacPhysical::setGroupAckFilter(const GroupAckFilter* filter) {
    _backend->setGroupAckFilter(filter);
}
#endif

void Tp1MacPhysical::close() {
    if (!_initialized) {
        return;
    }

    _backend->setEventCallback(nullptr, nullptr);
    // The ISR keeps its busy input across a close; a later init() must not
    // start out refusing every frame with BUSY.
    _backend->setLocalBusy(false);
    _backend->close();
    _lastTxAckResponse = Tp1AckClass::None;
    _lastTxAckDeadlineMiss = false;
    _txOutcomeSequence = 0;

    {
        platform::LocalLock<platform::LocalMutex> lock(_rxMutex);
        _rxHead = 0;
        _rxCount = 0;
        _rxHeld = false;
        _rxOverflowDrops = 0;
        _localBusyAsserted = false;
    }

    _initialized = false;
}

bool Tp1MacPhysical::isOpen() const {
    return _initialized;
}

util::Result<void> Tp1MacPhysical::validateOutgoingFrame(std::span<const uint8_t> frame,
                                                         datalink::LDataFrame* decoded) const {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (frame.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    if (_busMonitorMode) {
        return util::ErrorCode::OperationNotSupported;
    }
    if (_gateTxOnLinkDown && _linkState.load(std::memory_order_relaxed) == LinkState::Down) {
        // Nothing can be acknowledged on an unpowered bus; sending would only
        // spend the full CSMA + retransmission budget to reach the same answer.
        return util::ErrorCode::ResourceUnavailable;
    }

    datalink::LDataFrame validatedFrame;
    auto validateResult = datalink::FrameCodec::decodeFrame(frame, validatedFrame);
    if (validateResult.isError()) {
        return validateResult.error();
    }

    if (decoded != nullptr) {
        *decoded = validatedFrame;
    }
    return util::Result<void>::ok();
}

util::Result<uint32_t> Tp1MacPhysical::beginTransmit(std::span<const uint8_t> frame) {
    datalink::LDataFrame outgoing;
    auto validateResult = validateOutgoingFrame(frame, &outgoing);
    if (validateResult.isError()) {
        return validateResult.error();
    }

    // On TP1 every L_Data frame is acknowledged, group and broadcast frames
    // included (03_02_02 §2.4.2), whether or not the sender set the A bit, so
    // the outcome of the ACK window decides success.
    //
    // The one exception must be exactly the ISR's (txFrameExpectsAck): it
    // opens no window for individual address FFFFh, the address every
    // unconfigured device shares, which therefore never acknowledges. A
    // mismatch would leave this waiting for an outcome the ISR never reports
    // (docs/reference/tp1_bitbang_isr.md §6.1).
    const bool toUnconfiguredAddress =
        outgoing.destinationType == AddressType::Individual
        && isInitialIndividualAddress(IndividualAddress(outgoing.destination.raw));
    _txAckRequested = (frame[0] & datalink::CTRL_ACK_REQ) != 0u || !toUnconfiguredAddress;

    const uint32_t initialTxOutcomeSequence = _txOutcomeSequence;

    auto sendResult = _backend->sendFrame(frame);
    if (sendResult.isError()) {
        return sendResult.error();
    }

    if (!_backend->getCapabilities().supportsDetailedTxConfirm) {
        auto tickResult = _backend->service();
        if (tickResult.isError()) {
            return tickResult.error();
        }
    }

    return initialTxOutcomeSequence;
}

util::Result<TxOutcomeState> Tp1MacPhysical::pollTransmit(uint32_t initialSequence) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }

    auto tickResult = _backend->service();
    if (tickResult.isError()) {
        return tickResult.error();
    }

    if (!_txAckRequested) {
        // Wait until physical TX actually completes before declaring success.
        // Without this check, the caller immediately queues the next frame while the
        // bitbang driver is still sending bits, causing a TX collision.
        if (_backend->getState() == Tp1MediumState::Transmitting) {
            return TxOutcomeState::Pending;
        }
        return TxOutcomeState::Success;
    }

    if (!_backend->getCapabilities().supportsDetailedTxConfirm) {
        return TxOutcomeState::Success;
    }

    if (_txOutcomeSequence == initialSequence) {
        return TxOutcomeState::Pending;
    }

    if (_lastTxAckDeadlineMiss) {
        return TxOutcomeState::Timeout;
    }

    switch (_lastTxAckResponse) {
        case Tp1AckClass::Ack:
            return TxOutcomeState::Success;
        case Tp1AckClass::Busy:
        case Tp1AckClass::NackBusy:
            return TxOutcomeState::Busy;
        case Tp1AckClass::Nack:
        case Tp1AckClass::None:
        default:
            return TxOutcomeState::TransmissionFailed;
    }
}

util::Result<void> Tp1MacPhysical::beginReceive(uint32_t timeoutMs) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }

    {
        // The caller is done with the frame the previous receive handed out.
        platform::LocalLock<platform::LocalMutex> lock(_rxMutex);
        releaseHeldFrameLocked();
    }

    _receiveActive = true;
    _lastReceivedFrameLength = 0;
    const uint64_t startMs = util::nowMs(_timingPlatform);
    _receiveDeadlineMs = startMs + static_cast<uint64_t>(timeoutMs);
    return util::Result<void>::ok();
}

util::Result<Tp1MacPhysical::ProgressState> Tp1MacPhysical::pollReceive() {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_receiveActive) {
        return util::ErrorCode::OperationNotReady;
    }

    (void)_backend->service();

    {
        platform::LocalLock<platform::LocalMutex> lock(_rxMutex);
        releaseHeldFrameLocked();
        if (_rxCount > 0) {
            // Hand the queue slot itself to the caller instead of copying it
            // out: receivedFrameView() points into it, and it stays reserved
            // (still counted, so the producer cannot reuse it) until the next
            // beginReceive()/pollReceive() releases it.
            _rxHeld = true;
            _lastReceivedFrameLength = _rxFrames[_rxHead].length;
            _receiveActive = false;
            return ProgressState::Complete;
        }
    }

    if (util::nowMs(_timingPlatform) >= _receiveDeadlineMs) {
        _receiveActive = false;
        return ProgressState::Timeout;
    }

    return ProgressState::Pending;
}

util::Result<std::span<const uint8_t>> Tp1MacPhysical::receivedFrameView() {
    // Only the consumer moves _rxHead, and the producer never writes the held
    // slot, so the view is stable without the lock.
    if (_lastReceivedFrameLength == 0 || !_rxHeld) {
        return util::ErrorCode::OperationNotReady;
    }
    return std::span<const uint8_t>(_rxFrames[_rxHead].data).first(_lastReceivedFrameLength);
}

void Tp1MacPhysical::releaseHeldFrameLocked() {
    if (_rxHeld) {
        _rxHead = (_rxHead + 1u) % RX_QUEUE_CAPACITY;
        --_rxCount;
        _rxHeld = false;
    }
    updateLocalBusyLocked();
}

void Tp1MacPhysical::updateLocalBusyLocked() {
    // 03_02_02 §2.4.2: a device without the resources to process a correct
    // frame may answer BUSY, provided it can take frames again within 100 ms —
    // which a draining queue can. The decision has to reach the ISR before
    // the frame's header does, so it is made ahead of time from the queue
    // depth, with RX_BUSY_RESERVE slots left for frames that were already
    // acknowledged while this task was not running.
    const bool busy = _rxCount >= (RX_QUEUE_CAPACITY - RX_BUSY_RESERVE);
    if (busy != _localBusyAsserted) {
        _localBusyAsserted = busy;
        _backend->setLocalBusy(busy);
    }
}

util::Result<size_t> Tp1MacPhysical::sendFrame(std::span<const uint8_t> frame) {
    auto beginResult = beginTransmit(frame);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    if (_backend->getCapabilities().supportsDetailedTxConfirm) {
        auto txOutcomeResult = waitForTxOutcome(beginResult.value());
        if (txOutcomeResult.isError()) {
            return txOutcomeResult.error();
        }
    }

    return frame.size();
}

util::Result<std::vector<uint8_t>> Tp1MacPhysical::receiveFrame(uint32_t timeoutMs) {
    auto beginResult = beginReceive(timeoutMs);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    for (;;) {
        auto progress = pollReceive();
        if (progress.isError()) {
            return progress.error();
        }

        switch (progress.value()) {
            case ProgressState::Pending:
                if (timeoutMs == 0) {
                    return util::ErrorCode::Timeout;
                }
                util::delayMs(_timingPlatform, 1u);
                continue;
            case ProgressState::Complete: {
                auto received = receivedFrameView();
                if (received.isError()) return received.error();
                return std::vector<uint8_t>(received.value().begin(), received.value().end());
            }
            case ProgressState::Timeout:
                return util::ErrorCode::Timeout;
            case ProgressState::Busy:
                return util::ErrorCode::Busy;
            case ProgressState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
        }
    }
}

void Tp1MacPhysical::setReceiveCallback(ReceiveCallback callback, void* context) {
    _rxCallback = std::move(callback);
    _rxCallbackContext = context;
    KNX_LOGD(TAG, "Physical receive callback %s, context=%p",
             _rxCallback ? "set" : "cleared",
             context);
}

void Tp1MacPhysical::setQueueNotifyCallback(QueueNotifyCallback callback, void* context) {
    _backend->setQueueNotifyCallback(callback, context);
}

void Tp1MacPhysical::setTimingPlatform(platform::TimingPlatform* timingPlatform) {
    _timingPlatform = timingPlatform;
}

void Tp1MacPhysical::setTxOutcomeTimeoutMs(uint32_t timeoutMs) {
    _txOutcomeTimeoutMs = timeoutMs;
}

uint32_t Tp1MacPhysical::txOutcomeTimeoutMs() const {
    return _txOutcomeTimeoutMs;
}

PhysicalLayerState Tp1MacPhysical::getState() const {
    if (!_initialized) {
        return PhysicalLayerState::Idle;
    }
    return mapState(_backend->getState());
}

util::Result<void> Tp1MacPhysical::setBusMonitorMode(Toggle mode) {
    const bool enabled = isEnabled(mode);
    _busMonitorMode = enabled;
    _config.busMonitorMode = enabled;

    if (!_initialized) {
        return util::Result<void>::ok();
    }

    auto setResult = _backend->setBusMonitorMode(enabled);
    if (setResult.isError() && setResult.error() != util::ErrorCode::OperationNotSupported) {
        return setResult.error();
    }

    return util::Result<void>::ok();
}

LinkState Tp1MacPhysical::getLinkState() const {
    if (!_initialized) {
        return LinkState::Unknown;
    }
    return _backend->getLinkState();
}

void Tp1MacPhysical::setLinkStateCallback(LinkStateCallback callback, void* context) {
    _linkStateCallback = std::move(callback);
    _linkStateCallbackContext = context;
}

void Tp1MacPhysical::setTxGatedByLinkDown(bool enabled) {
    _gateTxOnLinkDown = enabled;
}

void Tp1MacPhysical::onTp1Event(const Tp1RxEvent& event, void* context) {
    auto* self = static_cast<Tp1MacPhysical*>(context);
    if (self) {
        self->handleTp1Event(event);
    }
}

void Tp1MacPhysical::latchTxOutcome(const Tp1RxEvent& event) {
    switch (event.type) {
        case Tp1RxEventType::TxAckResponse:
            _lastTxAckResponse = event.ackClass;
            _lastTxAckDeadlineMiss = false;
            ++_txOutcomeSequence;
            break;
        case Tp1RxEventType::TxAckDeadlineMiss:
            _lastTxAckResponse = Tp1AckClass::None;
            _lastTxAckDeadlineMiss = true;
            ++_txOutcomeSequence;
            break;
        default:
            break;
    }
}

void Tp1MacPhysical::handleTp1Event(const Tp1RxEvent& event) {
    // The TX outcome is latched first, whatever else the event carries.
    latchTxOutcome(event);

    // Byte-level parity/framing errors are already logged (deduplicated) by
    // the driver's noteRxPartialError/flushRxPartialErrorSummary — logging
    // again here would just repeat the same information with less detail.

    // Link events carry no frame, so they must be handled before the guard.
    if (event.type == Tp1RxEventType::LinkStateChanged) {
        handleLinkStateEvent(event.linkStatus);
        return;
    }

    if (event.frame.empty()) {
        return;
    }

    // Defensive guard: some backends can surface standalone TP1 ACK bytes as
    // len=1 frame callbacks. These are link-layer responses, not telegrams.
    if (event.frame.size() == 1u && isTp1AckByte(event.frame[0])) {
        return;
    }

    // Raw bytes are logged once, either by the datalink layer's "RX ..."
    // summary on successful decode, or by its "decode failed" WARN.

    if (event.frame.size() > MAX_RX_FRAME_SIZE) {
        _rxOversizedDrops.fetch_add(1, std::memory_order_relaxed);
        KNX_LOGW(TAG, "Dropping oversized TP1 frame: %zu bytes (maximum %zu)", event.frame.size(),
                 MAX_RX_FRAME_SIZE);
        return;
    }

    {
        platform::LocalLock<platform::LocalMutex> lock(_rxMutex);
        if (_rxCount >= RX_QUEUE_CAPACITY) {
            // Only reachable when more than RX_BUSY_RESERVE frames were
            // acknowledged before the busy state reached the ISR. The newest
            // is dropped, so the queue stays in arrival order and the frames
            // that have waited longest are kept.
            ++_rxOverflowDrops;
            _rxOverflowDropsTotal.fetch_add(1, std::memory_order_relaxed);
            if (_rxOverflowDrops == 1u || (_rxOverflowDrops % 64u) == 0u) {
                KNX_LOGW(TAG, "RX queue full, dropping acknowledged TP1 frame (%zu drops)", _rxOverflowDrops);
            }
            return;
        }
        _rxOverflowDrops = 0;

        const size_t insertIndex = (_rxHead + _rxCount) % RX_QUEUE_CAPACITY;
        auto& buffered = _rxFrames[insertIndex];
        buffered.length = event.frame.size();
        std::memcpy(buffered.data.data(), event.frame.data(), buffered.length);
        ++_rxCount;
        updateLocalBusyLocked();
    }

    if (_rxCallback) {
        _rxCallback(_rxCallbackContext);
    }
}

void Tp1MacPhysical::handleLinkStateEvent(const LinkStatus& status) {
    _linkState.store(status.state, std::memory_order_relaxed);

    KNX_LOGD(TAG, "Medium link state: %s (transition %" PRIu32 ")",
             linkStateToString(status.state), status.transitionCount);

    if (_linkStateCallback) {
        _linkStateCallback(status, _linkStateCallbackContext);
    }
}

PhysicalLayerState Tp1MacPhysical::mapState(Tp1MediumState state) {
    switch (state) {
        case Tp1MediumState::Uninitialized:
        case Tp1MediumState::Idle:
            return PhysicalLayerState::Idle;
        case Tp1MediumState::Receiving:
            return PhysicalLayerState::Receiving;
        case Tp1MediumState::Transmitting:
            return PhysicalLayerState::Transmitting;
        case Tp1MediumState::Error:
        default:
            return PhysicalLayerState::Error;
    }
}

util::Result<void> Tp1MacPhysical::waitForTxOutcome(uint32_t initialSequence) {
    const uint64_t startMs = util::nowMs(_timingPlatform);
    const uint64_t deadlineMs = startMs + static_cast<uint64_t>(_txOutcomeTimeoutMs);

    for (;;) {
        auto tickResult = _backend->service();
        if (tickResult.isError()) {
            return tickResult.error();
        }

        if (!_txAckRequested) {
            // No ACK window: the frame is done when the medium leaves
            // Transmitting. Returning earlier would let the caller stage the
            // next frame while this one is still on the wire, and the medium
            // would reject it as busy.
            if (_backend->getState() != Tp1MediumState::Transmitting) {
                return util::Result<void>::ok();
            }
            // Still transmitting — keep looping.
        } else if (_txOutcomeSequence != initialSequence) {
            if (_lastTxAckDeadlineMiss) {
                return util::ErrorCode::Timeout;
            }

            const Tp1AckClass ackClass = _lastTxAckResponse;
            switch (ackClass) {
                case Tp1AckClass::Ack:
                    return util::Result<void>::ok();
                case Tp1AckClass::Busy:
                case Tp1AckClass::NackBusy:
                    return util::ErrorCode::Busy;
                case Tp1AckClass::Nack:
                case Tp1AckClass::None:
                default:
                    return util::ErrorCode::TransmissionFailed;
            }
        }

        if (util::nowMs(_timingPlatform) >= deadlineMs) {
            return util::ErrorCode::Timeout;
        }

        util::delayMs(_timingPlatform, 1u);
    }
}

} // namespace physical
} // namespace knx