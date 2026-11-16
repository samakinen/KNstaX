// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

// TP1 bit-bang ISR core. The state machine, invariants, schedule and event
// contract are specified in docs/reference/tp1_bitbang_isr.md; section
// numbers below (§n) refer to it.

#include "knx/physical/bitbang_driver_timer_isr.hpp"
#include "knx/datalink/tp1_dl_common.hpp"

#include <cstring>

#if KNX_TP1_BITBANG_SELF_CHECK
#include <cstdio>
#include <cstdlib>
#endif

namespace knx {
namespace physical {

namespace {

inline constexpr bool isDataBitPosition(int8_t position) noexcept
{
    return static_cast<unsigned>(position) <= 7u;
}

#if KNX_TP1_BITBANG_SELF_CHECK
[[noreturn]] void selfCheckFailed(const char* what, const char* where)
{
    std::fprintf(stderr, "TP1 bit-bang ISR self-check failed in %s: %s\n", where, what);
    std::fflush(stderr);
    std::abort();
}
#endif

} // namespace

template<typename IsrPolicy>
bool BitBangDriverTimerIsrT<IsrPolicy>::init(const knx_timer_gpio_hal_t& hal,
                                             const BitBangConfig& config)
{
    if (!knx_timer_gpio_hal_is_valid(&hal)) {
        return false;
    }
    // The schedule needs a sample point after the start-bit re-read and a
    // DL-ACK delay that is positive (§4).
    const uint32_t bitUs = config.serialBitTimeUs;
    if (bitUs == 0u
            || config.bitSamplingOffsetUs <= config.startBitValidationTimeUs
            || config.bitSamplingOffsetUs >= bitUs
                   * (ACK_START_FROM_FRAME_END_BT - TELEGRAM_COMPLETE_FROM_FRAME_END_BT)
            || config.reactionLatencyUs >= bitUs) {
        return false;
    }

    _hal = hal;
    _config = config;
    _rxDominantLevel = config.rxDominantHigh ? 1u : 0u;

    _bitUs = bitUs;
    _sampleOffsetUs = config.bitSamplingOffsetUs;
    _startValidationUs = config.startBitValidationTimeUs;
    _zeroActiveUs = config.zeroActiveTimeUs;
    _zeroEqualizationUs = config.zeroEqualizationTimeUs;
    _waitMoreDataUs = bitUs * 3u;
    _staleCharGapUs = bitUs * STALE_CHARACTER_GAP_BT;
    _ackWindowUs = bitUs * (1u + ACK_WINDOW_BT);
    _ackSlotDelayUs = bitUs * (ACK_START_FROM_FRAME_END_BT - TELEGRAM_COMPLETE_FROM_FRAME_END_BT)
                      - config.bitSamplingOffsetUs;
    _busyHoldoffUs = bitUs * (CHARACTER_BT + BUSY_HOLDOFF_BT);
    _txEarlyEdgeUs = bitUs / TX_EARLY_EDGE_DIVISOR;
    _reactionLatencyUs = config.reactionLatencyUs;
    _firstSampleDelayUs = config.bitSamplingOffsetUs - config.startBitValidationTimeUs + bitUs;
    _txIdleWaitUs = 0;

    _msgHead.store(0, std::memory_order_relaxed);
    _msgTail.store(0, std::memory_order_relaxed);
    _msgDropped.store(0, std::memory_order_relaxed);
    _armedAlarmOwner = AlarmOwner::None;
    _armedAlarmUs = 0;
    _txAlarmUs = 0;
    _rxAlarmUs = 0;
    _rxEdgeUs = 0;
    _lastCharStartUs = 0;
    _lastCharStartIsOwn = false;
    _txHoldoffUntilUs = 0;
    _txSlotDueUs = 0;
    _txState = TX_IDLE;
    _rxState = RX_IDLE;
    _rxZeroDetected = 0;
    _rxByte = 0;
    _rxBitPosition = -1;
    _rxErrors = 0;
    _rxByteCount = 0;
    _rxCheckXor = 0;
    _rxDstHi = 0;
    _rxDstLo = 0;
    _rxExtendedFrame = false;
    _rxExtGroupAddressed = false;
    _rxTelegramOpen = false;
    _rxAckIntent = 0;
    _ackSlotByte = 0;
    _sendingAck = false;
    _txCurrentByte = 0;
    _txBytePosition = 0;
    _txTelegramLength = 0;
    _txBitPosition = -1;
    _txArbitrationLosses = 0;
    _dlAckEchoSeen = false;
    _ackStartEdgeSeen = false;
    _ackStartValidated = false;
    // Re-applied by the owner after init (MAC queue depth, monitor mode); a
    // stale value must not survive a close/init cycle.
    _localBusy = false;
    _busMonitor = false;
    _linkCounters.reset();
#if KNX_TP1_BITBANG_SELF_CHECK
    _selfCheck = SelfCheckState{};
#endif

    const auto rxEdge = _config.rxDominantHigh
                            ? KNX_TIMER_GPIO_HAL_RX_EDGE_RISING
                            : KNX_TIMER_GPIO_HAL_RX_EDGE_FALLING;

    if (!knx_timer_gpio_hal_configure_pins(&_hal,
                                           static_cast<int>(_config.txPin),
                                           static_cast<int>(_config.rxPin),
                                           _config.enablePullup,
                                           rxEdge)) {
        return false;
    }

    if (!knx_timer_gpio_hal_install_rx_edge_isr(&_hal, &BitBangDriverTimerIsrT<IsrPolicy>::rxEdgeShim, this)) {
        return false;
    }

    if (!knx_timer_gpio_hal_start_timer(&_hal, &BitBangDriverTimerIsrT<IsrPolicy>::timerAlarmShim, this)) {
        knx_timer_gpio_hal_remove_rx_edge_isr(&_hal);
        return false;
    }

    // Extract the handles the hot path uses directly, once.
    _isrHal.attach(_hal, _config.txDominantHigh);

    setTxRecessiveFast();
    _initialized = true;
    return true;
}

template<typename IsrPolicy>
void BitBangDriverTimerIsrT<IsrPolicy>::shutdown()
{
    if (!_initialized) {
        return;
    }

    knx_timer_gpio_hal_stop_timer(&_hal);
    knx_timer_gpio_hal_remove_rx_edge_isr(&_hal);
    _initialized = false;
}

template<typename IsrPolicy>
bool BitBangDriverTimerIsrT<IsrPolicy>::send(std::span<const uint8_t> frame)
{
    if (!_initialized || frame.empty() || frame.size() > MAX_TELEGRAM_SIZE) {
        return false;
    }

    // Everything below writes ISR-owned state, so the driver's interrupts are
    // masked for the whole check-and-stage sequence. Edges and alarms that
    // occur meanwhile are latched by the hardware and serviced on release.
    _isrHal.lockFromTask();

    // A frame may be staged behind a DL-ACK that is armed or on the wire: the
    // ACK goes first, then the frame takes the normal bus-idle wait (§3.3).
    // Otherwise the transmitter must be idle.
    const bool behindAck = _sendingAck && _txTelegramLength == 0u;
    if (_txState != TX_IDLE && !behindAck) {
        _isrHal.unlockFromTask();
        return false;
    }

    std::memcpy(_txBuffer.data(), frame.data(), frame.size());
    _txTelegramLength = static_cast<uint16_t>(frame.size());
    _txArbitrationLosses = 0;

    // Bus access (03_02_02 §2.3): start only after the bus has been idle for
    // t_idle, measured here from the start bit of the last character on the
    // bus — 11 bits for that character plus the idle time. CTRL[3:2] is the
    // priority (00 system, 01 normal, 10 urgent, 11 low): system and urgent
    // frames wait the 50-bit floor, normal and low 53. A repeated frame (CTRL
    // bit 5 cleared) is in the 50-bit class whatever its priority, so a
    // repetition goes out ahead of new normal and low traffic. No margin is
    // added: it would hand the bus to other devices' lower-priority frames.
    static constexpr uint8_t kTxWaitBitsByPriority[4] = {61u, 64u, 61u, 64u};
    const bool repeated = (frame[0] & datalink::CTRL_REPEAT) == 0u;
    _txIdleWaitUs = _bitUs * (repeated ? uint32_t{CHARACTER_BT + BUS_IDLE_FLOOR_BT}
                                       : uint32_t{kTxWaitBitsByPriority[(frame[0] >> 2u) & 0x03u]});

    if (!behindAck) {
        // First CSMA evaluation one bit time from now. Behind a DL-ACK the
        // TX alarm already holds the ACK's t_ack schedule and must stay.
        _txState = TX_WAITING_SLOT;
        _txAlarmUs = timerNow() + _bitUs;
        rearmTimer();
    }

    selfCheckSendAccepted();
    selfCheckState("send");
    _isrHal.unlockFromTask();
    return true;
}

template<typename IsrPolicy>
bool BitBangDriverTimerIsrT<IsrPolicy>::popMessage(Message& outMessage)
{
    const uint32_t head = _msgHead.load(std::memory_order_relaxed);
    if (head == _msgTail.load(std::memory_order_acquire)) {
        return false;
    }

    outMessage = _messages[head & MESSAGE_QUEUE_MASK];
    _msgHead.store(head + 1u, std::memory_order_release);
    return true;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::onTimerAlarm()
{
    if (!_initialized) {
        return;
    }

    BITBANG_TIMING_BEGIN(onTimerAlarm);

    // Consume the programmed-alarm record: this compare has fired and is no
    // longer armed; rearmTimer() at the end decides what replaces it.
    const AlarmOwner owner = _armedAlarmOwner;
    const uint64_t firedAlarmUs = _armedAlarmUs;
    _armedAlarmOwner = AlarmOwner::None;
    _armedAlarmUs = 0;

    // Service the deadline only if it is still the one that was programmed
    // (§4.3); otherwise this is a compare that survived a reprogram.
    const bool current = owner == AlarmOwner::Tx ? _txAlarmUs == firedAlarmUs
                       : owner == AlarmOwner::Rx ? _rxAlarmUs == firedAlarmUs
                       : false;
    if (!current) [[unlikely]] {
        ++_linkCounters.staleTimerAlarms;
    } else {
        BITBANG_TIMING_RECORD(_timingStats.timerJitterUs, timerNow() - firedAlarmUs);
        // Every delay a handler returns is added to firedAlarmUs, the
        // scheduled instant of this alarm, never to the current time: the
        // schedule stays locked to the start-bit edge and does not accumulate
        // interrupt latency (§4.1).
        if (owner == AlarmOwner::Tx) {
            diagTxBitLateness(firedAlarmUs);
            _txAlarmUs = 0;
            const uint64_t nextDelayUs = processTxTimer(firedAlarmUs);
            if (nextDelayUs != 0u) {
                _txAlarmUs = firedAlarmUs + nextDelayUs;
            }
        } else {
            _rxAlarmUs = 0;
            const uint64_t nextDelayUs = processRxTimer();
            if (nextDelayUs != 0u) {
                // An RX handler that leaves the receiver idle and returns a
                // delay has handed the timeline to the DL-ACK slot (§3.3).
                if (_rxState == RX_IDLE) {
                    _txAlarmUs = firedAlarmUs + nextDelayUs;
                } else {
                    _rxAlarmUs = firedAlarmUs + nextDelayUs;
                }
            }
        }
    }

    rearmTimer();
    selfCheckState("onTimerAlarm");
    BITBANG_TIMING_END(_timingStats.onTimerAlarmCycles, onTimerAlarm);
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::onRxEdge()
{
    if (!_initialized) {
        return;
    }

    BITBANG_TIMING_BEGIN(onRxEdge);

    _rxZeroDetected = 1;

    if (isDrivingBits()) {
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
        // The echo of a frame's start bit: the reaction latency of this
        // board, measured (§4.4).
        if (_txState == TX_SENDING_ZERO_ACTIVE && _txBitPosition == 0 && !_sendingAck) {
            BITBANG_TIMING_RECORD(_timingStats.txEchoDelayUs, timerNow() - _lastCharStartUs);
        }
#endif
        const int8_t lostPosition = txEdgeLostPosition();
        if (lostPosition == TX_EDGE_NOT_LOST) {
            // The local echo of a bit this node drives, or the same 0 from a
            // sender that is still level with it.
            BITBANG_TIMING_END(_timingStats.onRxEdgeCycles, onRxEdge);
            return;
        }
        // Another sender's 0 where this node sends a 1: it won arbitration.
        // If that happened inside a character the receiver has taken the
        // character over; otherwise the edge is handled below as the start
        // bit of a character.
        loseArbitration(lostPosition);
        if (_rxState != RX_IDLE) {
            rearmTimer();
            selfCheckState("onRxEdge/arbitration");
            BITBANG_TIMING_END(_timingStats.onRxEdgeCycles, onRxEdge);
            return;
        }
    }

    if (_txState == TX_WAITING_ACK && _rxState == RX_IDLE) {
        // Separates "the peer never answered" from "the peer answered and
        // start-bit validation rejected it" when the window times out.
        _ackStartEdgeSeen = true;
    }

    if (_rxState == RX_IDLE || _rxState == RX_WAIT_MORE_DATA) {
        const uint64_t nowUs = timerNow();
        // In RX_WAIT_MORE_DATA the edge is normally the next character. Long
        // after the previous one it can only be the next telegram, and the
        // telegram-complete deadline was missed: close the old telegram first
        // so the two are not concatenated. Its t_ack slot has passed, so its
        // acknowledge intent goes with it.
        if (_rxState == RX_WAIT_MORE_DATA && (nowUs - _lastCharStartUs) > _staleCharGapUs) {
            finishRxTelegram();
        }
        if (!_rxTelegramOpen) {
            diagRxFrameStart(nowUs);
        }
        _rxEdgeUs = nowUs;
        _rxState = RX_START_BIT_PENDING;
        _rxAlarmUs = nowUs + _startValidationUs;
        rearmTimer();
    }
    // In RX_START_BIT_PENDING and RX_RECEIVE the only effect is
    // _rxZeroDetected, so repeated edges (reflections) are harmless.

    selfCheckState("onRxEdge");
    BITBANG_TIMING_END(_timingStats.onRxEdgeCycles, onRxEdge);
}

template<typename IsrPolicy>
uint32_t BitBangDriverTimerIsrT<IsrPolicy>::droppedMessageCount() const noexcept
{
    return _msgDropped.load(std::memory_order_relaxed);
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR KNX_TIMER_GPIO_HAL_ISR_NOCLONE_ATTR
bool BitBangDriverTimerIsrT<IsrPolicy>::queueMessage(
    MessageType type, uint8_t data, uint64_t t0Us, uint64_t t1Us) noexcept
{
    selfCheckMessage(type);

    // Producer side of the SPSC ring (ISR context only; the GPIO and timer
    // interrupts share one priority and never nest).
    const uint32_t tail = _msgTail.load(std::memory_order_relaxed);
    const uint32_t head = _msgHead.load(std::memory_order_acquire);
    if (tail - head >= MESSAGE_QUEUE_CAPACITY) [[unlikely]] {
        // No wake-up for a drop: it would be a context switch out of the
        // bit-timing interrupt to report that nothing was published.
        _msgDropped.store(_msgDropped.load(std::memory_order_relaxed) + 1u,
                          std::memory_order_relaxed);
        return false;
    }

    Message& slot = _messages[tail & MESSAGE_QUEUE_MASK];
    slot.type = type;
    slot.data = data;
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    slot.t0 = static_cast<uint32_t>(t0Us);
    slot.t1 = static_cast<uint32_t>(t1Us);
#else
    (void)t0Us;
    (void)t1Us;
#endif
    _msgTail.store(tail + 1u, std::memory_order_release);

    // `head` predates the publish, so the depth is a lower bound: the
    // watermark can wake early, never late.
    if (_queueNotifyCallback != nullptr
            && (messageNeedsNotify(type)
                || (tail + 1u - head) >= MESSAGE_QUEUE_NOTIFY_WATERMARK)) {
        _queueNotifyCallback(_queueNotifyContext);
    }

    return true;
}

// Program the hardware compare with the earlier of the two logical deadlines
// and record which one it is (§4.3). On a tie RX wins: a bit sample cannot be
// deferred without losing the bit, a TX transition only moves by one ISR.
//
// HAL contract: rearmTimerAbs() replaces the compare target and discards any
// pending compare of the old target. Where a HAL cannot guarantee that, the
// owner/deadline check in onTimerAlarm() rejects the survivor and counts it
// in linkCounters().staleTimerAlarms.
template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::rearmTimer()
{
    AlarmOwner owner = AlarmOwner::None;
    uint64_t deadlineUs = 0;

    if (_rxAlarmUs != 0 && (_txAlarmUs == 0 || _rxAlarmUs <= _txAlarmUs)) {
        owner = AlarmOwner::Rx;
        deadlineUs = _rxAlarmUs;
    } else if (_txAlarmUs != 0) {
        owner = AlarmOwner::Tx;
        deadlineUs = _txAlarmUs;
    }

    _armedAlarmOwner = owner;
    _armedAlarmUs = deadlineUs;

    if (owner != AlarmOwner::None) {
        rearmTimerAbs(deadlineUs);
    }
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::processRxTimer()
{
    switch (_rxState) {
        case RX_RECEIVE: {
            BITBANG_TIMING_BEGIN(processReceiveBit);
            const uint64_t result = processReceiveBit();
            BITBANG_TIMING_END(_timingStats.processReceiveBitCycles, processReceiveBit);
            return result;
        }
        case RX_START_BIT_PENDING: {
            BITBANG_TIMING_BEGIN(processStartBit);
            const uint64_t result = processStartBit();
            BITBANG_TIMING_END(_timingStats.processStartBitCycles, processStartBit);
            return result;
        }
        case RX_WAIT_MORE_DATA:
            return rxHandleTelegramComplete();
        case RX_IDLE:
        default:
            return 0;
    }
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::processStartBit()
{
    // Runs startBitValidationTimeUs after the edge; a real start bit is still
    // dominant. This is the check with the least margin in the driver: the
    // dominant pulse is only zeroActiveTimeUs (35 µs) wide, so edge-ISR entry
    // latency plus alarm lateness must stay below the difference.
    if (!rxIsDominant()) {
        if (_txState == TX_WAITING_ACK) {
            ++_linkCounters.ackStartValidationRejects;
        } else {
            ++_linkCounters.startValidationRejects;
        }
        _rxZeroDetected = 0;
        if (_rxTelegramOpen) {
            // A spike between two characters of a telegram: keep waiting for
            // the next character under the previous one's telegram-complete
            // deadline (§3.1).
            _rxState = RX_WAIT_MORE_DATA;
            const uint64_t completeAtUs = _lastCharStartUs + _sampleOffsetUs
                                          + uint64_t{_bitUs} * (CHARACTER_BT - 1u)
                                          + _waitMoreDataUs;
            const uint64_t nowUs = _rxEdgeUs + _startValidationUs;
            return completeAtUs > nowUs ? completeAtUs - nowUs : 1u;
        }
        _rxState = RX_IDLE;
        return 0;
    }

    // A character starts. Its edge becomes the bus's last character start
    // (the CSMA reference), and its bit cells are timed from it.
    _lastCharStartUs = _rxEdgeUs;
    _lastCharStartIsOwn = false;
    // Clear the edge flag now, before the D0 cell begins: the next thing the
    // sampler looks at is the D0 edge.
    _rxZeroDetected = 0;
    _rxByte = 0;
    _rxBitPosition = 0;
    _rxState = RX_RECEIVE;
    if (_txState == TX_WAITING_ACK) {
        _ackStartValidated = true;
    }
    // The start bit is confirmed, so its own cell needs no sample. The next
    // alarm is the D0 sample at edge + bitSamplingOffsetUs + one bit; this
    // alarm fired at edge + startBitValidationTimeUs.
    return _firstSampleDelayUs;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::processReceiveBit()
{
    const int8_t position = _rxBitPosition;
    const uint8_t bit = _rxZeroDetected ? 0 : 1;
    _rxZeroDetected = 0;

    if (isDataBitPosition(position)) {
        _rxByte = static_cast<uint8_t>(_rxByte | (bit << position));
        _rxBitPosition = static_cast<int8_t>(position + 1);
        return _bitUs;
    }

    if (position == 8) {
        if (bit != parityBit(_rxByte)) {
            _rxErrors |= RX_ERROR_PARITY;
            // Inside an ACK window the error belongs to the ACK transaction;
            // finishReceivedAck() reports it.
            if (_txState != TX_WAITING_ACK) {
                (void)queueMessage(MessageType::ParityError, 0);
                _rxTelegramOpen = true;
            }
        }
        _rxBitPosition = 9;
        return _bitUs;
    }

    return rxHandleStop(bit);
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::rxHandleStop(uint8_t bit)
{
    if (bit != 1) {
        _rxErrors |= RX_ERROR_FRAMING;
    }

    if (_txState == TX_WAITING_ACK) {
        // The character is the acknowledgement of the frame just sent. (An
        // acknowledgement between two other devices is not: no window is
        // open, and it decodes below as a one-octet telegram.)
        const uint8_t ackByte = finishReceivedAck();
        _txState = TX_IDLE;
        _txAlarmUs = 0;
        if (ackByte == ACK_BYTE_BUSY || ackByte == ACK_BYTE_NACK_BUSY) {
            // The repetition waits at least 150 bit times after the BUSY
            // character, on top of the usual bus-idle wait (03_02_02 A.8).
            _txHoldoffUntilUs = _lastCharStartUs + _busyHoldoffUs - referenceLatencyUs();
        }
        (void)queueMessage(MessageType::TxAckResponse, ackByte, diagNowUs());
        return 0;
    }

    if (bit == 1) {
        (void)queueMessage(MessageType::Data, _rxByte);
        rxAcceptOctet(_rxByte);
    } else {
        (void)queueMessage(MessageType::FramingError, 0);
    }
    _rxTelegramOpen = true;
    _rxState = RX_WAIT_MORE_DATA;
    return _waitMoreDataUs;
}

// Fold one octet of the telegram into the running check and the header
// decoder, and take the acknowledge decision when the destination is known.
//
// Standard frames: CTRL SA SA DA DA NPCI ... — address type in NPCI (pos 5).
// Extended frames: CTRL CTRLE SA SA DA DA LEN ... — address type in CTRLE.
template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::rxAcceptOctet(uint8_t octet)
{
    _rxCheckXor ^= octet;

    const uint16_t index = _rxByteCount;
    if (index == datalink::CTRL_FIELD_POS) {
        _rxExtendedFrame = (octet & 0xC0u) == 0x00u;
    } else if (_rxExtendedFrame) {
        switch (index) {
            case 1:
                _rxExtGroupAddressed = (octet & 0x80u) != 0u;
                break;
            case datalink::DEST_ADDR_HI_POS + 1:
                _rxDstHi = octet;
                break;
            case datalink::DEST_ADDR_LO_POS + 1:
                _rxDstLo = octet;
                armAckIfAddressed(_rxExtGroupAddressed);
                break;
            default:
                break;
        }
    } else {
        switch (index) {
            case datalink::DEST_ADDR_HI_POS:
                _rxDstHi = octet;
                break;
            case datalink::DEST_ADDR_LO_POS:
                _rxDstLo = octet;
                break;
            case datalink::LENGTH_POS:
                armAckIfAddressed((octet & datalink::DEST_ADDR_TYPE) != 0u);
                break;
            default:
                break;
        }
    }
    _rxByteCount = static_cast<uint16_t>(index + 1u);
}

// Close the open telegram: publish End and reset all per-telegram state and
// the sampler.
template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::finishRxTelegram(uint8_t endFlags)
{
    const uint8_t rxErrors = _rxErrors;
    _rxErrors = 0;
    _rxByteCount = 0;
    _rxCheckXor = 0;
    _rxDstHi = 0;
    _rxDstLo = 0;
    _rxExtendedFrame = false;
    _rxExtGroupAddressed = false;
    _rxAckIntent = 0;
    _rxTelegramOpen = false;
    resetRxSamplingState();
    (void)queueMessage(MessageType::End, static_cast<uint8_t>(rxErrors | endFlags),
                       diagRxFrameStartUs(), diagNowUs());
}

// Stop receiving whatever is in progress because this node is about to drive
// the bus, or just did: a character overlapping its own would decode as the
// wired-AND of the two, and its own echo leaves the sampler dirty. An open
// telegram is closed with End so the consumer never sees a telegram that
// does not end.
template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::abandonReception()
{
    if (_rxTelegramOpen) {
        finishRxTelegram();
        return;
    }
    resetRxSamplingState();
    _rxErrors = 0;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::rxHandleTelegramComplete()
{
    // The acknowledge decision was latched while the header streamed in; an
    // addressed device answers an incorrect telegram with NAK (03_02_02
    // §2.4.2) — plain NAK even when busy, as the spec answers incorrect
    // frames.
    uint8_t ackByte = _rxAckIntent;
    if (ackByte != 0u && (_rxErrors != 0u || !rxFrameIsCorrect())) {
        ackByte = ACK_BYTE_NACK;
    }
    const bool slotFree = _txState == TX_IDLE
                       || (_txState == TX_WAITING_SLOT && !_sendingAck);

    // The sender repeats a frame answered with BUSY, so End tells the
    // consumer not to deliver this copy.
    const uint8_t endFlags = (slotFree && ackByte == ACK_BYTE_BUSY)
                                 ? END_FLAG_ANSWERED_BUSY
                                 : uint8_t{0};
    finishRxTelegram(endFlags);

    if (ackByte == 0u) {
        return 0;
    }
    if (!slotFree) {
        (void)queueMessage(MessageType::DlAckSuppressed, _txState);
        return 0;
    }

    // Arm the DL-ACK slot. The returned delay is added by onTimerAlarm() to
    // this alarm's scheduled instant, so the ACK starts t_ack after the frame
    // end without picking up interrupt jitter (§4.2). The frame was timed
    // from another device's edges, so the constant reaction latency comes
    // off (§4.4). A frame staged in TX_WAITING_SLOT keeps its place and
    // follows the ACK.
    _ackSlotByte = ackByte;
    _sendingAck = true;
    _txState = TX_WAITING_SLOT;
    return _ackSlotDelayUs - referenceLatencyUs();
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::sendBit(uint8_t bit)
{
    if (bit != 0) {
        _txState = TX_SENDING_ONE;
        return _bitUs;
    }

    setTxDominantFast();
    _txState = TX_SENDING_ZERO_ACTIVE;
    return _zeroActiveUs;
}

// Runs at the end of every transmitted bit (and once to start a character)
// and returns the delay to the next bit-end.
template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::processEndOfTxBit(uint64_t nowUs)
{
    const int8_t bitPosition = _txBitPosition;
    if (isDataBitPosition(bitPosition)) {
        _txBitPosition = static_cast<int8_t>(bitPosition + 1);
        return sendBit(static_cast<uint8_t>((_txCurrentByte >> bitPosition) & 0x1u));
    }

    switch (bitPosition) {
        case -1:
            // Start bit. Its scheduled instant becomes the bus's last
            // character start, the reference the receive path also uses.
            _lastCharStartUs = nowUs;
            _lastCharStartIsOwn = true;
            _txBitPosition = 0;
            return sendBit(0);
        case 8:
            _txBitPosition = 9;
            return sendBit(parityBit(_txCurrentByte));
        case 9:
            break;
        default:
            return 0;
    }

    // End of the parity bit: the stop bit begins. It is recessive and not
    // driven.
    if (_sendingAck) {
        (void)queueMessage(MessageType::DlAckDone, _dlAckEchoSeen ? 1u : 0u,
                           diagDlAckStartUs(), diagNowUs());
        _sendingAck = false;
        _ackSlotByte = 0;
        abandonReception();
        if (_txTelegramLength > 0u) {
            // A frame staged behind the ACK now takes the normal bus-idle wait.
            _txState = TX_WAITING_SLOT;
            return _bitUs;
        }
        _txState = TX_IDLE;
        return 0;
    }

    ++_txBytePosition;
    _txBitPosition = -1;
    if (_txBytePosition < _txTelegramLength) {
        // Stop bit and inter-character gap: released, and a dominant edge
        // here is a collision like in any recessive bit.
        _txCurrentByte = _txBuffer[_txBytePosition];
        _txState = TX_SENDING_ONE;
        return uint64_t{_bitUs} * (1u + INTER_CHARACTER_GAP_BT);
    }

    // The frame is on the wire once its last stop bit completes, whether or
    // not an ACK window follows.
    const bool ackExpected = txFrameExpectsAck();
    _txTelegramLength = 0;
    diagQueueTxFrameEnd(diagNowUs());
    if (ackExpected) {
        // The window opens now, at the start of the stop bit; the extra bit
        // time in _ackWindowUs makes it close 35 bit times after the frame end.
        openAckWindow();
        return _ackWindowUs;
    }
    abandonReception();
    _txState = TX_IDLE;
    (void)queueMessage(MessageType::TxComplete, 0);
    return 0;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::txHandleWaitingSlot(uint64_t nowUs)
{
    if (_sendingAck) {
        // The DL-ACK slot. Acknowledges of other addressed devices may already
        // have started; ours superimposes on theirs, and what the receiver
        // decoded of theirs is meaningless now.
        abandonReception();
        _txSlotDueUs = 0;
        _txCurrentByte = _ackSlotByte;
        _dlAckEchoSeen = false;
        _txBitPosition = -1;
        diagDlAckStart(diagNowUs());
        (void)queueMessage(MessageType::DlAckTxStart, _txCurrentByte);
        return processEndOfTxBit(nowUs);
    }

    // Nonzero when this alarm is the instant the frame was found due at.
    const uint64_t dueUs = _txSlotDueUs;
    _txSlotDueUs = 0;

    if (_rxState != RX_IDLE) {
        // CSMA: never start while a character is being received. A long
        // telegram refreshes _lastCharStartUs only once per character, so the
        // elapsed-time test alone could pass in the middle of it. Poll once
        // per bit until the receiver is idle again.
        //
        // The exception is a start bit that began as this frame came due:
        // another sender of the same access class, at most a fraction of a
        // bit ahead. Waiting would give it the bus without arbitration, so
        // this node sends its start bit too and lets the frames arbitrate
        // (§3.5). What the receiver saw of that start bit is dropped; if the
        // other sender wins, the takeover of §3.4 receives its frame.
        if (nowUs != dueUs || !startBitJustBegan(nowUs)) {
            return _bitUs;
        }
        resetRxSamplingState();
    } else {
        // The idle time runs from the last character on the bus. When that
        // was another device's, the reaction latency comes off (§4.4).
        uint64_t earliestStartUs = _lastCharStartUs + _txIdleWaitUs - referenceLatencyUs();
        if (earliestStartUs < _txHoldoffUntilUs) {
            earliestStartUs = _txHoldoffUntilUs;
        }
        if (nowUs < earliestStartUs) {
            _txSlotDueUs = earliestStartUs;
            return earliestStartUs - nowUs;
        }
    }

    _txBitPosition = -1;
    _txBytePosition = 0;
    _txCurrentByte = _txBuffer[0];
    diagTxFrameStart(nowUs);
    return processEndOfTxBit(nowUs);
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::txHandleWaitingAck()
{
    // Classify the timeout while the evidence is still this transaction's
    // state: a silent peer and a discarded acknowledgement have opposite fixes.
    if (!_ackStartEdgeSeen) {
        ++_linkCounters.ackTimeoutNoEdge;
    } else if (!_ackStartValidated) {
        ++_linkCounters.ackTimeoutStartRejected;
    } else {
        ++_linkCounters.ackTimeoutIncomplete;
    }

    _ackStartEdgeSeen = false;
    _ackStartValidated = false;
    // A character that started late is still worth finishing: once the window
    // is closed it completes down the ordinary receive path. Only a sampler
    // with nothing in flight is reset.
    if (_rxState == RX_IDLE) {
        resetRxSamplingState();
        _rxErrors = 0;
    }
    (void)queueMessage(MessageType::TxAckResponse, ACK_BYTE_NONE);
    _txState = TX_IDLE;
    return 0;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::processTxTimer(uint64_t nowUs)
{
    switch (_txState) {
        case TX_SENDING_ZERO_ACTIVE:
            if (_sendingAck && rxIsDominant()) {
                // The local echo of the DL-ACK's dominant bit: proof the
                // transceiver drives the bus. Read before releasing.
                _dlAckEchoSeen = true;
            }
            setTxRecessiveFast();
            _txState = TX_SENDING_ZERO_EQUALIZATION;
            return _zeroEqualizationUs;

        case TX_SENDING_ONE:
        case TX_SENDING_ZERO_EQUALIZATION:
            // The line is already released.
            return processEndOfTxBit(nowUs);

        case TX_WAITING_SLOT:
            return txHandleWaitingSlot(nowUs);

        case TX_WAITING_ACK:
            return txHandleWaitingAck();

        case TX_IDLE:
        default:
            return 0;
    }
}

// Place a dominant edge seen while this node drives bits on its own bit grid
// and decide whether it means arbitration is lost (§3.4). Returns the bit
// position that was lost (0..7 data, 8 parity), TX_EDGE_OUTSIDE_CHARACTER for
// a loss in the stop bit or the gap, or TX_EDGE_NOT_LOST.
//
// The state alone does not say which bit the edge belongs to. A competitor is
// never level with this node to the microsecond, and this interrupt can run
// before the alarm that ends the current cell even when the edge came after
// it. So an edge in the last part of a cell, or after the cell's alarm is
// due, is the next cell's: it is a loss only if that bit is a 1.
template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR int8_t BitBangDriverTimerIsrT<IsrPolicy>::txEdgeLostPosition() const
{
    // Acknowledges superimpose and are not arbitrated. In the dominant part
    // of a 0 the edge is this node's own echo.
    if (_sendingAck || _txState == TX_SENDING_ZERO_ACTIVE) {
        return TX_EDGE_NOT_LOST;
    }

    // _txBitPosition names the cell that follows the one on the wire: -1 the
    // next character's start bit, 0..7 data, 8 parity, 9 the stop bit. The TX
    // alarm is the end of the cell on the wire.
    const int8_t nextPosition = _txBitPosition;
    const int64_t remainingUs = static_cast<int64_t>(_txAlarmUs - timerNow());
    if (remainingUs <= static_cast<int64_t>(_txEarlyEdgeUs) && nextPosition != 9) {
        if (nextPosition < 0) {
            return TX_EDGE_NOT_LOST;  // the start bit both senders send
        }
        const uint8_t nextBit = nextPosition == 8
                                    ? parityBit(_txCurrentByte)
                                    : static_cast<uint8_t>((_txCurrentByte >> nextPosition) & 0x1u);
        return nextBit != 0u ? nextPosition : TX_EDGE_NOT_LOST;
    }

    if (_txState != TX_SENDING_ONE) {
        return TX_EDGE_NOT_LOST;  // late echo or ringing in a 0 this node sent
    }
    // The 1 on the wire: a data or parity bit, or the stop bit and the gap
    // (nextPosition -1), where no synchronised sender sends anything.
    return nextPosition >= 1 ? static_cast<int8_t>(nextPosition - 1) : TX_EDGE_OUTSIDE_CHARACTER;
}

// Arbitration lost (03_02_02 §2.3): the edge is another sender's dominant bit
// where this node sends a recessive one, at @p lostPosition of the current
// character (see txEdgeLostPosition()). The line is released: the cell on
// the wire is a 1 or the recessive rest of a 0.
template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::loseArbitration(int8_t lostPosition)
{
    (void)queueMessage(MessageType::Collision, 0);
    takeOverReceptionAfterArbitrationLoss(lostPosition);

    // Losing arbitration is how TP1 shares the bus, not an error: the frame
    // goes back to waiting for the bus, behind the winner's frame and its
    // acknowledge. The TX alarm armed for the end of the cell on the wire
    // stays and lands in txHandleWaitingSlot(), which polls until the
    // receiver is idle.
    ++_txArbitrationLosses;
    if (_txArbitrationLosses < MAX_TX_ARBITRATION_ATTEMPTS) {
        _txBytePosition = 0;
        _txBitPosition = -1;
        _txState = TX_WAITING_SLOT;
        return;
    }

    // A bus that wins every time is jammed or shorted: report the failure.
    _txTelegramLength = 0;
    _txBytePosition = 0;
    _txBitPosition = -1;
    _txAlarmUs = 0;
    _txState = TX_IDLE;
    (void)queueMessage(MessageType::TxArbitrationLost, _txArbitrationLosses);
}

// After losing arbitration inside a character, receive the winner's frame
// from where it diverged: up to that bit the wired-AND bus carried exactly
// what this node sent, so the octets already sent and the bits of the current
// character are the winner's too (§3.4).
template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::takeOverReceptionAfterArbitrationLoss(
    int8_t lostPosition)
{
    // Outside a character (stop bit or gap) nothing sent so far belongs to
    // the other sender's character, and the edge is a start bit.
    if (lostPosition < 0 || lostPosition > 8) {
        return;
    }
    const uint8_t bitOnWire = static_cast<uint8_t>(lostPosition);

    diagRxFrameStart(_lastCharStartUs);
    for (uint16_t i = 0; i < _txBytePosition; ++i) {
        (void)queueMessage(MessageType::Data, _txBuffer[i]);
        rxAcceptOctet(_txBuffer[i]);
        _rxTelegramOpen = true;
    }

    _rxByte = bitOnWire >= 8u
                  ? _txCurrentByte
                  : static_cast<uint8_t>(_txCurrentByte & ((1u << bitOnWire) - 1u));
    // _rxZeroDetected, set by this edge, makes the bit sampled next a 0. The
    // sample point is the middle of the lost bit's cell, so it lies ahead
    // whether the edge came early or late in that cell.
    _rxBitPosition = static_cast<int8_t>(bitOnWire);
    _rxState = RX_RECEIVE;
    _rxAlarmUs = _lastCharStartUs + _sampleOffsetUs + uint64_t{_bitUs} * (bitOnWire + 1u);
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::timerAlarmShim(void* context)
{
    static_cast<BitBangDriverTimerIsrT<IsrPolicy>*>(context)->onTimerAlarm();
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR void BitBangDriverTimerIsrT<IsrPolicy>::rxEdgeShim(void* context)
{
    static_cast<BitBangDriverTimerIsrT<IsrPolicy>*>(context)->onRxEdge();
}

// ── Self-check (host builds) ─────────────────────────────────────────────────
#if KNX_TP1_BITBANG_SELF_CHECK

// The state invariants of §5, evaluated whenever an interrupt or send()
// returns.
template<typename IsrPolicy>
void BitBangDriverTimerIsrT<IsrPolicy>::selfCheckState(const char* where) const noexcept
{
    if (!_selfCheck.enabled || !_initialized) {
        return;
    }
    const auto require = [where](bool condition, const char* what) {
        if (!condition) {
            selfCheckFailed(what, where);
        }
    };

    // I1: the hardware compare holds the earliest pending deadline.
    AlarmOwner owner = AlarmOwner::None;
    uint64_t deadlineUs = 0;
    if (_rxAlarmUs != 0 && (_txAlarmUs == 0 || _rxAlarmUs <= _txAlarmUs)) {
        owner = AlarmOwner::Rx;
        deadlineUs = _rxAlarmUs;
    } else if (_txAlarmUs != 0) {
        owner = AlarmOwner::Tx;
        deadlineUs = _txAlarmUs;
    }
    require(owner == _armedAlarmOwner && deadlineUs == _armedAlarmUs,
            "I1 the armed alarm is not the earliest pending deadline");

    // I2, I3: a timeline has an alarm exactly when it is not idle.
    require((_rxState == RX_IDLE) == (_rxAlarmUs == 0), "I2 RX alarm does not match RX state");
    require((_txState == TX_IDLE) == (_txAlarmUs == 0), "I3 TX alarm does not match TX state");

    // I4–I8: the transmitter's composite modes (§3.3).
    require(_sendingAck == (_ackSlotByte != 0), "I4 DL-ACK flag and slot octet disagree");
    if (_sendingAck) {
        require(_txState != TX_IDLE && _txState != TX_WAITING_ACK,
                "I5 DL-ACK pending in a state that cannot send it");
    }
    if (_txState == TX_IDLE || _txState == TX_WAITING_ACK) {
        require(_txTelegramLength == 0, "I6 a frame is staged while idle or awaiting an ACK");
    }
    if (_txState == TX_WAITING_SLOT) {
        require(_sendingAck || _txTelegramLength > 0, "I7 waiting for the bus with nothing to send");
    }
    if (isDrivingBits() && !_sendingAck) {
        require(_txTelegramLength > 0, "I8 sending bits of no frame");
    }

    // I9–I11: the receiver's telegram bookkeeping (§3.1).
    require(_rxAckIntent == 0 || _rxTelegramOpen, "I9 acknowledge intent outside a telegram");
    require(!_rxTelegramOpen || _rxState != RX_IDLE, "I10 telegram open with the receiver idle");
    if (_rxState == RX_IDLE) {
        require(_rxByteCount == 0 && _rxErrors == 0, "I11 idle receiver holds telegram state");
    }

    // I12–I14: the state agrees with the event contract (§6).
    require(_selfCheck.sendOutstanding == (_txTelegramLength > 0 || _txState == TX_WAITING_ACK),
            "I12 accepted send() and staged frame disagree");
    require(_selfCheck.telegramOpen == _rxTelegramOpen, "I13 End owed and telegram state disagree");
    require(_selfCheck.dlAckInFlight == (_sendingAck && isDrivingBits()),
            "I14 DL-ACK events and DL-ACK transmission disagree");
}

// The event contract of §6, checked as each message is produced.
template<typename IsrPolicy>
void BitBangDriverTimerIsrT<IsrPolicy>::selfCheckMessage(MessageType type) noexcept
{
    if (!_selfCheck.enabled) {
        return;
    }
    SelfCheckState& s = _selfCheck;
    const auto require = [](bool condition, const char* what) {
        if (!condition) {
            selfCheckFailed(what, "queueMessage");
        }
    };

    require(!s.dlAckInFlight || type == MessageType::DlAckDone,
            "E1 message between DlAckTxStart and DlAckDone");

    switch (type) {
        case MessageType::Data:
        case MessageType::ParityError:
        case MessageType::FramingError:
            s.telegramOpen = true;
            break;
        case MessageType::End:
            require(s.telegramOpen, "E2 End without an open telegram");
            s.telegramOpen = false;
            s.ackArmedInTelegram = false;
            break;
        case MessageType::EarlyAckArmed:
            require(s.telegramOpen && !s.ackArmedInTelegram,
                    "E3 EarlyAckArmed outside a telegram or twice in one");
            s.ackArmedInTelegram = true;
            break;
        case MessageType::TxComplete:
        case MessageType::TxAckResponse:
        case MessageType::TxArbitrationLost:
            require(s.sendOutstanding, "E4 terminal TX event without an accepted send()");
            s.sendOutstanding = false;
            break;
        case MessageType::Collision:
        case MessageType::TxFrameEnd:
            require(s.sendOutstanding, "E5 TX event without an accepted send()");
            break;
        case MessageType::DlAckTxStart:
            require(!s.dlAckInFlight, "E6 nested DlAckTxStart");
            s.dlAckInFlight = true;
            break;
        case MessageType::DlAckDone:
            require(s.dlAckInFlight, "E7 DlAckDone without DlAckTxStart");
            s.dlAckInFlight = false;
            break;
        case MessageType::DlAckSuppressed:
            require(!s.telegramOpen, "E8 DlAckSuppressed inside a telegram");
            break;
    }
}

template<typename IsrPolicy>
void BitBangDriverTimerIsrT<IsrPolicy>::selfCheckSendAccepted() noexcept
{
    if (!_selfCheck.enabled) {
        return;
    }
    if (_selfCheck.sendOutstanding) {
        selfCheckFailed("E9 send() accepted while the previous frame has no terminal event", "send");
    }
    _selfCheck.sendOutstanding = true;
}

#endif // KNX_TP1_BITBANG_SELF_CHECK

} // namespace physical
} // namespace knx

// Explicit instantiation for the platform ISR policy selected by the alias in
// bitbang_driver_timer_isr.hpp.
#ifdef ESP_PLATFORM
template class knx::physical::BitBangDriverTimerIsrT<knx::physical::EspIdfIsrHalPolicy>;
#else
template class knx::physical::BitBangDriverTimerIsrT<knx::physical::VirtualIsrHalPolicy>;
#endif
