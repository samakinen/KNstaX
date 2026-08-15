// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/bitbang_driver_timer_isr.hpp"
#include "knx/datalink/tp1_dl_common.hpp"

#include <cstring>

namespace knx {
namespace physical {

namespace {

inline constexpr bool isDataBitPosition(int8_t position) noexcept
{
    return static_cast<unsigned>(position) <= 7u;
}

} // namespace

template<typename IsrPolicy>
bool BitBangDriverTimerIsrT<IsrPolicy>::init(const knx_timer_gpio_hal_t& hal,
                                             const BitBangConfig& config)
{
    if (!knx_timer_gpio_hal_is_valid(&hal)) {
        return false;
    }

    _hal = hal;
    _config = config;
    _rxDominantLevel = config.rxDominantHigh ? 1u : 0u;
    _msgHead.store(0, std::memory_order_relaxed);
    _msgTail.store(0, std::memory_order_relaxed);
    _msgDropped.store(0, std::memory_order_relaxed);
    _txAlarmUs = 0;
    _rxAlarmUs = 0;
    _lastCharStartUs = 0;
    _flags = 0;
    _rxZeroDetected = 0;
    _txState = TX_IDLE;
    _rxState = RX_IDLE;
    _txCurrentByte = 0;
    _pendingAckByte = 0;
    _txBytePosition = 0;
    _txTelegramLength = 0;
    _txBitPosition = -1;
    _rxByte = 0;
    _rxBitPosition = -1;
    _rxErrors = 0;
    _txWaitTime = 0;
    _rxByteCount = 0;
    _rxDstHi = 0;
    _rxDstLo = 0;
    _dlAckEchoSeen = false;
    _dlAckRxErrors = 0;
    _ackStartEdgeSeen = false;
    _ackStartValidated = false;
    _linkCounters.reset();

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

    // Attach the ISR hardware policy — extracts all needed handles from _hal.context
    // once at init time. The ISR hot path then uses _isrHal directly with no vtable
    // indirection or null-check overhead.
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

    // Everything below mutates ISR-owned state (_txState, _txAlarmUs, the TX
    // buffer) from task context. Mask the driver's ISRs for the whole
    // check-and-arm sequence: without this, a telegram completing between the
    // busy check and the alarm write can arm the DL-ACK slot concurrently and
    // have its 15-bit t_ack alarm silently overwritten. The section runs a few
    // µs (bounded by the ≤263-byte copy); pending edges/alarms are latched in
    // hardware and serviced right after release.
    _isrHal.lockFromTask();

    // A frame may be queued behind an armed-but-idle DL-ACK slot: the slot
    // alarm transmits the ACK first and then falls through to the normal
    // bus-idle wait for this frame (txHandleWaitingSlot). Every other busy
    // state still rejects. This avoids a busy/retry round-trip for the
    // response the task sends right after each received request.
    const bool ackSlotArmedIdle = _txState == TX_WAITING_SLOT
        && (_flags & FLAG_SENDING_ACK) != 0u
        && _txTelegramLength == 0u;
    if (_txState > TX_IDLE && !ackSlotArmedIdle) {
        _isrHal.unlockFromTask();
        return false;
    }

    std::memcpy(_txBuffer.data(), frame.data(), frame.size());
    _txTelegramLength = static_cast<uint16_t>(frame.size());
    _txBytePosition = 0;
    _txBitPosition = -1;

    // KNX TP1 bus access (03.02.02): a transmitter may start only after the bus
    // has been idle for t_idle (spec floor 50 bit-times, 53 for normal/low; the
    // 3-bit reduction gives system/urgent frames and repeats earlier access).
    // _lastCharStartUs is the start bit of the last character on the bus — set
    // uniformly by both the RX path (onRxEdge) and our own TX (processEndOfTxBit
    // case -1), so the same reference covers received frames, our transmissions,
    // and a remote L2-ACK of our frame. The wait is therefore 11 bits (that
    // character incl. stop bit) + the idle window (floor + margin, see below).
    // This also keeps us clear of the 15-bit DL-ACK slot of the exchange that
    // just ended. Simultaneous starts with another sender resolve via the normal
    // TP1 bitwise arbitration (dominant '0' wins, we detect collision + retry).
    // CTRL[3:2] priority wire codes (03_02_02 §2.3.2): 00=System, 01=Normal,
    // 10=Urgent, 11=Low. System and urgent start after the 50-bit-time floor,
    // non-repeated normal and low after 53 — kept spec-tight on purpose: adding
    // margin here delays our high-priority frames vs other devices' lower-
    // priority ones, and bus-monitor traces proved extra idle does NOT fix the
    // dropped-response problem (byte-identical responses at identical idle were
    // received in one connection and dropped in the next), so the cause is not
    // the t_idle magnitude.
    static constexpr uint8_t kTxWaitBitsByPriority[4] = {61u, 64u, 61u, 64u};
    _txWaitTime = kTxWaitBitsByPriority[(frame[0] >> 2u) & 0x03u];

    _txState = TX_WAITING_SLOT;
    // Do NOT rearm the TX alarm when staging a frame behind an armed DL-ACK
    // slot: _txAlarmUs already holds the ACK's start-bit-locked t_ack schedule
    // (firedAlarmUs + telegramCompleteToAckStartDelayUs, set by the RX→TX
    // handoff in onTimerAlarm). Overwriting it with now + 1 bit-time fires the
    // ACK ~1 bit-time after this call instead of at 15 BT after frame end —
    // certified couplers then ignore the too-early ACK and retransmit. The
    // queued frame is transmitted after the ACK finishes (processEndOfTxBit
    // case 9 → TX_WAITING_SLOT → the normal CSMA bus-idle wait). For a fresh
    // send (no ACK slot armed) we arm the wait to start on the next tick.
    if (!ackSlotArmedIdle) {
        const uint64_t nowUs = timerNow();
        _txAlarmUs = nowUs + bitTimeUs();
    }
    rearmTimer();
    _isrHal.unlockFromTask();
    return true;
}

template<typename IsrPolicy>
bool BitBangDriverTimerIsrT<IsrPolicy>::popMessage(Message& outMessage)
{
    // Consumer side of the SPSC ring (task context only).
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

    // Consume the programmed-alarm record up front. Whatever happens below, this
    // compare has fired and is no longer armed; rearmTimer() at the end decides
    // what replaces it.
    const AlarmOwner owner = _armedAlarmOwner;
    const uint64_t firedAlarmUs = _armedAlarmUs;
    _armedAlarmOwner = AlarmOwner::None;
    _armedAlarmUs = 0;

    if (owner == AlarmOwner::None) {
        // Nothing was armed, so this is a stale compare left over from an alarm
        // that was reprogrammed after the hardware latched it. Servicing it
        // would run a state handler with a deadline nobody scheduled.
        ++_linkCounters.staleTimerAlarms;
        rearmTimer();
        BITBANG_TIMING_END(_timingStats.onTimerAlarmCycles, onTimerAlarm);
        return;
    }

    BITBANG_TIMING_RECORD(_timingStats.timerJitterUs, timerNow() - firedAlarmUs);

    // This is the ONE place that turns a state handler's returned delay into a
    // scheduled alarm. Handlers only change state and return a cumulative offset
    // (>0); they never write _txAlarmUs/_rxAlarmUs themselves. The offset is
    // always added to firedAlarmUs (the scheduled fire time of THIS alarm), never
    // timerNow(), so the whole schedule stays locked to the start-bit edge and
    // never accumulates per-fire ISR-entry latency.
    if (owner == AlarmOwner::Tx) {
        if (_txAlarmUs != firedAlarmUs) {
            // The TX deadline moved after this compare was programmed. The
            // replacement is still pending in _txAlarmUs, so leave it alone.
            ++_linkCounters.staleTimerAlarms;
            rearmTimer();
            BITBANG_TIMING_END(_timingStats.onTimerAlarmCycles, onTimerAlarm);
            return;
        }
        // While actively driving bits, record how late this alarm fired vs its
        // schedule (excessive lateness = malformed TX edges). Compiled out unless
        // timing diagnostics are enabled — see diagTxBitLateness.
        diagTxBitLateness(firedAlarmUs);
        _txAlarmUs = 0;
        const uint64_t nextDelayUs = processTxTimer();
        _txAlarmUs = (nextDelayUs > 0) ? (firedAlarmUs + nextDelayUs) : 0;
    } else {
        if (_rxAlarmUs != firedAlarmUs) {
            ++_linkCounters.staleTimerAlarms;
            rearmTimer();
            BITBANG_TIMING_END(_timingStats.onTimerAlarmCycles, onTimerAlarm);
            return;
        }
        _rxAlarmUs = 0;
        const uint64_t nextDelayUs = processRxTimer();
        if (nextDelayUs > 0) {
            // processRxTimer may hand the timeline off to the DL-ACK send window:
            // on telegram-complete it finishes RX (→ RX_IDLE), arms the TX ACK
            // slot (→ TX_WAITING_SLOT) and returns the ACK's cumulative offset.
            // Route the offset to whichever timeline now owns it — every alarm
            // write stays in this function.
            if (_rxState == RX_IDLE && _txState == TX_WAITING_SLOT) {
                _txAlarmUs = firedAlarmUs + nextDelayUs;
            } else {
                _rxAlarmUs = firedAlarmUs + nextDelayUs;
            }
        }
    }

    rearmTimer();
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
    (void)detectCollision();

    const bool activelyDrivingBus = _txState == TX_SENDING_ZERO_ACTIVE
                                 || _txState == TX_SENDING_ZERO_EQUALIZATION
                                 || _txState == TX_SENDING_ONE;
    if (activelyDrivingBus) {
        BITBANG_TIMING_END(_timingStats.onRxEdgeCycles, onRxEdge);
        return;
    }

    // An edge inside the ACK window, before any character has been accepted:
    // this is what separates "the peer never answered" from "the peer answered
    // and start-bit validation rejected it" when the window later times out.
    if (_txState == TX_WAITING_ACK && _rxState == RX_IDLE) {
        _ackStartEdgeSeen = true;
    }

    if (_rxState == RX_IDLE || _rxState == RX_WAIT_MORE_DATA) {
        const uint64_t nowUs = timerNow();
        // An RX_WAIT_MORE_DATA edge is normally the next byte of an in-progress
        // frame. But if it arrives long after the previous byte, the telegram-
        // complete timeout was lost (alarm starved or clobbered) and this edge
        // is really the NEXT frame's start bit. Finalize the stuck telegram now
        // — otherwise the new frame's bytes append to the old buffer (observed
        // once as two frames concatenated into one 17-byte buffer). The frame's
        // t_ack slot has long passed, so any latched ACK intent is dropped.
        if (_rxState == RX_WAIT_MORE_DATA
                && (nowUs - _lastCharStartUs) > rxInterByteStaleUs()) {
            (void)finishRxTelegram();
            _pendingAckByte = 0;
        }
        // A new telegram begins only when we were idle; stamp its start once.
        if (_rxState == RX_IDLE) {
            diagRxFrameStart(nowUs);
            // Bind the latched DL-ACK intent to THIS telegram. Every normal
            // path consumes _pendingAckByte, but an abnormal one (arbitration
            // loss cancelling the armed slot) can leave it set, and a stale
            // intent would acknowledge a frame addressed to somebody else.
            // Clearing it where a telegram begins makes that impossible by
            // construction, with no generation counter to keep in step.
            _pendingAckByte = 0;
        }
        _lastCharStartUs = nowUs;
        _rxState = RX_START_BIT_PENDING;
        _rxAlarmUs = nowUs + _config.startBitValidationTimeUs;
        rearmTimer();
    }
    // In all other states (RX_START_BIT_PENDING, RX_RECEIVE), setting
    // _rxZeroDetected=1 is the only effect. Fully idempotent for reflections.

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
    // Producer side of the SPSC ring (ISR context only; ISRs never nest here —
    // GPIO and timer run at the same interrupt priority). Plain load/store
    // atomics compile to single lw/sw instructions on the RV32 target.
    const uint32_t tail = _msgTail.load(std::memory_order_relaxed);
    const uint32_t head = _msgHead.load(std::memory_order_acquire);
    if (tail - head >= MESSAGE_QUEUE_CAPACITY) {
        // A dropped message must NOT notify: waking the consumer to tell it
        // nothing was published is a pure context switch out of the bit-timing
        // ISR, and overflow is exactly when the system can least afford one.
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

    // Wake the consumer for anything that completes a unit of work, and for a
    // per-byte message only once the ring is half full. `head` is a snapshot
    // taken before publishing, so the depth used here is a lower bound on the
    // real depth — it can wake early, never late, which is the safe direction.
    if (_queueNotifyCallback != nullptr
            && (messageNeedsNotify(type)
                || (tail + 1u - head) >= MESSAGE_QUEUE_NOTIFY_WATERMARK)) {
        _queueNotifyCallback(_queueNotifyContext);
    }

    return true;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint8_t BitBangDriverTimerIsrT<IsrPolicy>::calculateParityBit(uint8_t byte) noexcept
{
    return static_cast<uint8_t>(__builtin_popcount(byte) & 0x1);
}

// Selects the earlier of the two logical deadlines and records what the
// hardware compare is being programmed with, immediately before programming it.
//
// On an equal deadline RX wins: a bit sample cannot be deferred without losing
// the bit, whereas a TX state transition only shifts the edge it is about to
// drive by one ISR.
//
// HAL contract this relies on: rearmTimerAbs() must replace the compare target
// AND discard any pending compare condition belonging to the old target. Where
// that cannot be guaranteed, the owner/deadline check in onTimerAlarm() rejects
// the survivor instead of servicing it under the new owner, and counts it in
// linkCounters().staleTimerAlarms.
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
        case RX_IDLE:
            return 0;
        case RX_START_BIT_PENDING: {
            BITBANG_TIMING_BEGIN(processStartBit);
            const uint64_t result = processStartBit();
            BITBANG_TIMING_END(_timingStats.processStartBitCycles, processStartBit);
            return result;
        }
        case RX_RECEIVE: {
            BITBANG_TIMING_BEGIN(processReceiveBit);
            const uint64_t result = processReceiveBit();
            BITBANG_TIMING_END(_timingStats.processReceiveBitCycles, processReceiveBit);
            return result;
        }
        case RX_WAIT_MORE_DATA:
            return rxHandleTelegramComplete();
        default:
            return 0;
    }
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::processStartBit()
{
    // Timer fires at syncPointUs + startBitValidationTimeUs.
    // Bus must still be dominant to confirm this is a real start bit.
    const bool busIsDominant = _isrHal.readRxLevel() == static_cast<int>(_rxDominantLevel);

    if (!busIsDominant) {
        // Bus is recessive at validation time — treat as a glitch and discard.
        // If a late ISR caused us to miss a genuine start bit, the higher layer
        // will request retransmission.
        //
        // This is also the failure mode with the least margin in the whole
        // driver: the re-read happens startBitValidationTimeUs after the edge
        // ISR ran, and the dominant pulse is only zeroActiveTimeUs (35 µs)
        // wide, so GPIO-ISR entry latency plus timer-alarm lateness must stay
        // under the difference. Counting rejections separates "the peer sent
        // nothing" from "we threw away what the peer sent".
        if (_txState == TX_WAITING_ACK) {
            ++_linkCounters.ackStartValidationRejects;
        } else {
            ++_linkCounters.startValidationRejects;
        }
        _rxZeroDetected = 0;
        _rxState = RX_IDLE;
        return 0;
    }

    // Valid start bit confirmed. Prepare to receive the byte.
    // Clear _rxZeroDetected NOW (at t=startBitValidationTimeUs, safely before the
    // D0 window at t=bitTimeUs) so that position -1 never has to touch the flag.
    // If the position-1 ISR fires late (after D0's edge), the D0 evidence survives.
    _rxZeroDetected = 0;
    _rxByte = 0;
    _rxBitPosition = -1;
    _rxState = RX_RECEIVE;
    if (_txState == TX_WAITING_ACK) {
        _ackStartValidated = true;
    }
    // Schedule the start-bit sample so it lands at syncPointUs + bitSamplingOffsetUs.
    // onTimerAlarm() computes: firedAlarmUs + return_value
    //   = (syncPointUs + startBitValidationTimeUs) + (bitSamplingOffsetUs - startBitValidationTimeUs)
    //   = syncPointUs + bitSamplingOffsetUs
    return _config.bitSamplingOffsetUs - _config.startBitValidationTimeUs;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::processReceiveBit()
{
    const int8_t position = _rxBitPosition;

    // Position -1 is the start-bit slot: the bit value is unused (the start bit
    // was already confirmed by processStartBit), and _rxZeroDetected must NOT be
    // cleared here. processStartBit() cleared it at t=startBitValidationTimeUs;
    // any dominant edge since then (including D0 firing before this late ISR)
    // must survive intact for the position-0 sample.
    if (position == -1) {
        _rxBitPosition = 0;
        return bitTimeUs();
    }

    const uint8_t bit = _rxZeroDetected ? 0 : 1;
    _rxZeroDetected = 0;

    if (isDataBitPosition(position)) {
        _rxByte = static_cast<uint8_t>(_rxByte | ((bit & 0x1u) << position));
        _rxBitPosition = static_cast<int8_t>(position + 1);
        return bitTimeUs();
    }

    switch (position) {
        case 8:
            if (bit != calculateParityBit(_rxByte)) {
                _rxErrors |= RX_ERROR_PARITY;
                // An error inside an ACK window belongs to the ACK transaction,
                // not to a received telegram. Publishing it would make the
                // consumer attribute it to whichever frame it is assembling;
                // finishReceivedAck() reports it as part of the ACK outcome.
                if (_txState != TX_WAITING_ACK) {
                    (void)queueMessage(MessageType::ParityError, 0);
                }
            }
            _rxBitPosition = 9;
            return bitTimeUs();
        case 9:
            return rxHandleStop(bit);
        default:
            return 0;
    }
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::rxHandleStop(uint8_t bit)
{
    const bool receivingAck = _txState == TX_WAITING_ACK;

    if (bit != 1) {
        _rxErrors |= RX_ERROR_FRAMING;
        // See the parity case: an ACK-window error is reported by
        // finishReceivedAck(), never as a receive error against a telegram.
        if (!receivingAck) {
            (void)queueMessage(MessageType::FramingError, 0);
        }
    }

    if (receivingAck) {
        // finishReceivedAck() consumes the whole ACK transaction: it resets the
        // byte decoder AND _rxErrors, and maps a corrupted character to
        // ACK_BYTE_NONE.
        //
        // An acknowledgement between two OTHER devices does not come through
        // here: no window is open, so it decodes down the ordinary receive path
        // as a one-byte telegram and reaches the consumer as Data + End.
        const uint8_t ackByte = finishReceivedAck();
        _txState = TX_IDLE;
        _txAlarmUs = 0;
        (void)queueMessage(MessageType::TxAckResponse, ackByte, diagNowUs());
        return 0;
    }

    if (bit == 1) {
        (void)queueMessage(MessageType::Data, _rxByte);

        // ISR early DL-ACK: the ACK decision is made HERE, byte-synchronously,
        // as the header streams in — the only place that can reliably meet the
        // 15-bit-time t_ack deadline. _pendingAckByte holds the intent; the
        // DL-ACK slot alarm fires the final byte (downgrading to NACK on RX
        // errors). KNX TP1 acknowledgement rules:
        //   - individually-addressed frames matching our address: always L_ACK
        //     (regardless of the CTRL A bit); individual broadcast (0xFFFF) is
        //     never acknowledged.
        //   - group-addressed frames: L_ACK when the sender requests it (CTRL
        //     A bit) and the group address is in our subscription table;
        //     group broadcast (0x0000) is never acknowledged.
        // Standard frames carry the address type in the length octet (pos 5);
        // L_Data_Extended frames carry it in CTRLE (pos 1) and shift the
        // destination by one octet.
        if (_rxByteCount == datalink::CTRL_FIELD_POS) {
            _rxExtendedFrame = (_rxByte & 0xC0) == 0x00;
        } else if (_rxExtendedFrame) {
            switch (_rxByteCount) {
                case 1:
                    _rxExtGroupAddressed = (_rxByte & 0x80) != 0;
                    break;
                case datalink::DEST_ADDR_HI_POS + 1:
                    _rxDstHi = _rxByte;
                    break;
                case datalink::DEST_ADDR_LO_POS + 1:
                    _rxDstLo = _rxByte;
                    armEarlyAckIfAddressed(_rxExtGroupAddressed);
                    break;
                default:
                    break;
            }
        } else {
            switch (_rxByteCount) {
                case datalink::DEST_ADDR_HI_POS:
                    _rxDstHi = _rxByte;
                    break;
                case datalink::DEST_ADDR_LO_POS:
                    _rxDstLo = _rxByte;
                    break;
                case datalink::LENGTH_POS:
                    armEarlyAckIfAddressed((_rxByte & datalink::DEST_ADDR_TYPE) != 0);
                    break;
                default:
                    break;
            }
        }
        ++_rxByteCount;
    }

    _rxState = RX_WAIT_MORE_DATA;
    return waitMoreDataTimeoutUs();
}

// Queue the End message and reset all per-telegram RX state. Shared by the
// normal telegram-complete timeout and the lost-timeout recovery in onRxEdge.
template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint8_t BitBangDriverTimerIsrT<IsrPolicy>::finishRxTelegram()
{
    const uint8_t rxErrors = _rxErrors;
    _rxErrors = 0;
    _rxByteCount = 0;
    _rxDstHi = 0;
    _rxDstLo = 0;
    _rxExtendedFrame = false;
    _rxExtGroupAddressed = false;
    // t0/t1 travel with the event: the consumer formats this telegram's own
    // start and end, not whatever the ISR happens to be doing when it gets
    // around to logging.
    (void)queueMessage(MessageType::End, rxErrors, diagRxFrameStartUs(), diagNowUs());
    _rxState = RX_IDLE;
    return rxErrors;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::rxHandleTelegramComplete()
{
    const uint8_t rxErrors = finishRxTelegram();

    // The ACK decision was made byte-synchronously during reception (see
    // armEarlyAckIfAddressed); no decision by now means the frame is not ours
    // to acknowledge — don't burn a timer alarm on an empty ACK slot.
    if (_pendingAckByte == 0) {
        return 0;
    }

    if (_txState != TX_IDLE && _txState != TX_WAITING_SLOT) {
        (void)queueMessage(MessageType::DlAckSuppressed, _txState);
        _pendingAckByte = 0;
        return 0;
    }

    // Hand the timeline off to the DL-ACK send window: set state only and return
    // the cumulative offset. onTimerAlarm sees RX_IDLE + TX_WAITING_SLOT and arms
    // _txAlarmUs = firedAlarmUs + offset. firedAlarmUs is the SCHEDULED telegram-
    // complete instant (start-bit-locked via the anti-cumulative chain), so the
    // DL-ACK lands on the 15-BT t_ack window without folding in ISR-entry latency.
    // Writing _txAlarmUs here directly (from timerNow) was what jittered the ACK
    // a few bit-times, so certified couplers intermittently missed it (9–12 BT).
    _dlAckRxErrors = rxErrors;
    _flags |= FLAG_SENDING_ACK;
    _txState = TX_WAITING_SLOT;
    return telegramCompleteToAckStartDelayUs();
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::sendBit(uint8_t bit)
{
    if (bit != 0) {
        _txState = TX_SENDING_ONE;
        return bitTimeUs();
    }

    _txState = TX_SENDING_ZERO_ACTIVE;
    setTxDominantFast();
    return _config.zeroActiveTimeUs;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::processEndOfTxBit()
{
    const int8_t bitPosition = _txBitPosition;
    if (isDataBitPosition(bitPosition)) {
        _txBitPosition = static_cast<int8_t>(bitPosition + 1);
        return sendBit(static_cast<uint8_t>((_txCurrentByte >> bitPosition) & 0x1u));
    }

    switch (bitPosition) {
        case -1:
            // Stamp the CSMA sync point at every transmitted character's start-bit
            // edge — the same reference the RX path uses (onRxEdge line 241). The
            // last character's start bit is what survives as the post-frame sync,
            // so the priority idle wait (_txWaitTime = 11-bit character + 50/53-bit
            // t_idle) is measured from one uniform "last start bit on the bus"
            // reference for both our own transmissions and received frames. A
            // remote L2-ACK of our frame folds in the same way: the RX path stamps
            // the sync at the ACK's start bit, so the next frame waits t_idle from
            // it — not from our own frame end (which cannot see the remote's ACK).
            _lastCharStartUs = timerNow();
            _txBitPosition = 0;
            return sendBit(0);
        case 8:
            _txBitPosition = 9;
            return sendBit(calculateParityBit(_txCurrentByte));
        case 9:
            if ((_flags & FLAG_SENDING_ACK) != 0u) {
                (void)queueMessage(MessageType::DlAckDone, _dlAckEchoSeen ? 1u : 0u,
                                   diagDlAckStartUs(), diagNowUs());
                _flags &= ~FLAG_SENDING_ACK;
                // The sync point was already stamped at this DL-ACK's start bit
                // (case -1) — the same reference the RX path and our data frames
                // use. Do NOT re-stamp it to the byte's end here: that used a
                // different (+11-bit) reference than received frames.
                // The DL-ACK transmission dirtied the RX sampling state via local
                // bus echo — clear it so the next incoming frame decodes cleanly.
                resetRxSamplingState();
                if (_txTelegramLength > 0) {
                    // A frame is queued behind the ACK slot: hand it to the normal
                    // bus-idle gate (txHandleWaitingSlot), which measures the
                    // priority wait from the start-bit sync above — a tight 50/53-
                    // bit t_idle after this ACK, computed exactly on the next tick.
                    _txState = TX_WAITING_SLOT;
                    return bitTimeUs();
                }
                _txState = TX_IDLE;
                return 0;
            }

            ++_txBytePosition;
            _txBitPosition = -1;
            if (_txBytePosition >= _txTelegramLength) {
                const bool ackRequested = txFrameExpectsAck();
                // Frame fully sent: clear the length so a later DL-ACK completion
                // does not treat the stale buffer as a queued retransmission.
                _txTelegramLength = 0;
                // Logical TX completion is HERE, before the ACK/no-ACK branch:
                // the frame is fully on the wire once its last stop bit has
                // completed, whether or not an acknowledgement window follows.
                // Publishing it only on the no-ACK path left every individually
                // addressed frame — i.e. every frame of an ETS session — with no
                // TX timing record at all, which is precisely the measurement
                // needed to tell a corrupted transmission from a lost ACK.
                diagQueueTxFrameEnd(diagNowUs());

                // Our own transmission dirtied the RX sampling state via local bus
                // echo — clear it before the next frame is received (or before the
                // ACK window opens) so its start-bit validation is not corrupted.
                if (ackRequested) {
                    prepareAckReception();
                    _txState = TX_WAITING_ACK;
                    // The stop bit is not driven explicitly (the line is already
                    // recessive and processEndOfTxBit runs at its START), so the
                    // extra bit time is what makes the window open at the true
                    // frame end. t_ack lands at 15 BT, well inside it.
                    return bitTimeUs() + ackTimeoutUs();
                }

                resetRxSamplingState();
                // The sync point was already stamped at this frame's last start bit
                // (case -1) — the uniform "last start bit on the bus" reference
                // shared with the RX path. Do NOT overwrite it with the frame-end
                // time: that used a different (+11-bit) reference than received
                // frames AND could not account for a remote L2-ACK of this frame.
                // A remote ACK now folds in naturally — the RX path stamps the sync
                // at the ACK's start bit, so the next TX waits t_idle from the ACK.
                _txState = TX_IDLE;
                (void)queueMessage(MessageType::TxComplete, 0);
                return 0;
            }

            _txCurrentByte = _txBuffer[_txBytePosition];
            return bitTimeUs() + interByteTimeUs();
        default:
            return 0;
    }
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::txHandleWaitingSlot(uint64_t nowUs)
{
    if ((_flags & FLAG_SENDING_ACK) != 0u) {
        // DL-ACK slot alarm: the slot is only armed when a decision was latched
        // during reception (armEarlyAckIfAddressed), so pending is normally
        // non-zero here; a collision reset in between can clear it.
        const uint8_t pending = _pendingAckByte;
        _pendingAckByte = 0;

        if (pending != 0) {
            // On reception errors, downgrade the pending ACK decision:
            //   ACK  (0xCC) → NACK      (0x0C): frame had bit errors, sender must retransmit
            //   BUSY (0xC0) → NACK+BUSY (0x00): frame had errors AND device was busy
            // The "reception quality OK" signal lives in the upper nibble (ACK_BYTE_BUSY = 0xC0).
            // Clearing it on errors converts ACK→NACK and BUSY→NACK+BUSY correctly.
            _txCurrentByte = (_dlAckRxErrors != 0)
                                 ? static_cast<uint8_t>(pending & (~ACK_BYTE_BUSY))
                                 : pending;
            _dlAckEchoSeen = false;
            _txBitPosition = -1;
            _txBytePosition = 0;
            diagDlAckStart(diagNowUs());
            // Carries the byte; DlAckDone that follows carries the timing and
            // the echo result. Nothing can be queued between the two — while
            // the DL-ACK is driven, onRxEdge returns before touching the queue.
            (void)queueMessage(MessageType::DlAckTxStart, _txCurrentByte);
            return processEndOfTxBit();
        }

        // Decision was cleared before the slot fired — stand down.
        _flags &= ~FLAG_SENDING_ACK;
        if (_txTelegramLength == 0) {
            _txState = TX_IDLE;
            return 0;
        }
        // A queued frame was waiting behind the armed ACK slot — fall through
        // to the normal bus-idle wait for it.
    }

    // CSMA: never begin driving the bus while a reception is in progress. The
    // priority wait below is measured from _lastCharStartUs (the last observed bus
    // edge), but a long incoming telegram only refreshes _lastCharStartUs at each
    // byte's start bit, so the elapsed-time test can go true mid-telegram and
    // start a frame on top of the one being received — the collisions seen when
    // the device answers into ETS's DeviceDescriptor_Read repeats. Defer until
    // the receiver returns to idle, then re-arm; _lastCharStartUs will then reflect
    // the just-received frame so the priority wait runs from its end.
    if (_rxState != RX_IDLE) {
        return bitTimeUs();
    }

    const uint64_t waitTimeUs = bitTimeUs() * _txWaitTime;
    if ((nowUs - _lastCharStartUs) >= waitTimeUs) {
        _txBitPosition = -1;
        _txBytePosition = 0;
        _txCurrentByte = _txBuffer[0];
        // Diagnostics: mark the instant the first start bit is driven so the
        // whole-frame duration and worst per-bit lateness can be measured.
        diagTxFrameStart(nowUs);
        return processEndOfTxBit();
    }

    return (_lastCharStartUs + waitTimeUs) - nowUs;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::txHandleWaitingAck()
{
    // Classify the timeout HERE, in the ISR, while the evidence is still the
    // state of this transaction. "No L_ACK" on its own cannot distinguish a
    // peer that stayed silent from an acknowledgement this receiver threw away,
    // and those have opposite fixes. No timer read, no allocation — three
    // counters read back from task context whenever they are wanted.
    if (!_ackStartEdgeSeen) {
        ++_linkCounters.ackTimeoutNoEdge;
    } else if (!_ackStartValidated) {
        ++_linkCounters.ackTimeoutStartRejected;
    } else {
        ++_linkCounters.ackTimeoutIncomplete;
    }

    _ackStartEdgeSeen = false;
    _ackStartValidated = false;
    // Drop the abandoned window's decoder residue, but only when nothing is in
    // flight: a character that started late is still worth finishing, and once
    // TX_WAITING_ACK is left it completes down the ordinary receive path,
    // where finishRxTelegram() consumes _rxErrors properly. Resetting under it
    // would discard a byte the bus really carried.
    if (_rxState == RX_IDLE) {
        resetRxSamplingState();
        _rxErrors = 0;
    }
    (void)queueMessage(MessageType::TxAckResponse, ACK_BYTE_NONE);
    _txState = TX_IDLE;
    return 0;
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR uint64_t BitBangDriverTimerIsrT<IsrPolicy>::processTxTimer()
{
    switch (_txState) {
        case TX_SENDING_ONE:
        case TX_SENDING_ZERO_EQUALIZATION:
            setTxRecessiveFast();
            return processEndOfTxBit();

        case TX_SENDING_ZERO_ACTIVE:
            if ((_flags & FLAG_SENDING_ACK) != 0u) {
                // Sample RX just before releasing the dominant drive to check local bus echo.
                // A working transceiver should pull the bus dominant when TX is asserted, which
                // is visible on the RX pin. If RX never goes dominant during any active bit of
                // the DL-ACK, the transceiver is not driving the bus (hardware/config issue).
                const bool rxDominant =
                    _isrHal.readRxLevel() == static_cast<int>(_rxDominantLevel);
                if (rxDominant) {
                    _dlAckEchoSeen = true;
                }
            }
            setTxRecessiveFast();
            _txState = TX_SENDING_ZERO_EQUALIZATION;
            return _config.zeroEqualizationTimeUs;

        case TX_WAITING_ACK:
            return txHandleWaitingAck();

        case TX_WAITING_SLOT:
            return txHandleWaitingSlot(timerNow());

        case TX_IDLE:
        default:
            return 0;
    }
}

template<typename IsrPolicy>
KNX_TIMER_GPIO_HAL_ISR_ATTR bool BitBangDriverTimerIsrT<IsrPolicy>::detectCollision() noexcept
{
    // Collision is meaningful only while we expect the bus to stay recessive
    // (sending a logical '1'). A dominant edge in that phase indicates another
    // sender pulled the line, i.e. arbitration loss.
    if (_txState != TX_SENDING_ONE || (_flags & (FLAG_SENDING_ACK | FLAG_NO_COLLISION_DETECT)) != 0u) {
        return false;
    }

    (void)queueMessage(MessageType::Collision, 0);

    // Abort this TX attempt on collision and report BUSY to upper layers.
    // Retrying indefinitely at ISR level can flood the bus when collision
    // detection is triggered repeatedly.
    //
    // BUSY is not literally what happened — this is arbitration loss, not a
    // peer reporting itself busy — but it is the outcome the data-link layer
    // needs: back off and retry, which is exactly right for a frame that lost
    // the bus. The distinct Collision message above carries the honest signal
    // for diagnostics. Reporting the two differently would require a TX outcome
    // that the MAC layer does not have, and pollTransmit() only advances on an
    // ack-response or deadline-miss event, so dropping this would hang the
    // transmit state machine.
    setTxRecessiveFast();
    clearTxAttemptState();
    _txState = TX_IDLE;
    (void)queueMessage(MessageType::TxAckResponse, ACK_BYTE_BUSY);
    return true;
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

} // namespace physical
} // namespace knx

// Explicit instantiation for the platform ISR policy selected by the alias in
// bitbang_driver_timer_isr.hpp. Forces code generation in this translation unit.
#ifdef ESP_PLATFORM
template class knx::physical::BitBangDriverTimerIsrT<knx::physical::EspIdfIsrHalPolicy>;
#else
template class knx::physical::BitBangDriverTimerIsrT<knx::physical::VirtualIsrHalPolicy>;
#endif
