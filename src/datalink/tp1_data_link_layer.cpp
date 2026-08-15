// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <span>

/**
 * @file tp1_data_link_layer.cpp
 * @brief TP1 data link layer implementation
 */

#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/util/log.hpp"
#include "knx/util/hex.hpp"
#include "knx/util/result.hpp"
#include "knx/platform/raii_resources.hpp"
#include "knx/datalink/tp1_dl_common.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include <cstring>
#include <algorithm>
#include <cstdint>

#if defined(ESP_PLATFORM)
// physicalQueueNotifyCallback() runs in the bitbang driver's interrupt context
// with the flash cache potentially disabled — see the comment on its definition.
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define KNX_DL_ISR_ATTR IRAM_ATTR
#else
#define KNX_DL_ISR_ATTR
#endif

static const char* TAG = "KNX.DL.TP1";
static constexpr uint32_t kRxTaskPollTimeoutMs = 10000u;
// Physicals without a queue-notify callback only surface received frames when
// polled, so the RX task must wake on a short interval for them.
static constexpr uint32_t kRxTaskNoNotifyPollMs = 10u;
static constexpr uint32_t kRxTaskIdleDelayMs = 1u;

namespace knx {
namespace datalink {

namespace {

// Minimal header peek used only for drop diagnostics.
struct RawRxHeader {
    bool valid{false};
    GroupAddress destination{0};
    AddressType destinationType{AddressType::Individual};
};

RawRxHeader parseRawRxHeader(std::span<const uint8_t> buffer) {
    RawRxHeader header{};
    if (buffer.size() <= LENGTH_POS) {
        return header;
    }

    header.valid = true;
    const uint16_t destRaw = (static_cast<uint16_t>(buffer[DEST_ADDR_HI_POS]) << 8)
                           | buffer[DEST_ADDR_LO_POS];
    header.destination = GroupAddress(destRaw);
    header.destinationType = (buffer[LENGTH_POS] & DEST_ADDR_TYPE) != 0
        ? AddressType::Group
        : AddressType::Individual;
    return header;
}

} // namespace

Tp1DataLinkLayer::~Tp1DataLinkLayer() {
    close();
}

util::Result<void> Tp1DataLinkLayer::init(const IndividualAddress& ownAddress) {
    if (_initialized) {
        KNX_LOGW(TAG, "Already initialized");
        return util::Result<void>::ok();
    }
    
    _ownAddress = ownAddress;
    
    // Initialize physical layer
    auto physResult = _physicalPort.init(_physicalPort.context);
    if (physResult.isError()) {
        KNX_LOGE(TAG, "Failed to initialize physical layer: %s",
                 util::errorCodeToString(physResult.error()));
        return physResult.error();
    }

    // Set physical layer callback
    _physicalPort.setReceiveCallback(_physicalPort.context, physicalRxCallback, this);

    if (!_txMutex.handle()) {
        KNX_LOGE(TAG, "Failed to create TX mutex");
        _physicalPort.close(_physicalPort.context);
        return util::ErrorCode::ResourceUnavailable;
    }

    _rxTaskStopRequested.store(false);
    _rxTask = nullptr;

    if (_config.enableRxTask) {
        platform::TaskConfig rxTaskConfig{};
        rxTaskConfig.name = "knx_tp1_rx";
        rxTaskConfig.function = [this]() { rxTaskFunc(this); };
        rxTaskConfig.stackSize = _config.rxTaskStackSize;
        rxTaskConfig.priority = _config.rxTaskPriority;
        rxTaskConfig.parameter = this;
        _rxTask = _platform.createTask(rxTaskConfig);

        if (!_rxTask) {
            KNX_LOGE(TAG, "Failed to create RX task");
            _physicalPort.close(_physicalPort.context);
            return util::ErrorCode::ResourceUnavailable;
        }

        if (_physicalPort.setQueueNotifyCallback) {
            _physicalPort.setQueueNotifyCallback(_physicalPort.context,
                physicalQueueNotifyCallback, this);
        }
    }
    
    _initialized = true;
    KNX_LOGD(TAG, "TP1 data link layer initialized, own address: %d.%d.%d",
             _ownAddress.area(), _ownAddress.line(), _ownAddress.device());
    
    return util::Result<void>::ok();
}

void Tp1DataLinkLayer::setOwnAddress(const IndividualAddress& ownAddress) {
    _ownAddress = ownAddress;
    if (_physicalPort.setOwnAddress) {
        (void)_physicalPort.setOwnAddress(_physicalPort.context, ownAddress.raw);
    }
}

void Tp1DataLinkLayer::close() {
    if (_initialized) {
        _rxTaskStopRequested.store(true);
        if (_rxTask) {
            // Wake the RX worker out of its notify wait so it observes the stop
            // request immediately — on host platforms deleteTask() joins the
            // thread, which otherwise blocks for the full poll timeout.
            (void)_platform.taskNotifyGive(_rxTask);
            _platform.deleteTask(_rxTask);
            _rxTask = nullptr;
        }

        // Ensure no further upper-layer callbacks will be invoked while tearing down.
        _rxCallback.store(nullptr, std::memory_order_release);

        // Clear the physical layer's receive and queue callbacks to avoid callbacks into this
        // object after we've stopped the RX task.
        _physicalPort.setReceiveCallback(_physicalPort.context, physical::ReceiveCallback(), nullptr);
        if (_physicalPort.setQueueNotifyCallback) {
            _physicalPort.setQueueNotifyCallback(_physicalPort.context,
                physical::Tp1MediumBackend::QueueNotifyCallback(), nullptr);
        }

        _physicalPort.close(_physicalPort.context);
        _initialized = false;
    }
}

bool Tp1DataLinkLayer::isOpen() const {
    return _initialized && _physicalPort.isOpen(_physicalPort.context);
}

void Tp1DataLinkLayer::finishTxOperation() {
    if (_txOperation.active && _txMutex.handle()) {
        _platform.unlockMutex(_txMutex.handle());
    }
    _txOperation = TxOperationState{};
}

void Tp1DataLinkLayer::finishRxOperation() {
    if (_rxOperation.currentFrame) {
        releaseFrame(_rxOperation.currentFrame);
        _rxOperation.currentFrame = nullptr;
    }
    // Latch the delivered count only while the operation is live: this method
    // may run twice on a completion path (from pollReceive and again from the
    // caller), and the second run must not clobber the count with the reset 0.
    if (_rxOperation.active) {
        _lastRxProgressDeliveredCount = _rxOperation.deliveredCount;
    }
    _rxOperation = RxOperationState{};
    _rxProcessing.store(false);
}

util::Result<void> Tp1DataLinkLayer::beginTransmit(const LDataFrame& frame) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }

    if (_txOperation.active) {
        KNX_LOGW(TAG, "TX operation already active");
        return util::ErrorCode::Busy;
    }

    auto txLockResult = _platform.lockMutex(_txMutex.handle(), _config.txMutexTimeout);
    if (txLockResult.isError()) {
        KNX_LOGW(TAG, "Failed to acquire TX mutex");
        return util::ErrorCode::Timeout;
    }

    auto result = FrameCodec::encodeFrame(
        frame,
        _txOperation.availableSpan());
    if (!result.isOk()) {
        KNX_LOGE(TAG, "Failed to encode frame: %s", util::errorCodeToString(result.error()));
        _platform.unlockMutex(_txMutex.handle());
        return result.error();
    }

    _txOperation.active = true;
    _txOperation.waitingPhysicalOutcome = false;
    _txOperation.usingPhysicalProgression = false;
    _txOperation.busyAttempt = 0u;
    _txOperation.repeatAttempt = 0u;
    _txOperation.retryNotBeforeMs = 0u;
    _txOperation.length = result.value();
    return util::Result<void>::ok();
}

// Clear the "not repeated" flag on the buffered TX frame and fix up the FCS.
// The control octet is covered by the checksum, so flipping the bit without
// recomputing it would put a frame on the wire that every receiver discards.
void Tp1DataLinkLayer::markTxFrameAsRepeated() {
    if (_txOperation.length < 2u) {
        return;
    }
    _txOperation.buffer[CTRL_FIELD_POS] &= static_cast<uint8_t>(~CTRL_REPEAT);
    const auto body = std::span<const uint8_t>(_txOperation.buffer.data(), _txOperation.length - 1u);
    _txOperation.buffer[_txOperation.length - 1u] = FrameCodec::calculateChecksum(body);
}

util::Result<Tp1DataLinkLayer::TxProgressState> Tp1DataLinkLayer::pollTransmit() {
    static constexpr uint8_t MAX_BUSY_RETRIES = 3;
    // 03/02/02: at most three repetitions of a frame the peer did not
    // acknowledge.  Shares the attempt counter with the BUSY path on purpose —
    // the budget is per frame, not per failure kind.
    static constexpr uint8_t MAX_ACK_RETRIES = 3;

    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_txOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    const uint64_t nowMs = _timingPlatform ? _timingPlatform->millis() : 0u;
    if (_timingPlatform && _txOperation.retryNotBeforeMs != 0u && nowMs < _txOperation.retryNotBeforeMs) {
        return TxProgressState::Pending;
    }

    if (_txOperation.waitingPhysicalOutcome) {
        auto outcome = _physicalPort.pollTransmit(_physicalPort.context, _txOperation.physicalOutcomeSequence);
        if (outcome.isError()) {
            finishTxOperation();
            return outcome.error();
        }

        switch (outcome.value()) {
            case physical::TxOutcomeState::Pending:
                return TxProgressState::Pending;
            case physical::TxOutcomeState::Success:
                _stats.txFrames.fetch_add(1, std::memory_order_relaxed);
                finishTxOperation();
                return TxProgressState::Success;
            case physical::TxOutcomeState::Busy:
                _txOperation.waitingPhysicalOutcome = false;
                if (_txOperation.busyAttempt < MAX_BUSY_RETRIES) {
                    ++_txOperation.busyAttempt;
                    _txOperation.retryNotBeforeMs = _timingPlatform ? (nowMs + 4u) : 0u;
                    KNX_LOGD(TAG, "TX busy, retry %u/%u", _txOperation.busyAttempt, MAX_BUSY_RETRIES);
                    return TxProgressState::Pending;
                }
                KNX_LOGE(TAG, "Physical send failed: %s", util::errorCodeToString(util::ErrorCode::Busy));
                finishTxOperation();
                return TxProgressState::Busy;
            case physical::TxOutcomeState::TransmissionFailed:
            case physical::TxOutcomeState::Timeout: {
                // A NAK or a missing L_ACK on an individually addressed frame:
                // 03/02/02 has the sender repeat the frame rather than drop it,
                // with the repeat flag cleared so the peer can recognise the
                // repetition and suppress a duplicate indication.  Without this
                // a single corrupted frame silently ended the exchange, which a
                // management client only sees as "the device did not respond".
                const bool timedOut = outcome.value() == physical::TxOutcomeState::Timeout;
                _txOperation.waitingPhysicalOutcome = false;
                if (_txOperation.repeatAttempt < MAX_ACK_RETRIES) {
                    ++_txOperation.repeatAttempt;
                    _txOperation.retryNotBeforeMs = _timingPlatform ? (nowMs + 4u) : 0u;
                    markTxFrameAsRepeated();
                    _stats.txRepetitions.fetch_add(1, std::memory_order_relaxed);
                    KNX_LOGD(TAG, "TX %s, repeat %u/%u",
                             timedOut ? "no L_ACK" : "NAK",
                             _txOperation.repeatAttempt, MAX_ACK_RETRIES);
                    return TxProgressState::Pending;
                }
                KNX_LOGE(TAG, "Physical send failed after %u repetitions: %s",
                         _txOperation.repeatAttempt,
                         util::errorCodeToString(timedOut ? util::ErrorCode::Timeout
                                                          : util::ErrorCode::TransmissionFailed));
                finishTxOperation();
                return timedOut ? TxProgressState::Timeout : TxProgressState::TransmissionFailed;
            }
        }
    }

    auto txFrame = _txOperation.filledSpan();
    auto beginResult = _physicalPort.beginTransmit(_physicalPort.context, txFrame);
    if (beginResult.isOk()) {
        _txOperation.waitingPhysicalOutcome = true;
        _txOperation.usingPhysicalProgression = true;
        _txOperation.physicalOutcomeSequence = beginResult.value();
        _txOperation.retryNotBeforeMs = 0u;
        return pollTransmit();
    }

    if (beginResult.error() == util::ErrorCode::Busy && _txOperation.busyAttempt < MAX_BUSY_RETRIES) {
        ++_txOperation.busyAttempt;
        _txOperation.retryNotBeforeMs = _timingPlatform ? (nowMs + 4u) : 0u;
        KNX_LOGD(TAG, "TX busy, retry %u/%u", _txOperation.busyAttempt, MAX_BUSY_RETRIES);
        return TxProgressState::Pending;
    }

    const auto error = beginResult.error();
    KNX_LOGE(TAG, "Physical send failed: %s", util::errorCodeToString(error));
    finishTxOperation();
    if (error == util::ErrorCode::Busy) {
        return TxProgressState::Busy;
    }
    if (error == util::ErrorCode::Timeout) {
        return TxProgressState::Timeout;
    }
    return TxProgressState::TransmissionFailed;
}

util::Result<void> Tp1DataLinkLayer::sendFrame(const LDataFrame& frame) {
    auto beginResult = beginTransmit(frame);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    for (;;) {
        auto progress = pollTransmit();
        if (progress.isError()) {
            return progress.error();
        }

        switch (progress.value()) {
            case TxProgressState::Pending:
                _platform.taskDelay(1u);
                continue;
            case TxProgressState::Success:
                return util::Result<void>::ok();
            case TxProgressState::Busy:
                return util::ErrorCode::Busy;
            case TxProgressState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
            case TxProgressState::Timeout:
                return util::ErrorCode::Timeout;
        }
    }
}

util::Result<void> Tp1DataLinkLayer::beginReceive(uint32_t firstReadTimeoutMs) {
    bool expected = false;
    if (!_rxProcessing.compare_exchange_strong(expected, true)) {
        return util::ErrorCode::Busy;
    }

    if (!_rxOperation.active) {
        _lastRxProgressDeliveredCount = 0u;
        _rxOperation = RxOperationState{};
        _rxOperation.active = true;
        _rxOperation.firstReadTimeoutMs = firstReadTimeoutMs;
    }
    return util::Result<void>::ok();
}

util::Result<Tp1DataLinkLayer::RxProgressState> Tp1DataLinkLayer::pollReceive() {
    if (!_rxOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    // Note on acknowledgment: the TP1 short acknowledge (single ACK/NAK/BUSY
    // character, 15 bit times after the frame — 03_02_02 §2.2.7) is generated
    // by the MAC/ISR level of the physical backend, which is the only place
    // that can meet the t_ack deadline. The data link layer never sends
    // acknowledgment traffic of its own.
    while (_rxOperation.active) {
        std::span<const uint8_t> buffer;
        if (!_rxOperation.waitingPhysicalReceive) {
            auto beginReceive = _physicalPort.beginReceive(
                _physicalPort.context,
                (_rxOperation.readIndex == 0u) ? _rxOperation.firstReadTimeoutMs : 0u);
            if (beginReceive.isOk()) {
                _rxOperation.waitingPhysicalReceive = true;
            } else {
                finishRxOperation();
                return RxProgressState::Complete;
            }
        }

        if (_rxOperation.waitingPhysicalReceive) {
            auto receiveProgress = _physicalPort.pollReceive(_physicalPort.context);
            if (receiveProgress.isError()) {
                finishRxOperation();
                return RxProgressState::Complete;
            } else {
                if (receiveProgress.value() == ProgressState::Pending) {
                    return RxProgressState::Pending;
                }

                if (receiveProgress.value() == ProgressState::Success) {
                    auto receivedView = _physicalPort.receivedFrameView(_physicalPort.context);
                    if (receivedView.isError()) {
                        KNX_LOGW(TAG, "Received frame view unavailable, ending RX operation");
                        finishRxOperation();
                        return RxProgressState::Complete;
                    }
                    buffer = receivedView.value();
                    _rxOperation.waitingPhysicalReceive = false;
                } else {
                    finishRxOperation();
                    return RxProgressState::Complete;
                }
            }
        }

        LDataFrame* frame = allocateFrame();
        if (!frame) {
            const RawRxHeader rawHeader = parseRawRxHeader(buffer);
            KNX_LOGW(TAG, "Datalink frame pool exhausted, dropping received frame dst=0x%04X type=%s",
                     static_cast<uint16_t>(rawHeader.destination.raw),
                     rawHeader.destinationType == AddressType::Group ? "Group" : "Individual");
            _stats.rxDropped.fetch_add(1, std::memory_order_relaxed);
            ++_rxOperation.readIndex;
            continue;
        }

        auto decodeResult = FrameCodec::decodeFrame(buffer, *frame);
        if (decodeResult.isOk()) {
            // L2 duplication prevention: suppress the repetition of the
            // directly preceding correctly received frame (03_02_02 §2.3).
            // Without this, a repeated broadcast (e.g. an unacked
            // IndividualAddress_Read) is answered once per repetition — ETS
            // then reports "more than one device in programming mode".
            //
            // BUT this only applies to broadcast/group traffic. A frame repeated
            // to our OWN individual address is the connection-oriented peer
            // telling us our previous response was lost: the transport layer
            // MUST see it to retransmit the pending response (it owns duplicate
            // detection by sequence number and never re-indicates the payload
            // to the application). Dropping it here strands the connection until
            // ETS times out — "Device does not respond" mid-download. The L2
            // short-ack for the repetition was already sent at ISR level, so the
            // peer's L2 is satisfied either way.
            const bool toOwnIndividual =
                frame->destinationType == AddressType::Individual
                && !isIndividualBroadcastAddress(IndividualAddress(frame->destination.raw))
                && IndividualAddress(frame->destination.raw) == _ownAddress;
            if (!toOwnIndividual && isRepetitionOfLastRxFrame(*frame)) {
                KNX_LOGD(TAG, "RX repetition of previous frame suppressed (src=0x%04X dst=0x%04X)",
                         frame->source.raw, frame->destination.raw);
                _stats.duplicatesDropped.fetch_add(1, std::memory_order_relaxed);
                releaseFrame(frame);
                ++_rxOperation.readIndex;
                continue;
            }
            rememberRxFrame(*frame);

            const bool addressed = isAddressMatch(frame->destination, frame->destinationType);
            KNX_LOGD(TAG,
                     "RX %s 0x%04X→0x%04X %s tpdu=%s%s",
                     frame->standardFrame ? "std" : "ext",
                     frame->source.raw,
                     frame->destination.raw,
                     frame->destinationType == AddressType::Group ? "Group" : "Individual",
                     util::formatHexBytes(frame->tpdu).c_str(),
                     addressed ? "" : " (filtered)");

            if (addressed) {
                const auto* cb = _rxCallback.load(std::memory_order_acquire);
                if (cb) { (*cb)(*frame); }

                releaseFrame(frame);
                _stats.rxFrames.fetch_add(1, std::memory_order_relaxed);
                ++_rxOperation.deliveredCount;
            } else {
                releaseFrame(frame);
                _stats.filterDropped.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            if (decodeResult.error() == util::ErrorCode::InvalidFrameSize) {
                // A short buffer here is a physical-layer artifact (a lone
                // ACK byte, a collision fragment) rather than a malformed
                // telegram — frames that short never reached the wire as
                // L_Data. Silent by design to avoid one WARN per bus ACK.
            } else {
                KNX_LOGW(TAG, "RX raw len=%zu bytes=%s decode failed: %s",
                         buffer.size(),
                         util::formatHexBytes(buffer).c_str(),
                         util::errorCodeToString(decodeResult.error()));
            }
            _stats.decodeFailed.fetch_add(1, std::memory_order_relaxed);
            releaseFrame(frame);
        }

        ++_rxOperation.readIndex;
    }

    finishRxOperation();
    return RxProgressState::Complete;
}

void Tp1DataLinkLayer::setReceiveCallback(LDataCallback callback) {
    if (callback) {
        _rxCallbackStorage = std::move(callback);
        _rxCallback.store(&_rxCallbackStorage, std::memory_order_release);
        KNX_LOGD(TAG, "DataLink receive callback set, context=%p", static_cast<const void*>(this));
    } else {
        _rxCallback.store(nullptr, std::memory_order_release);
        KNX_LOGD(TAG, "DataLink receive callback cleared");
    }
}

size_t Tp1DataLinkLayer::processRxAvailable(uint32_t firstReadTimeoutMs) {
    if (_rxTask) {
        return 0u;
    }

    return processRxAvailableInternal(firstReadTimeoutMs);
}

util::Result<void> Tp1DataLinkLayer::addGroupAddress(const GroupAddress& address) {
    // Check if already in list
    auto it = std::find(_groupAddresses.begin(), _groupAddresses.end(), address);
    if (it != _groupAddresses.end()) {
        return util::Result<void>::ok();  // Already present
    }

    if (!_groupAddresses.push_back(address)) {
        return util::ErrorCode::BufferTooSmall;
    }
    KNX_LOGD(TAG, "Added group address: %d/%d/%d",
             address.main(), address.middle(), address.sub());
    publishAckGroupAddresses();

    return util::Result<void>::ok();
}

util::Result<void> Tp1DataLinkLayer::removeGroupAddress(const GroupAddress& address) {
    auto it = std::find(_groupAddresses.begin(), _groupAddresses.end(), address);
    if (it != _groupAddresses.end()) {
        _groupAddresses.erase(static_cast<size_t>(std::distance(_groupAddresses.begin(), it)));
        KNX_LOGD(TAG, "Removed group address: %d/%d/%d",
                 address.main(), address.middle(), address.sub());
        publishAckGroupAddresses();
        return util::Result<void>::ok();
    }
    return util::ErrorCode::OperationFailed;
}

void Tp1DataLinkLayer::clearGroupAddresses() {
    _groupAddresses.clear();
    KNX_LOGD(TAG, "Cleared all group addresses");
    publishAckGroupAddresses();
}

void Tp1DataLinkLayer::publishAckGroupAddresses() {
    if (!_physicalPort.setAckGroupAddresses) {
        return;
    }

    // Snapshot the raw group addresses for the physical layer's ISR-side
    // L_ACK matcher. The ISR table is smaller than ours (it scans per frame,
    // inside the interrupt); addresses beyond its capacity simply don't get
    // the early ACK — the sender's link-layer repeat covers the gap.
    std::array<uint16_t, MAX_GROUP_ADDRESSES> raw{};
    size_t count = 0;
    for (const auto& ga : _groupAddresses) {
        raw[count++] = ga.raw;
    }
    _physicalPort.setAckGroupAddresses(_physicalPort.context,
                                       std::span<const uint16_t>(raw.data(), count));
}

void Tp1DataLinkLayer::setPromiscuousMode(PromiscuousMode mode) {
    _promiscuousMode = (mode == PromiscuousMode::Enable);
    KNX_LOGI(TAG, "Promiscuous mode: %s", _promiscuousMode ? "enabled" : "disabled");
}

LDataFrame* Tp1DataLinkLayer::allocateFrame() {
    for (size_t i = 0; i < FRAME_POOL_SIZE; ++i) {
        if (!_frameInUse[i]) {
            _frameInUse[i] = true;
            // Reset dynamic fields
            _framePool[i].tpdu.clear();
            return &_framePool[i];
        }
    }
    KNX_LOGW(TAG, "Frame pool exhausted");
    return nullptr;
}

void Tp1DataLinkLayer::releaseFrame(LDataFrame* frame) {
    if (!frame) return;
    for (size_t i = 0; i < FRAME_POOL_SIZE; ++i) {
        if (&_framePool[i] == frame) {
            _frameInUse[i] = false;
            frame->tpdu.clear();
            return;
        }
    }
}

// Duplication prevention (03_02_02 §2.3): a frame whose wire r-bit marks it
// as repeated and whose content matches the directly preceding correctly
// received frame must not be indicated to the DL user again. The short-ack
// for the repetition is still sent (ISR level) so the sender stops repeating.
bool Tp1DataLinkLayer::isRepetitionOfLastRxFrame(const LDataFrame& frame) const {
    if (!frame.repeated || !_lastRxFrame.valid) {
        return false;
    }

    if (frame.source.raw != _lastRxFrame.source ||
        frame.destination.raw != _lastRxFrame.destination ||
        frame.destinationType != _lastRxFrame.destinationType ||
        frame.priority != _lastRxFrame.priority ||
        frame.hopCount != _lastRxFrame.hopCount ||
        frame.tpdu.size() != _lastRxFrame.tpdu.size()) {
        return false;
    }

    return std::equal(frame.tpdu.begin(), frame.tpdu.end(), _lastRxFrame.tpdu.begin());
}

void Tp1DataLinkLayer::rememberRxFrame(const LDataFrame& frame) {
    _lastRxFrame.valid = true;
    _lastRxFrame.source = frame.source.raw;
    _lastRxFrame.destination = frame.destination.raw;
    _lastRxFrame.destinationType = frame.destinationType;
    _lastRxFrame.priority = frame.priority;
    _lastRxFrame.hopCount = frame.hopCount;
    _lastRxFrame.tpdu.assign(std::span<const uint8_t>(frame.tpdu.data(), frame.tpdu.size()));
}

// Address filtering
bool Tp1DataLinkLayer::isAddressMatch(const GroupAddress& address, AddressType addressType) const {
    if (_promiscuousMode) {
        return true;
    }
    
    if (addressType == AddressType::Group) {
        if (isGroupBroadcastAddress(address)) {
            return true;
        }

        // Check group address table
        GroupAddress groupAddr(address.raw);
        return std::find(_groupAddresses.begin(), _groupAddresses.end(), groupAddr) != _groupAddresses.end();
    } else {
        // Check individual address
        IndividualAddress indAddr(address.raw);
        if (isIndividualBroadcastAddress(indAddr)) {
            return true;
        }
        return indAddr == _ownAddress;
    }
}

// Physical layer RX callback
void Tp1DataLinkLayer::physicalRxCallback(void* context) {
    Tp1DataLinkLayer* self = static_cast<Tp1DataLinkLayer*>(context);
    if (!self) {
        return;
    }

    // Disabled per-callback logging for performance

    if (self->_rxTask) {
        auto notifyResult = self->_platform.taskNotifyGive(self->_rxTask);
        if (notifyResult.isError()) {
            KNX_LOGW(TAG, "Failed to notify RX task, processing receive queue directly (error=%d)",
                     static_cast<int>(notifyResult.error()));
            if (!self->_rxProcessing.load()) {
                (void)self->processRxAvailableInternal(0);
            }
        }
        return;
    }

    if (self->_rxProcessing.load()) {
        KNX_LOGD(TAG, "Physical RX callback ignored because RX processing is already active");
        return;
    }

    KNX_LOGD(TAG, "Physical RX callback processing receive queue directly");
    (void)self->processRxAvailableInternal(0);
}

// Physical layer queue notification callback for ISR contexts.
//
// The bitbang driver calls this from queueMessage() — i.e. from the bit-timing
// alarm and the RX edge interrupt, for every byte, error and telegram boundary.
// Those interrupts are allocated with ESP_INTR_FLAG_IRAM so they keep running
// while the flash cache is off (NVS commits during ETS commissioning), which
// means everything reachable from here must be IRAM/DRAM resident.
//
// Two consequences, both deliberate:
//   - KNX_DL_ISR_ATTR puts this function in IRAM. Without it, it linked into
//     flash (.flash.text) and every ISR message dereferenced a cached address.
//   - The FreeRTOS call is made directly instead of through
//     Platform::taskNotifyGiveFromISR(). That method is virtual, and the vtable
//     lives in .rodata — also flash. A virtual dispatch here would fault with
//     the cache disabled no matter where the target function sits.
//     vTaskNotifyGiveFromISR() itself is IRAM-resident via CONFIG_FREERTOS_IN_IRAM.
// Non-ESP platforms have no cache to disable, so they keep the portable path.
void KNX_DL_ISR_ATTR Tp1DataLinkLayer::physicalQueueNotifyCallback(void* context) {
    Tp1DataLinkLayer* self = static_cast<Tp1DataLinkLayer*>(context);
    if (!self || !self->_rxTask) {
        return;
    }

#if defined(ESP_PLATFORM)
    BaseType_t higherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(self->_rxTask), &higherPriorityTaskWoken);
    if (higherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR(higherPriorityTaskWoken);
    }
#else
    (void)self->_platform.taskNotifyGiveFromISR(self->_rxTask);
#endif
}

// RX task
void Tp1DataLinkLayer::rxTaskFunc(void* param) {
    Tp1DataLinkLayer* self = static_cast<Tp1DataLinkLayer*>(param);
    if (!self) {
        return;
    }
    
    const uint32_t pollTimeoutMs = self->_physicalPort.hasQueueNotify
        ? kRxTaskPollTimeoutMs
        : kRxTaskNoNotifyPollMs;

    while (!self->_rxTaskStopRequested.load()) {
        (void)self->_platform.taskNotifyTake(platform::TaskNotifyClearMode::Clear,
                                             pollTimeoutMs);
        if (self->_rxTaskStopRequested.load()) {
            break;
        }
        (void)self->processRxAvailableInternal(0);
    }
}

size_t Tp1DataLinkLayer::processRxAvailableInternal(uint32_t firstReadTimeoutMs) {
    // Disabled per-poll logging for performance (ISR timing critical)
    auto beginResult = beginReceive(firstReadTimeoutMs);
    if (beginResult.isError()) {
        KNX_LOGW(TAG, "beginReceive failed: %s", util::errorCodeToString(beginResult.error()));
        return 0u;
    }

    for (;;) {
        auto progress = pollReceive();
        if (progress.isError()) {
            KNX_LOGW(TAG, "pollReceive failed: %s", util::errorCodeToString(progress.error()));
            finishRxOperation();
            return _lastRxProgressDeliveredCount;
        }

        switch (progress.value()) {
            case RxProgressState::Pending:
                KNX_LOGD(TAG, "RX poll pending, no new frames available");
                finishRxOperation();
                return _lastRxProgressDeliveredCount;
            case RxProgressState::Complete:
                finishRxOperation();
                return _lastRxProgressDeliveredCount;
            case RxProgressState::Busy:
                finishRxOperation();
                return _lastRxProgressDeliveredCount;
            case RxProgressState::TransmissionFailed:
                finishRxOperation();
                return _lastRxProgressDeliveredCount;
            case RxProgressState::Timeout:
                finishRxOperation();
                return _lastRxProgressDeliveredCount;
        }
    }
}

} // namespace datalink
} // namespace knx
