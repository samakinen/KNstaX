// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/bitbang_driver_timer_isr_espidf.hpp"

#include "knx/physical/tp1_ack_utils.hpp"
#include "knx/util/log.hpp"
#include "knx/util/hex.hpp"
#include "knx/physical/timer_gpio_hal.h"

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#endif

#include <algorithm>
#include <cinttypes>

static const char* TAG = "KNX.BitBang";

namespace knx {
namespace physical {

#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
// Human-readable name for an L2 acknowledgement byte, for the bus-monitor trace.
static const char* busMonAckName(uint8_t b) {
    switch (b) {
        case BitBangDriverTimerIsr::ACK_BYTE_ACK:       return "ACK";
        case BitBangDriverTimerIsr::ACK_BYTE_NACK:      return "NACK";
        case BitBangDriverTimerIsr::ACK_BYTE_BUSY:      return "BUSY";
        case BitBangDriverTimerIsr::ACK_BYTE_NACK_BUSY: return "NACK_BUSY";
        default:                                        return "?";
    }
}
#endif

BitBangDriverTimerIsrEspIdf::~BitBangDriverTimerIsrEspIdf() {
    close();
}

util::Result<void> BitBangDriverTimerIsrEspIdf::init(const BitBangConfig& config) {
    if (_initialized) {
        return util::Result<void>::ok();
    }

    auto validation = config.validate(false);
    if (validation.isError()) {
        return validation;
    }

    _config = config;
    _busMonitorMode = false;
    _eventHead = 0;
    _eventTail = 0;
    _eventCount = 0;
    _diagnostics = Tp1AckDiagnosticsSnapshot{};
    _collisionDetected = false;
    _collisionEventCount = 0;
    _observedDroppedCount = 0;
    _activeFrameLength = 0;
    _activeFrameStarted = false;
    _activeFrameErrorFlags = 0;
    _activeFrameOverflowCount = 0;
    _lastRxErrorPartialLength = 0;
    _lastRxErrorFlag = 0;
    _lastRxErrorRepeatCount = 0;
    _queueNotifyCallback = nullptr;
    _queueNotifyContext = nullptr;

    if (!knx_timer_gpio_hal_make_espidf(&_hal)) {
        return util::ErrorCode::OperationFailed;
    }
    _halCreated = true;

    if (config.txPin == 0xFF || config.rxPin == 0xFF) {
        close();
        return util::ErrorCode::InvalidParameter;
    }

    if (!_core.init(_hal, config)) {
        close();
        return util::ErrorCode::OperationFailed;
    }

    _initialized = true;
    _state = DriverState::Idle;
    _observedDroppedCount = _core.droppedMessageCount();

    if (!initLinkSignal()) {
        // A missing or unusable link signal is not fatal: the driver simply
        // keeps reporting LinkState::Unknown, exactly as before the signal
        // existed. Only a configured-but-unusable pin is worth a warning.
        if (_config.linkSignal.isConfigured()) {
            KNX_LOGW(TAG,
                     "Link-health pin %u configured but unavailable — reporting LinkState::Unknown",
                     static_cast<unsigned>(_config.linkSignal.pin));
        }
    }

    return util::Result<void>::ok();
}

void BitBangDriverTimerIsrEspIdf::close() {
    closeLinkSignal();

    if (_initialized) {
        _core.shutdown();
    }

    if (_halCreated) {
        (void)knx_timer_gpio_hal_destroy_espidf(&_hal);
        _halCreated = false;
    }

    _initialized = false;
    _state = DriverState::Uninitialized;
    _collisionDetected = false;
    _collisionEventCount = 0;
    _activeFrameLength = 0;
    _activeFrameStarted = false;
    _activeFrameErrorFlags = 0;
    _activeFrameOverflowCount = 0;
    _lastRxErrorPartialLength = 0;
    _lastRxErrorFlag = 0;
    _lastRxErrorRepeatCount = 0;
    _eventHead = 0;
    _eventTail = 0;
    _eventCount = 0;
    _observedDroppedCount = 0;
}

util::Result<void> BitBangDriverTimerIsrEspIdf::send(std::span<const uint8_t> frame) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }

    if (frame.empty()) {
        return util::ErrorCode::InvalidParameter;
    }

    if (_busMonitorMode) {
        return util::ErrorCode::OperationNotSupported;
    }

    KNX_LOGD(TAG,
             "TX len=%zu frame=%s",
             frame.size(),
             util::formatHexBytes(frame).c_str());

    const std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    _collisionDetected = false;
    setState(DriverState::Transmitting);
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    // Bus-monitor: keep a copy of this frame's bytes to print with the BUS TX
    // line once the ISR core reports the frame's raw start/end timers.
    _lastTxFrameLen = std::min(frame.size(), _lastTxFrame.size());
    std::copy_n(frame.data(), _lastTxFrameLen, _lastTxFrame.data());
#endif
    if (!_core.send(frame)) {
        KNX_LOGD(TAG,
                 "TX telegram request rejected by core len=%zu frame=%s",
                 frame.size(),
                 util::formatHexBytes(frame).c_str());
        setState(DriverState::Idle);
        return util::ErrorCode::Busy;
    }

    return util::Result<void>::ok();
}

void BitBangDriverTimerIsrEspIdf::setRxCallback(RxCallback callback, void* context) {
    _rxCallback = callback;
    _rxContext = context;
}

void BitBangDriverTimerIsrEspIdf::setQueueNotifyCallback(QueueNotifyCallback callback, void* context) {
    _queueNotifyCallback = callback;
    _queueNotifyContext = context;
    _core.setQueueNotifyCallback(callback, context);
}

DriverState BitBangDriverTimerIsrEspIdf::getState() const {
    return _state;
}

void BitBangDriverTimerIsrEspIdf::reset() {
    if (!_initialized) {
        return;
    }
    const std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    _collisionDetected = false;
    setState(DriverState::Idle);
}

bool BitBangDriverTimerIsrEspIdf::isMediumIdle() const {
    return _state != DriverState::Transmitting;
}

bool BitBangDriverTimerIsrEspIdf::isCollisionDetected() const {
    return _collisionDetected;
}

void BitBangDriverTimerIsrEspIdf::abortTransmission() {
    const std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    if (_state == DriverState::Transmitting) {
        _collisionDetected = false;
        setState(DriverState::Idle);
    }
}

util::Result<void> BitBangDriverTimerIsrEspIdf::setOwnAddress(uint16_t addressRaw) {
    _ownAddressRaw = addressRaw;
    _core.setOwnAddress(addressRaw);
    KNX_LOGD(TAG, "setOwnAddress 0x%04X → ISR core", addressRaw);
    return util::Result<void>::ok();
}

util::Result<void> BitBangDriverTimerIsrEspIdf::setBusMonitorMode(bool enabled) {
    _busMonitorMode = enabled;
    return util::Result<void>::ok();
}

void BitBangDriverTimerIsrEspIdf::setAckGroupAddresses(std::span<const uint16_t> addresses) {
    _core.setAckGroupAddresses(addresses.data(),
                               static_cast<uint8_t>(std::min<size_t>(addresses.size(), 0xFFu)));
}

void BitBangDriverTimerIsrEspIdf::setLocalBusy(bool busy) {
    _core.setLocalBusy(busy);
}

void BitBangDriverTimerIsrEspIdf::pollTp1() {
    process();
}

bool BitBangDriverTimerIsrEspIdf::popTp1Event(Tp1RxEvent& outEvent) {
    const std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    if (_eventCount == 0) {
        return false;
    }

    outEvent = _eventQueue[_eventHead];
    _eventHead = (_eventHead + 1) % TP1_EVENT_QUEUE_CAPACITY;
    --_eventCount;
    return true;
}

Tp1AckDiagnosticsSnapshot BitBangDriverTimerIsrEspIdf::getTp1AckDiagnostics() const {
    const std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    return _diagnostics;
}

void BitBangDriverTimerIsrEspIdf::process() {
    if (!_initialized) {
        return;
    }

    const std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    BitBangDriverTimerIsr::Message message{};
    while (_core.popMessage(message)) {
        switch (message.type) {
            case BitBangDriverTimerIsr::MessageType::Data: {
                beginActiveFrameIfNeeded();
                if (_activeFrameLength < _activeFrame.size()) {
                    _activeFrame[_activeFrameLength++] = message.data;
                } else if (_activeFrameOverflowCount++ == 0u) {
                    // Only possible if a peer sends a longer telegram than the
                    // medium defines; log once so it cannot pass silently.
                    KNX_LOGE(TAG, "RX telegram exceeds %zu octets — frame truncated",
                             _activeFrame.size());
                }

                Tp1RxEvent event;
                event.type = Tp1RxEventType::DataByte;
                event.dataByte = message.data;
                (void)enqueueTp1Event(event);
                break;
            }

            case BitBangDriverTimerIsr::MessageType::ParityError: {
                beginActiveFrameIfNeeded();
                _activeFrameErrorFlags |= 0x01;
                Tp1RxEvent event;
                event.type = Tp1RxEventType::ReceiveError;
                event.errorFlags = 0x01;
                (void)enqueueTp1Event(event);
                noteRxPartialError(0x01, "RX parity error");
                break;
            }

            case BitBangDriverTimerIsr::MessageType::FramingError: {
                if (_activeFrameLength == 0u) {
                    break;
                }

                if (_activeFrameLength == 1u && isTp1AckByte(_activeFrame[0])) {
                    Tp1RxEvent event;
                    event.type = Tp1RxEventType::RxAckObserved;
                    event.ackClass = tp1AckClassFromByte(_activeFrame[0]);
                    ++_diagnostics.rxAckObservedCount;
                    noteUnsupportedAckByte(_activeFrame[0]);
                    (void)enqueueTp1Event(event);
                } else {
                    _activeFrameErrorFlags |= 0x02;
                    Tp1RxEvent event;
                    event.type = Tp1RxEventType::ReceiveError;
                    event.errorFlags = 0x02;
                    (void)enqueueTp1Event(event);
                    noteRxPartialError(0x02, "RX framing error");
                }
                break;
            }

            case BitBangDriverTimerIsr::MessageType::End: {
                if (_state != DriverState::Transmitting) {
                    setState(DriverState::Idle);
                }

                flushRxPartialErrorSummary();

                if (message.data != 0) {
                    Tp1RxEvent errorEvent;
                    errorEvent.type = Tp1RxEventType::ReceiveError;
                    errorEvent.errorFlags = message.data;
                    (void)enqueueTp1Event(errorEvent);
                }

                Tp1RxEvent endEvent;
                endEvent.type = Tp1RxEventType::TelegramEnd;
                (void)enqueueTp1Event(endEvent);

#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
                // Bus-monitor trace. Raw timer values (µs, free-running
                // monotonic domain) carried BY THIS MESSAGE: t0 = start-bit edge
                // of this telegram, t1 = its telegram-complete instant. Reading
                // them off live driver fields instead produced traces that could
                // not be true (t1 before t0, an 11-byte frame "lasting" 1.4 ms)
                // because the ISR had already moved on to the next event by the
                // time this handler ran. idle = gap from the previous bus
                // event's end. A single-byte reception is an L2 acknowledgement
                // between two other devices.
                if (_activeFrameStarted && _activeFrameLength >= 1) {
                    const uint32_t rxStart = message.t0;
                    const uint32_t rxEnd = message.t1;
                    const int64_t idleBeforeUs = busIdleBefore(rxStart);
                    if (_activeFrameLength == 1u) {
                        const uint8_t b = _activeFrame[0];
                        KNX_LOGW(TAG, "BUS AKRX t0=%" PRIu32 "us idle=%" PRId64 "us byte=0x%02X(%s)",
                                 rxStart, idleBeforeUs, b, busMonAckName(b));
                    } else {
                        KNX_LOGW(TAG, "BUS RX   t0=%" PRIu32 "us t1=%" PRIu32 "us dur=%" PRIu32 "us idle=%" PRId64 "us len=%zu err=0x%02X data=%s",
                                 rxStart, rxEnd, rxEnd - rxStart, idleBeforeUs,
                                 _activeFrameLength, _activeFrameErrorFlags,
                                 util::formatHexBytes(
                                     std::span<const uint8_t>(_activeFrame.data(), _activeFrameLength)).c_str());
                    }
                    _lastBusEndUs = rxEnd;
                }

                {
                    const auto& ts = _core.timingStats();
                    KNX_LOGD(TAG, "ISR jitter:   min=%" PRIu32 " max=%" PRIu32 " avg=%" PRIu32 " us  (n=%" PRIu32 ")",
                             ts.timerJitterUs.minVal, ts.timerJitterUs.maxVal,
                             ts.timerJitterUs.avg(),  ts.timerJitterUs.count);
                    KNX_LOGD(TAG, "ISR onTimer:  min=%" PRIu32 " max=%" PRIu32 " avg=%" PRIu32 " cyc (n=%" PRIu32 ")",
                             ts.onTimerAlarmCycles.minVal, ts.onTimerAlarmCycles.maxVal,
                             ts.onTimerAlarmCycles.avg(),  ts.onTimerAlarmCycles.count);
                    KNX_LOGD(TAG, "ISR onEdge:   min=%" PRIu32 " max=%" PRIu32 " avg=%" PRIu32 " cyc (n=%" PRIu32 ")",
                             ts.onRxEdgeCycles.minVal, ts.onRxEdgeCycles.maxVal,
                             ts.onRxEdgeCycles.avg(),  ts.onRxEdgeCycles.count);
                    KNX_LOGD(TAG, "ISR startBit: min=%" PRIu32 " max=%" PRIu32 " avg=%" PRIu32 " cyc (n=%" PRIu32 ")",
                             ts.processStartBitCycles.minVal, ts.processStartBitCycles.maxVal,
                             ts.processStartBitCycles.avg(),  ts.processStartBitCycles.count);
                    KNX_LOGD(TAG, "ISR rxBit:    min=%" PRIu32 " max=%" PRIu32 " avg=%" PRIu32 " cyc (n=%" PRIu32 ")",
                             ts.processReceiveBitCycles.minVal, ts.processReceiveBitCycles.maxVal,
                             ts.processReceiveBitCycles.avg(),  ts.processReceiveBitCycles.count);
                    _core.resetTimingStats();
                }
#endif

                if (_rxCallback && _activeFrameStarted && _activeFrameLength > 0) {
                    _rxCallback(std::span<const uint8_t>(_activeFrame.data(), _activeFrameLength), _rxContext);
                }

                _activeFrameStarted = false;
                _activeFrameLength = 0;
                _activeFrameErrorFlags = 0;
                break;
            }

            case BitBangDriverTimerIsr::MessageType::TxFrameEnd: {
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
                // Bus-monitor TX line, emitted for EVERY transmitted frame —
                // including the individually addressed ones that go on to wait
                // for an L2 ACK. Those are the whole of an ETS session, and
                // publishing this only on the no-ACK path meant a commissioning
                // trace contained no TX timing at all.
                //
                // maxLate is the number that matters: the driver's own bit
                // sampling tolerates roughly (serialBitTimeUs −
                // bitSamplingOffsetUs) of alarm lateness before it starts
                // mis-assigning bits, and a peer decoding our transmission has a
                // comparable budget. A maxLate approaching that figure means the
                // frame we keyed onto the bus was malformed, which looks
                // identical to a lost acknowledgement from the far end.
                const uint32_t txStart = message.t0;
                const uint32_t txEnd = message.t1;
                KNX_LOGW(TAG, "BUS TX   t0=%" PRIu32 "us t1=%" PRIu32 "us dur=%" PRIu32 "us idle=%" PRId64 "us maxLate=%uus len=%zu data=%s",
                         txStart, txEnd, txEnd - txStart,
                         busIdleBefore(txStart),
                         static_cast<unsigned>(message.data),
                         _lastTxFrameLen,
                         util::formatHexBytes(
                             std::span<const uint8_t>(_lastTxFrame.data(), _lastTxFrameLen)).c_str());
                _lastBusEndUs = txEnd;
#endif
                break;
            }

            case BitBangDriverTimerIsr::MessageType::TxComplete: {
                // Non-ack frame transaction finished; clear Transmitting state so
                // getState() correctly returns Idle for subsequent TX readiness
                // checks. Acknowledged frames reach Idle via TxAckResponse
                // instead — TxFrameEnd above is purely a timing record and must
                // not release the state while the ACK window is still open.
                setState(DriverState::Idle);
                break;
            }

            case BitBangDriverTimerIsr::MessageType::TxAckResponse: {
                if (message.data == BitBangDriverTimerIsr::ACK_BYTE_NONE) {
                    // The counters distinguish the three ways this can happen:
                    // nothing on the bus, an edge whose start bit was rejected,
                    // or a start bit accepted and the character never completed.
                    // rxReject and staleAlarm are whole-driver totals rather
                    // than ACK-window ones, but they belong on this line: they
                    // are the two ways a "the peer sent nothing" verdict could
                    // be the receiver's fault instead of the truth.
                    const auto& lc = _core.linkCounters();
                    KNX_LOGD(TAG,
                             "TX ACK timeout: no usable response byte "
                             "(noEdge=%" PRIu32 " startRejected=%" PRIu32
                             " incomplete=%" PRIu32 " corrupt=%" PRIu32
                             " rxReject=%" PRIu32 " staleAlarm=%" PRIu32 ")",
                             lc.ackTimeoutNoEdge, lc.ackTimeoutStartRejected,
                             lc.ackTimeoutIncomplete, lc.ackCharacterErrors,
                             lc.startValidationRejects, lc.staleTimerAlarms);
                    Tp1RxEvent event;
                    event.type = Tp1RxEventType::TxAckDeadlineMiss;
                    ++_diagnostics.deadlineMissCount;
                    (void)enqueueTp1Event(event);
                } else {
                    KNX_LOGD(TAG,
                             "TX ACK driven on bus ack=0x%02X class=%u",
                             message.data,
                             static_cast<unsigned>(tp1AckClassFromByte(message.data)));
                    Tp1RxEvent event;
                    event.type = Tp1RxEventType::TxAckResponse;
                    event.ackClass = tp1AckClassFromByte(message.data);
                    ++_diagnostics.responseEmittedCount;
                    noteUnsupportedAckByte(message.data);
                    (void)enqueueTp1Event(event);
                }
                setState(DriverState::Idle);
                break;
            }

            case BitBangDriverTimerIsr::MessageType::Collision: {
                _collisionDetected = true;
                ++_collisionEventCount;
                if (_collisionEventCount == 1u || (_collisionEventCount % 64u) == 0u) {
                    KNX_LOGW(TAG, "TX collision detected (%" PRIu32 ")", _collisionEventCount);
                }
                (void)enqueueMediumStateChanged();
                break;
            }

            case BitBangDriverTimerIsr::MessageType::EarlyAckArmed: {
                // Fires for every individually-addressed frame we intend to
                // ACK; the outcome is reported by DlAckDone below instead.
                break;
            }

            case BitBangDriverTimerIsr::MessageType::DlAckSuppressed: {
                KNX_LOGW(TAG, "DL-ACK suppressed: TX busy (txState=%u)", message.data);
                break;
            }

            case BitBangDriverTimerIsr::MessageType::DlAckTxStart: {
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
                // Carries the byte we are about to drive. The DlAckDone that
                // follows carries the timing and the echo result; the ring
                // preserves order and the ISR queues nothing between the two,
                // so pairing them here is exact rather than a live-field read.
                _pendingDlAckByte = message.data;
#endif
                break;
            }

            case BitBangDriverTimerIsr::MessageType::DlAckDone: {
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
                // Bus-monitor: our OWN L2 DL-ACK for a received frame. t0/t1 are
                // this ACK's own start/end instants, byte = ACK/BUSY/NACK we
                // drove, echo=1 if our transmission was seen back on the RX
                // line. Advances _lastBusEndUs so the next frame's idle gap is
                // measured from here.
                KNX_LOGW(TAG, "BUS AKTX t0=%" PRIu32 "us t1=%" PRIu32 "us byte=0x%02X(%s) echo=%u",
                         message.t0, message.t1,
                         _pendingDlAckByte, busMonAckName(_pendingDlAckByte),
                         static_cast<unsigned>(message.data));
                _lastBusEndUs = message.t1;
#endif
                if (message.data == 0) {
                    KNX_LOGW(TAG, "DL-ACK sent but bus echo MISSING — transceiver may not be driving bus");
                }
                break;
            }

            default:
                break;
        }
    }

    const uint32_t droppedNow = _core.droppedMessageCount();
    if (droppedNow >= _observedDroppedCount) {
        _diagnostics.overflowErrorCount += (droppedNow - _observedDroppedCount);
    }
    _observedDroppedCount = droppedNow;

    pollLinkSignal();
}

const char* BitBangDriverTimerIsrEspIdf::getVersion() const {
    return "knstax-timer-isr-espidf-0";
}

bool BitBangDriverTimerIsrEspIdf::enqueueTp1Event(const Tp1RxEvent& event) {
    if (_eventCount >= TP1_EVENT_QUEUE_CAPACITY) {
        ++_diagnostics.overflowErrorCount;
        return false;
    }

    _eventQueue[_eventTail] = event;
    _eventTail = (_eventTail + 1) % TP1_EVENT_QUEUE_CAPACITY;
    ++_eventCount;
    return true;
}

void BitBangDriverTimerIsrEspIdf::setState(DriverState newState) {
    if (_state == newState) {
        return;
    }

    _state = newState;
    (void)enqueueMediumStateChanged();
}

bool BitBangDriverTimerIsrEspIdf::enqueueMediumStateChanged() {
    Tp1RxEvent event;
    event.type = Tp1RxEventType::MediumStateChanged;
    return enqueueTp1Event(event);
}

bool BitBangDriverTimerIsrEspIdf::hasTp1LinkStateIndication() const {
    return _linkSignalActive;
}

LinkState BitBangDriverTimerIsrEspIdf::getTp1LinkState() const {
    return _linkState.load(std::memory_order_relaxed);
}

void BitBangDriverTimerIsrEspIdf::setPowerFailIsrHandler(LinkPowerFailIsrHandler handler, void* context) {
    const std::lock_guard<std::recursive_mutex> lock(_serviceMutex);
    _powerFailContext = context;
    _powerFailHandler = handler;
}

bool BitBangDriverTimerIsrEspIdf::initLinkSignal() {
    _linkSignalActive = false;
    _linkState.store(LinkState::Unknown, std::memory_order_relaxed);
    _linkMonitor.configure(LinkSignalConfig{});

    if (!_config.linkSignal.isConfigured()) {
        return true;  // Nothing wired — not an error.
    }

    if (!knx_timer_gpio_hal_has_status_pin(&_hal)) {
        return false;
    }

    if (!knx_timer_gpio_hal_configure_status_pin(&_hal,
                                                 static_cast<int>(_config.linkSignal.pin),
                                                 _config.linkSignal.enablePullup)) {
        return false;
    }

#if defined(ESP_PLATFORM)
    esp_timer_create_args_t repollArgs{};
    repollArgs.callback = &BitBangDriverTimerIsrEspIdf::linkRepollTimerCb;
    repollArgs.arg = this;
    repollArgs.dispatch_method = ESP_TIMER_TASK;
    repollArgs.name = "knx_link_repoll";
    esp_timer_handle_t repollTimer = nullptr;
    if (esp_timer_create(&repollArgs, &repollTimer) != ESP_OK) {
        return false;
    }
    _linkRepollTimer = repollTimer;
#endif

    _linkMonitor.configure(_config.linkSignal);

    // Seed from the current level before arming the interrupt, so the first
    // reported state is the real one rather than a transition at first poll.
    pollLinkSignal();

    if (!knx_timer_gpio_hal_install_status_edge_isr(&_hal, linkSignalEdgeIsr, this)) {
        closeLinkSignal();
        return false;
    }

    _linkSignalActive = true;
    KNX_LOGD(TAG,
             "Link-health signal on GPIO%u (active %s, debounce down=%" PRIu32 "us up=%" PRIu32 "us): %s",
             static_cast<unsigned>(_config.linkSignal.pin),
             _config.linkSignal.activeHigh ? "high" : "low",
             _config.linkSignal.downDebounceUs,
             _config.linkSignal.upDebounceUs,
             linkStateToString(_linkState.load(std::memory_order_relaxed)));
    return true;
}

void BitBangDriverTimerIsrEspIdf::closeLinkSignal() {
    if (_linkSignalActive) {
        knx_timer_gpio_hal_remove_status_edge_isr(&_hal);
        _linkSignalActive = false;
    }

#if defined(ESP_PLATFORM)
    if (_linkRepollTimer) {
        auto* timer = static_cast<esp_timer_handle_t>(_linkRepollTimer);
        (void)esp_timer_stop(timer);
        (void)esp_timer_delete(timer);
        _linkRepollTimer = nullptr;
    }
#endif

    _linkMonitor.configure(LinkSignalConfig{});
    _linkState.store(LinkState::Unknown, std::memory_order_relaxed);
}

// Ask to be sampled again once the pending debounce window has elapsed. The
// notify path is the same one the ISR uses, so the extra sample happens on the
// worker that already owns process().
void BitBangDriverTimerIsrEspIdf::scheduleLinkRepoll(uint64_t remainingUs) {
#if defined(ESP_PLATFORM)
    if (!_linkRepollTimer) {
        return;
    }

    auto* timer = static_cast<esp_timer_handle_t>(_linkRepollTimer);
    (void)esp_timer_stop(timer);

    if (remainingUs == 0) {
        return;
    }

    // A little slack so the sample lands after the window rather than on it.
    (void)esp_timer_start_once(timer, remainingUs + 500u);
#else
    (void)remainingUs;
#endif
}

void BitBangDriverTimerIsrEspIdf::linkRepollTimerCb(void* arg) {
    auto* self = static_cast<BitBangDriverTimerIsrEspIdf*>(arg);
    if (self && self->_queueNotifyCallback) {
        self->_queueNotifyCallback(self->_queueNotifyContext);
    }
}

void BitBangDriverTimerIsrEspIdf::pollLinkSignal() {
    if (!_linkMonitor.isConfigured()) {
        return;
    }

    const int rawLevel = knx_timer_gpio_hal_read_status_level_fast(&_hal);
    const uint64_t nowUs = knx_timer_gpio_hal_timer_now_us(&_hal);

    LinkStatus status{};
    const bool changed = _linkMonitor.sample(_linkMonitor.levelMeansUp(rawLevel), nowUs, status);

    // Either a change is now waiting out its window (schedule the sample that
    // will decide it) or none is (cancel any timer left from a filtered dip).
    scheduleLinkRepoll(_linkMonitor.pendingRemainingUs(nowUs));

    if (!changed) {
        return;
    }

    _linkState.store(status.state, std::memory_order_relaxed);

    if (status.state == LinkState::Down) {
        KNX_LOGW(TAG, "KNX bus power lost (link Down, transition %" PRIu32 ")", status.transitionCount);
    } else {
        KNX_LOGD(TAG, "KNX bus power present (link %s, transition %" PRIu32 ")",
                 linkStateToString(status.state), status.transitionCount);
    }

    Tp1RxEvent event;
    event.type = Tp1RxEventType::LinkStateChanged;
    event.linkStatus = status;
    (void)enqueueTp1Event(event);
}

// Interrupt context. Deliberately minimal: the debounced decision is made in
// pollLinkSignal() from task context. This only fires the optional pre-debounce
// power-fail hook and nudges the consumer so the debounce window is polled out
// promptly even when the bus has gone silent and no RX activity would wake it.
void KNX_TIMER_GPIO_HAL_ISR_ATTR BitBangDriverTimerIsrEspIdf::linkSignalEdgeIsr(void* context) {
    auto* self = static_cast<BitBangDriverTimerIsrEspIdf*>(context);
    if (!self) {
        return;
    }

    if (self->_config.linkSignal.notifyPowerFailFromIsr && self->_powerFailHandler) {
        const int rawLevel = knx_timer_gpio_hal_read_status_level_fast(&self->_hal);
        const bool up = (rawLevel != 0) == self->_config.linkSignal.activeHigh;
        if (!up) {
            self->_powerFailHandler(self->_powerFailContext,
                                    knx_timer_gpio_hal_timer_now_us(&self->_hal));
        }
    }

    if (self->_queueNotifyCallback) {
        self->_queueNotifyCallback(self->_queueNotifyContext);
    }
}

void BitBangDriverTimerIsrEspIdf::noteRxPartialError(uint8_t errorFlag, const char* label) {
    const size_t partialLen = std::min(_activeFrameLength, _lastRxErrorPartial.size());
    const auto partialSpan = std::span<const uint8_t>(_activeFrame.data(), partialLen);

    const bool sameAsLast = (_lastRxErrorFlag == errorFlag)
        && (_lastRxErrorPartialLength == partialLen)
        && (partialLen == 0u || std::equal(partialSpan.begin(), partialSpan.end(), _lastRxErrorPartial.begin()));

    if (sameAsLast) {
        ++_lastRxErrorRepeatCount;
        if ((_lastRxErrorRepeatCount % 64u) == 0u) {
            KNX_LOGD(TAG,
                     "%s partial=%s repeated=%" PRIu32,
                     label,
                     util::formatHexBytes(partialSpan).c_str(),
                     _lastRxErrorRepeatCount);
        }
        return;
    }

    flushRxPartialErrorSummary();

    _lastRxErrorFlag = errorFlag;
    _lastRxErrorPartialLength = partialLen;
    _lastRxErrorRepeatCount = 0u;
    if (partialLen > 0u) {
        std::copy(partialSpan.begin(), partialSpan.end(), _lastRxErrorPartial.begin());
    }

    KNX_LOGD(TAG,
             "%s partial=%s",
             label,
             util::formatHexBytes(partialSpan).c_str());
}

void BitBangDriverTimerIsrEspIdf::flushRxPartialErrorSummary() {
    if (_lastRxErrorRepeatCount == 0u) {
        return;
    }

    const char* label = (_lastRxErrorFlag == 0x01u) ? "RX parity error"
                      : (_lastRxErrorFlag == 0x02u) ? "RX framing error"
                                                    : "RX error";
    const auto partialSpan = std::span<const uint8_t>(_lastRxErrorPartial.data(), _lastRxErrorPartialLength);
    KNX_LOGD(TAG,
             "%s partial=%s repeated=%" PRIu32,
             label,
             util::formatHexBytes(partialSpan).c_str(),
             _lastRxErrorRepeatCount);
    _lastRxErrorRepeatCount = 0u;
}

void BitBangDriverTimerIsrEspIdf::noteUnsupportedAckByte(uint8_t ackByte) {
    if (!isTp1AckByte(ackByte)) {
        ++_diagnostics.unsupportedRawIngressCount;
    }
}

void BitBangDriverTimerIsrEspIdf::beginActiveFrameIfNeeded() {
    if (_activeFrameStarted) {
        return;
    }

    _activeFrameStarted = true;
    _activeFrameLength = 0;
    _activeFrameErrorFlags = 0;

    if (_state != DriverState::Transmitting) {
        setState(DriverState::Receiving);
    }

    Tp1RxEvent event;
    event.type = Tp1RxEventType::TelegramStart;
    (void)enqueueTp1Event(event);
}

int64_t BitBangDriverTimerIsrEspIdf::busIdleBefore(uint32_t startUs) const {
    if (_lastBusEndUs == 0u) {
        return -1;
    }
    return static_cast<int64_t>(startUs - _lastBusEndUs);
}

} // namespace physical
} // namespace knx
