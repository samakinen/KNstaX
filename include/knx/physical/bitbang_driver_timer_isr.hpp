// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/datalink/tp1_dl_common.hpp"
#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/bitbang_timing_stats.hpp"
#include "knx/physical/timer_gpio_hal.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace physical {

/// Production-safe link-quality counters maintained by the ISR.
///
/// Every field is a saturating-in-practice uint32_t incremented from interrupt
/// context with no timer read and no logging, so the whole struct stays enabled
/// in release builds. Together they answer the question a bare "ACK timeout"
/// cannot: was the acknowledgement absent from the bus, present but rejected by
/// start-bit validation, or received corrupted?
struct BitBangLinkCounters {
    /// Start bit rejected by the validation re-read, outside an ACK window.
    /// A nonzero and growing value means RX characters are being dropped
    /// because ISR latency pushed the re-read past the 35 µs dominant pulse.
    uint32_t startValidationRejects{0};
    /// Start bit rejected by the validation re-read while awaiting an L2 ACK.
    uint32_t ackStartValidationRejects{0};
    /// ACK window expired without any dominant edge — the peer sent nothing.
    uint32_t ackTimeoutNoEdge{0};
    /// ACK window expired after an edge whose start bit failed validation.
    uint32_t ackTimeoutStartRejected{0};
    /// ACK window expired after a validated start bit — sampling was lost.
    uint32_t ackTimeoutIncomplete{0};
    /// ACK character completed but carried a parity or framing error.
    uint32_t ackCharacterErrors{0};
    /// Timer compares that fired for a deadline no longer armed — a pending
    /// interrupt that survived a reprogram. Should stay at zero; a nonzero
    /// value means the timer HAL does not clear the old compare condition on
    /// rearm, and the scheduler is relying on its own guard to stay correct.
    uint32_t staleTimerAlarms{0};

    void reset() noexcept { *this = BitBangLinkCounters{}; }
};

template<typename IsrPolicy>
class BitBangDriverTimerIsrT {
public:
    enum class MessageType : uint8_t {
        Data = 2,
        ParityError = 3,
        FramingError = 4,
        End = 5,
        TxAckResponse = 7,
        Collision = 8,
        TxComplete = 11,  ///< Non-ack frame TX transaction done (no ack window follows)
        EarlyAckArmed = 12, ///< ISR early ACK: _pendingAckByte set for an addressed frame
        DlAckSuppressed = 13, ///< DL-ACK dropped because TX was busy (data=_txState)
        DlAckTxStart = 14,    ///< DL-ACK byte started transmitting on the bus
        DlAckDone = 15,       ///< DL-ACK byte fully sent; data=1 if local bus echo seen, 0 if not
        /// Diagnostics only, queued only when CONFIG_KNX_TP1_BITBANG_TIMING_STATS
        /// is set: the frame's last stop bit has completed, regardless of whether
        /// an ACK window follows. This is what makes TX timing observable for
        /// acknowledged frames — TxComplete never fires for those, so without it
        /// the per-bit alarm lateness of every individually addressed frame is
        /// invisible. data = worst per-bit lateness in µs, saturated at 255.
        TxFrameEnd = 16,
    };

    /// One ISR→task event.
    ///
    /// When timing diagnostics are enabled the event carries its OWN timestamps,
    /// captured in the ISR at the instant it is queued. This is deliberate: the
    /// task formats messages long after the fact, so reading live diagnostic
    /// fields at format time correlated timestamps with the wrong event and
    /// produced impossible traces (t1 before t0, 11-byte frames "lasting"
    /// 1.4 ms). The timestamps are raw free-running timer µs truncated to 32
    /// bits, which is unambiguous over any interval this trace spans.
    /// Both fields vanish from the object layout in production builds.
    struct Message {
        MessageType type{MessageType::Data};
        uint8_t data{0};
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
        uint32_t t0{0};  ///< event start instant (see MessageType docs)
        uint32_t t1{0};  ///< event end instant, 0 when the event is punctual
#endif
    };

#ifndef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    // Guards the "diagnostics compile out completely" contract: with the
    // feature off the message must be exactly the two protocol bytes, so the
    // ring costs the same 256 bytes it always did.
    static_assert(sizeof(Message) == 2,
                  "diagnostic timestamps must not exist in production builds");
#endif

    static constexpr uint8_t ACK_BYTE_NONE = 0xFF;
    static constexpr uint8_t ACK_BYTE_ACK = 0xCC;
    static constexpr uint8_t ACK_BYTE_NACK = 0x0C;
    static constexpr uint8_t ACK_BYTE_BUSY = 0xC0;
    static constexpr uint8_t ACK_BYTE_NACK_BUSY = 0x00;

    /// Maximum group addresses the ISR early-ACK matcher scans per telegram.
    static constexpr uint8_t MAX_ACK_GROUP_ADDRESSES = 64;

    bool init(const knx_timer_gpio_hal_t& hal, const BitBangConfig& config);
    void shutdown();

    bool send(std::span<const uint8_t> frame);
    void setOwnAddress(uint16_t addr) { _ownAddressRaw = addr; }

    // ── ISR ACK decision configuration (task context) ──────────────────────
    // The DL-ACK decision itself is made entirely inside the ISR as the frame
    // header streams in — a task-scheduled decision can never reliably meet the
    // 15-bit-time (~1.56 ms) t_ack deadline. Tasks only configure the decision
    // inputs ahead of time here.

    /// Publish the subscribed group addresses the ISR should L_ACK (when the
    /// sender requests acknowledgement).
    ///
    /// Published under the same ISR lock send() uses. `volatile` on the count
    /// orders nothing with respect to the plain array stores beside it, so the
    /// compiler was free to publish the count before the entries it describes;
    /// a critical section makes the table and its length change together. This
    /// runs at commissioning, not on any hot path, and the section is bounded by
    /// a 64-entry copy.
    void setAckGroupAddresses(const uint16_t* addresses, uint8_t count) {
        if (count > MAX_ACK_GROUP_ADDRESSES) {
            count = MAX_ACK_GROUP_ADDRESSES;
        }
        _isrHal.lockFromTask();
        for (uint8_t i = 0; i < count; ++i) {
            _ackGroupAddresses[i] = addresses[i];
        }
        _ackGroupAddressCount = count;
        _isrHal.unlockFromTask();
    }

    /// Busy state reflected in the ISR's ACK decision (BUSY instead of ACK).
    void setLocalBusy(bool busy) { _localBusy = busy; }

    bool popMessage(Message& outMessage);

    using QueueNotifyCallback = void(*)(void* context);
    void setQueueNotifyCallback(QueueNotifyCallback callback, void* context) {
        _queueNotifyCallback = callback;
        _queueNotifyContext = context;
    }

    void onTimerAlarm();
    void onRxEdge();

    uint32_t droppedMessageCount() const noexcept;

    /// Link-quality counters (see BitBangLinkCounters). Always available —
    /// maintaining them costs one increment on already-exceptional paths.
    /// Readable from task context; individual increments are not atomic, which
    /// is acceptable for counters that are only ever compared against
    /// themselves over time.
    const BitBangLinkCounters& linkCounters() const noexcept { return _linkCounters; }

#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    /// Read-only access to accumulated ISR timing statistics.
    /// May be called from task context; individual samples are best-effort consistent.
    const BitBangTimingStats& timingStats() const noexcept { return _timingStats; }

    /// Reset all timing stat accumulators to zero. Safe to call from task context
    /// on a single-core device; individual counter updates are not atomic.
    void resetTimingStats() noexcept { _timingStats.reset(); }
#endif

private:
    // Full L_Data_Extended frame: 8-byte header + 254-byte APDU + FCS.
    static constexpr size_t MAX_TELEGRAM_SIZE = 263;
    // Power of two so ring wrap-around is an AND mask and the free-running
    // 32-bit producer/consumer indices stay exact across overflow.
    static constexpr size_t MESSAGE_QUEUE_CAPACITY = 128;
    static_assert((MESSAGE_QUEUE_CAPACITY & (MESSAGE_QUEUE_CAPACITY - 1u)) == 0u,
                  "message queue capacity must be a power of two");
    static constexpr uint32_t MESSAGE_QUEUE_MASK = MESSAGE_QUEUE_CAPACITY - 1u;
    // Depth at which a queued message wakes the consumer even when its type
    // would not normally notify. A 263-byte L_Data_Extended telegram produces
    // more Data messages than the ring holds, so deferring every wakeup to the
    // End message would drop the tail of the frame.
    static constexpr uint32_t MESSAGE_QUEUE_NOTIFY_WATERMARK = MESSAGE_QUEUE_CAPACITY / 2u;

    enum RxState : uint8_t {
        RX_IDLE = 1,
        RX_START_BIT_PENDING = 2,
        RX_RECEIVE = 5,
        RX_WAIT_MORE_DATA = 6,
    };

    enum TxState : uint8_t {
        TX_IDLE = 0,
        TX_WAITING_SLOT = 8,
        TX_SENDING_ZERO_ACTIVE = 9,
        TX_SENDING_ZERO_EQUALIZATION = 10,
        TX_SENDING_ONE = 11,
        TX_WAITING_ACK = 13,
    };

    /// Which logical deadline the hardware compare is currently programmed for.
    ///
    /// Replaces a mutable "next event is TX" bit. The distinction matters: a
    /// flag says what the scheduler last decided, this says what the hardware
    /// was actually armed with. onTimerAlarm() consumes the record and services
    /// the matching deadline only if it is still the one that was programmed,
    /// so an alarm that was replaced between latching and dispatch cannot be
    /// serviced under the wrong timeline.
    enum class AlarmOwner : uint8_t {
        None,
        Rx,
        Tx,
    };

    static constexpr uint32_t FLAG_NO_COLLISION_DETECT = (1u << 1);
    static constexpr uint32_t FLAG_SENDING_ACK = (1u << 3);

    static constexpr uint8_t RX_ERROR_PARITY = (1u << 0);
    static constexpr uint8_t RX_ERROR_FRAMING = (1u << 1);

    // ── ISR hot state: fields accessed on every ISR entry ────────────────────
    bool     _initialized{false};
    uint8_t  _rxZeroDetected{0};
    uint8_t  _rxState{RX_IDLE};
    uint8_t  _txState{TX_IDLE};
    uint32_t _flags{0};
    AlarmOwner _armedAlarmOwner{AlarmOwner::None};
    uint64_t _armedAlarmUs{0};
    uint64_t _rxAlarmUs{0};
    uint64_t _txAlarmUs{0};
    /// Start-bit instant of the current or most recently observed character on
    /// the bus — set by onRxEdge() for received characters and by
    /// processEndOfTxBit() case -1 for every character this node transmits,
    /// including DL-ACKs. One uniform reference for the CSMA idle gate, which
    /// is why it is NOT updated at parity, stop, frame end or ISR entry.
    uint64_t _lastCharStartUs{0};
    // ── IsrPolicy: compile-time hardware abstraction for the ISR hot path ──────
    // Populated once by attach() in init(). On ESP-IDF, expands to direct GPIO
    // register writes and gptimer calls with no vtable or null-check overhead.
    // On Linux/test builds, routes through the vtable for mock injection.
    IsrPolicy _isrHal{};
    // ── Full HAL vtable: configure/install/start/stop (slow path only) ───────
    knx_timer_gpio_hal_t _hal{};

    // RX fields (touched per received byte)
    uint8_t  _rxByte{0};
    int8_t   _rxBitPosition{-1};
    uint8_t  _rxErrors{0};
    uint8_t  _pendingAckByte{0};
    // Per-frame state for ISR-level early DL-ACK decision
    uint8_t  _rxByteCount{0};
    uint8_t  _rxDstHi{0};
    uint8_t  _rxDstLo{0};
    bool     _rxExtendedFrame{false};    // CTRL bits 7..6 == 00 (L_Data_Extended)
    bool     _rxExtGroupAddressed{false}; // CTRLE bit 7 of an extended frame
    uint8_t  _rxDominantLevel{1};        // GPIO level meaning "dominant", from config
    uint16_t _ownAddressRaw{0};
    // ISR ACK decision inputs, configured from task context (see setters above).
    volatile bool    _localBusy{false};
    volatile uint8_t _ackGroupAddressCount{0};
    std::array<uint16_t, MAX_ACK_GROUP_ADDRESSES> _ackGroupAddresses{};
    // ACK receive transaction: one-bit state feeding the timeout classification
    // in txHandleWaitingAck(). Cleared by prepareAckReception().
    bool     _ackStartEdgeSeen{false};
    bool     _ackStartValidated{false};
    // TX fields
    bool     _dlAckEchoSeen{false};  // ISR: RX was dominant during ≥1 active bit of the DL-ACK
    uint8_t  _dlAckRxErrors{0};      // RX error flags captured at telegram-complete for the armed DL-ACK slot
    uint8_t  _txCurrentByte{0};
    int8_t   _txBitPosition{-1};
    // 16-bit because MAX_TELEGRAM_SIZE is 263: a full L_Data_Extended frame
    // wraps a uint8_t length and truncates the transmission to a few bytes.
    uint16_t _txBytePosition{0};
    uint16_t _txTelegramLength{0};
    uint8_t  _txWaitTime{0};
    std::array<uint8_t, MAX_TELEGRAM_SIZE> _txBuffer{};

    BitBangLinkCounters _linkCounters{};

    // ── Optional TX/RX timing diagnostics (bench bring-up only) ──────────────
    // Gated by CONFIG_KNX_TP1_BITBANG_TIMING_STATS. When disabled, every diag*
    // helper below is an empty inline and diagNowUs() folds to a constant 0, so
    // the ISR makes ZERO extra timer reads — in particular the per-TX-bit
    // lateness probe (an extra gptimer read on every bit edge) disappears
    // entirely. The timerNow() calls that feed protocol state (_lastCharStartUs,
    // alarm scheduling) are unconditional and not part of this block.
    //
    // Nothing here is exposed as a getter any more. Every captured value is
    // handed to queueMessage() at the instant the corresponding event is
    // published, so the task always formats a snapshot that belongs to the
    // event it dequeued. The fields below are therefore only the in-flight
    // accumulators needed between "event begins" and "event is queued".
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    uint64_t _txFrameStartUs{0};  ///< first start bit of the TX frame in flight
    uint32_t _txMaxLateUs{0};     ///< worst per-bit timer-alarm lateness this frame
    uint64_t _rxFrameStartUs{0};  ///< first start bit of the RX telegram in flight
    uint64_t _dlAckStartUs{0};    ///< instant the DL-ACK byte in flight began transmitting

    /// Raw timer read, but only in diagnostic builds. Every timestamp handed to
    /// queueMessage() comes from here so production keeps a literal zero.
    [[gnu::always_inline]] uint64_t diagNowUs() const noexcept { return timerNow(); }

    // Worst per-bit alarm lateness while actively driving TX bits. The extra
    // timerNow() here is the only per-bit diagnostic read — the whole reason
    // this family is gated out of the production hot path.
    [[gnu::always_inline]] void diagTxBitLateness(uint64_t firedAlarmUs) noexcept {
        if (_txState == TX_SENDING_ONE
                || _txState == TX_SENDING_ZERO_ACTIVE
                || _txState == TX_SENDING_ZERO_EQUALIZATION) {
            const uint32_t lateUs = static_cast<uint32_t>(timerNow() - firedAlarmUs);
            if (lateUs > _txMaxLateUs) {
                _txMaxLateUs = lateUs;
            }
        }
    }
    [[gnu::always_inline]] void diagTxFrameStart(uint64_t nowUs) noexcept {
        _txFrameStartUs = nowUs;
        _txMaxLateUs = 0;
    }
    [[gnu::always_inline]] void diagRxFrameStart(uint64_t nowUs) noexcept { _rxFrameStartUs = nowUs; }
    [[gnu::always_inline]] void diagDlAckStart(uint64_t nowUs) noexcept { _dlAckStartUs = nowUs; }
    /// Worst per-bit lateness of the frame just finished, saturated into the
    /// message's data byte. Anything at or above 255 µs is already two and a
    /// half bit times — the exact value stops mattering well before that.
    [[gnu::always_inline]] uint8_t diagTxMaxLateSaturated() const noexcept {
        return static_cast<uint8_t>(_txMaxLateUs > 255u ? 255u : _txMaxLateUs);
    }
    [[gnu::always_inline]] uint64_t diagTxFrameStartUs() const noexcept { return _txFrameStartUs; }
    [[gnu::always_inline]] uint64_t diagRxFrameStartUs() const noexcept { return _rxFrameStartUs; }
    [[gnu::always_inline]] uint64_t diagDlAckStartUs() const noexcept { return _dlAckStartUs; }
#else
    [[gnu::always_inline]] uint64_t diagNowUs() const noexcept { return 0; }
    [[gnu::always_inline]] void diagTxBitLateness(uint64_t) noexcept {}
    [[gnu::always_inline]] void diagTxFrameStart(uint64_t) noexcept {}
    [[gnu::always_inline]] void diagRxFrameStart(uint64_t) noexcept {}
    [[gnu::always_inline]] void diagDlAckStart(uint64_t) noexcept {}
    [[gnu::always_inline]] uint8_t diagTxMaxLateSaturated() const noexcept { return 0; }
    [[gnu::always_inline]] uint64_t diagTxFrameStartUs() const noexcept { return 0; }
    [[gnu::always_inline]] uint64_t diagRxFrameStartUs() const noexcept { return 0; }
    [[gnu::always_inline]] uint64_t diagDlAckStartUs() const noexcept { return 0; }
#endif

    /// Publish the "frame fully on the wire" diagnostic event. Compiled out
    /// entirely in production, where nothing consumes it — an acknowledged
    /// frame's TX timing is a bring-up measurement, not a protocol signal.
    [[gnu::always_inline]] void diagQueueTxFrameEnd(uint64_t txEndUs) noexcept
    {
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
        (void)queueMessage(MessageType::TxFrameEnd, diagTxMaxLateSaturated(),
                           _txFrameStartUs, txEndUs);
#else
        (void)txEndUs;
#endif
    }

#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    BitBangTimingStats _timingStats{};
#endif

    // Lock-free single-producer/single-consumer ring: the ISR produces at
    // _msgTail, exactly one task context consumes at _msgHead (the ESP-IDF
    // wrapper serializes its consumers). Free-running 32-bit indices; entries
    // are addressed modulo the power-of-two capacity. No shared count — a
    // count would need a read-modify-write from both contexts, which the ISR
    // can tear mid-update.
    std::array<Message, MESSAGE_QUEUE_CAPACITY> _messages{};
    std::atomic<uint32_t> _msgHead{0};
    std::atomic<uint32_t> _msgTail{0};
    std::atomic<uint32_t> _msgDropped{0};
    BitBangConfig _config{};

    QueueNotifyCallback _queueNotifyCallback{nullptr};
    void* _queueNotifyContext{nullptr};

    bool queueMessage(MessageType type, uint8_t data,
                      uint64_t t0Us = 0, uint64_t t1Us = 0) noexcept;
    static uint8_t calculateParityBit(uint8_t byte) noexcept;

    /// Whether queueing `type` must wake the consumer immediately.
    ///
    /// Only messages that complete a unit of work do. Notifying on every queued
    /// message cost a vTaskNotifyGiveFromISR plus a portYIELD_FROM_ISR out of
    /// the bit-timing interrupt roughly twice per received octet, and that
    /// context switch is charged to the latency budget of the very next bit
    /// sample — the same budget that decides whether a bit lands in the right
    /// cell. The excluded types are all published mid-transaction, which is the
    /// worst possible moment to run the scheduler:
    ///   - Data:          followed by End (the stale-inter-byte recovery in
    ///                    onRxEdge guarantees one even for a truncated telegram)
    ///   - EarlyAckArmed: mid-telegram, purely informational
    ///   - DlAckTxStart:  followed by DlAckDone once the ACK byte is out
    ///   - TxFrameEnd:    diagnostic; TxComplete or TxAckResponse follows
    /// The queue-depth watermark in queueMessage() covers long frames that
    /// would otherwise overrun the ring before their End arrives.
    [[gnu::always_inline]] static bool messageNeedsNotify(MessageType type) noexcept
    {
        return type != MessageType::Data
            && type != MessageType::EarlyAckArmed
            && type != MessageType::DlAckTxStart
            && type != MessageType::TxFrameEnd;
    }

    void rearmTimer();
    uint64_t processRxTimer();
    uint64_t processTxTimer();

    uint64_t processReceiveBit();
    uint64_t processStartBit();
    uint64_t rxHandleStop(uint8_t bit);
    uint64_t rxHandleTelegramComplete();
    uint8_t finishRxTelegram();

    uint64_t sendBit(uint8_t bit);
    uint64_t processEndOfTxBit();

    uint64_t txHandleWaitingSlot(uint64_t nowUs);
    uint64_t txHandleWaitingAck();

    bool detectCollision() noexcept;

    // Clear the RX bit-sampling state after a transmission completes. During our
    // own TX the local bus echo fires onRxEdge on every edge (which sets
    // _rxZeroDetected and can advance _rxState), so the RX machine is left dirty.
    // Failing to clear it corrupts start-bit validation of the NEXT incoming
    // frame — observed as spurious parity errors right after a broadcast TX.
    //
    // _rxErrors belongs to the RX *telegram*, not to one character, so it is
    // owned by finishRxTelegram()/finishReceivedAck() and deliberately not
    // touched here.
    [[gnu::always_inline]] void resetRxSamplingState() noexcept
    {
        _rxState = RX_IDLE;
        _rxByte = 0;
        _rxBitPosition = -1;
        _rxZeroDetected = 0;
    }

    /// Open the L2 acknowledgement window for the frame just transmitted.
    ///
    /// The ACK character is decoded by the ordinary RX byte machine, so its
    /// decoder state must start clean — including _rxErrors. Leaving a stale
    /// error there is not cosmetic: finishRxTelegram() of the NEXT telegram
    /// would report it, downgrading that telegram's DL-ACK to NACK and making
    /// the peer retransmit a frame that arrived perfectly.
    /// Discard everything belonging to the current transmit attempt, leaving the
    /// receive timeline untouched. The caller sets the resulting _txState —
    /// arbitration loss and reinitialisation want different ones.
    [[gnu::always_inline]] void clearTxAttemptState() noexcept
    {
        _flags &= ~FLAG_SENDING_ACK;
        _pendingAckByte = 0;
        _ackStartEdgeSeen = false;
        _ackStartValidated = false;
        _txTelegramLength = 0;
        _txBytePosition = 0;
        _txBitPosition = -1;
        _txAlarmUs = 0;
    }

    /// TX_WAITING_ACK is itself the "an ACK window is open" state — there is no
    /// separate flag. One used to exist and was necessarily set and cleared in
    /// lockstep with this state, which made every combination of the two either
    /// redundant or invalid.
    [[gnu::always_inline]] void prepareAckReception() noexcept
    {
        resetRxSamplingState();
        _rxErrors = 0;
        _ackStartEdgeSeen = false;
        _ackStartValidated = false;
        _txState = TX_WAITING_ACK;
    }

    /// Close the ACK receive transaction and report its outcome.
    ///
    /// A character that arrived with a parity or framing error is NOT an
    /// acknowledgement: reporting its raw byte would let a corrupted 0xCC pass
    /// as ACK, or a corrupted anything be logged as an unsupported ack byte.
    /// It is reported as ACK_BYTE_NONE instead, which routes into the data-link
    /// layer's existing "no L_ACK → repeat the frame" path — the spec-correct
    /// response to an acknowledgement that could not be read.
    [[gnu::always_inline]] uint8_t finishReceivedAck() noexcept
    {
        const uint8_t ackErrors = _rxErrors;
        const uint8_t ackByte = _rxByte;
        resetRxSamplingState();
        _rxErrors = 0;
        _ackStartEdgeSeen = false;
        _ackStartValidated = false;
        if (ackErrors != 0u) {
            ++_linkCounters.ackCharacterErrors;
            return ACK_BYTE_NONE;
        }
        return ackByte;
    }

    // ISR-side DL-ACK decision, evaluated the instant the destination address
    // is fully known (mid-frame). Latches the ACK/BUSY intent that the DL-ACK
    // slot alarm transmits after t_ack; reception errors detected later
    // downgrade it to NACK at slot time.
    /// Whether the frame just transmitted from _txBuffer must be acknowledged
    /// by its addressee, i.e. whether to open the ACK window instead of going
    /// idle.  Mirrors armEarlyAckIfAddressed() on the receive side: TP1 has no
    /// meaningful ACK-request bit on the wire, so every individually addressed
    /// frame is acknowledged and gating on CTRL bit 1 alone means the sender
    /// never learns that a frame was NAK'd or lost.
    [[gnu::always_inline]] bool txFrameExpectsAck() const noexcept
    {
        if (_txTelegramLength == 0) {
            return false;
        }
        if ((_txBuffer[datalink::CTRL_FIELD_POS] & datalink::CTRL_ACK_REQ) != 0u) {
            return true;
        }

        // Standard: CTRL SA SA DA DA NPCI ... — address type in the NPCI octet.
        // Extended: CTRL CTRLE SA SA DA DA LEN ... — address type in CTRLE.
        const bool standardFrame =
            (_txBuffer[datalink::CTRL_FIELD_POS] & datalink::CTRL_FRAME_TYPE) != 0u;
        const size_t addressTypePos = standardFrame ? datalink::LENGTH_POS : size_t{1};
        const size_t destHiPos = standardFrame ? datalink::DEST_ADDR_HI_POS : size_t{4};

        if (_txTelegramLength <= addressTypePos || _txTelegramLength <= destHiPos + 1u) {
            return false;
        }
        if ((_txBuffer[addressTypePos] & datalink::DEST_ADDR_TYPE) != 0u) {
            return false;  // group addressed, including broadcast 0/0/0
        }

        const uint16_t destRaw = static_cast<uint16_t>(
            (static_cast<uint16_t>(_txBuffer[destHiPos]) << 8) | _txBuffer[destHiPos + 1u]);
        // The individual broadcast 0xFFFF is never acknowledged — same
        // exclusion the receive side applies.
        return destRaw != 0xFFFFu;
    }

    [[gnu::always_inline]] void armEarlyAckIfAddressed(bool isGroupAddressed) noexcept
    {
        const uint16_t destRaw = (static_cast<uint16_t>(_rxDstHi) << 8) | _rxDstLo;
        bool addressed;
        if (isGroupAddressed) {
            // Group frames: the device is addressed when the destination is in
            // its group address table, or is the broadcast address 0/0/0 —
            // and an addressed device SHALL confirm (03_02_02 §2.3, mirrors
            // TPUART/NCN5120 auto-ACK). There is no ACK-request bit on the
            // TP1 wire: certified senders keep CTRL bits 1..0 at zero, so
            // gating on them would suppress every acknowledgment.
            addressed = destRaw == 0x0000u;
            if (!addressed) {
                const uint8_t count = _ackGroupAddressCount;
                for (uint8_t i = 0; i < count; ++i) {
                    if (_ackGroupAddresses[i] == destRaw) {
                        addressed = true;
                        break;
                    }
                }
            }
        } else {
            // Individually-addressed frames: always ACK when they match our
            // address; the individual broadcast 0xFFFF is never acknowledged.
            addressed = _ownAddressRaw != 0
                && destRaw != 0xFFFFu
                && destRaw == _ownAddressRaw;
        }

        if (addressed) {
            const uint8_t ackByte = _localBusy ? ACK_BYTE_BUSY : ACK_BYTE_ACK;
            _pendingAckByte = ackByte;
            (void)queueMessage(MessageType::EarlyAckArmed, ackByte);
        }
    }
    // Hot-path hardware delegates — inlined into ISR callers by the compiler.
    // IsrPolicy methods are [[gnu::always_inline]] on ESP-IDF, so the full chain
    // collapses to 1–3 instructions per call site with no function-call overhead.
    [[gnu::always_inline]] uint64_t timerNow() const noexcept
        { return _isrHal.timerNow(); }
    [[gnu::always_inline]] void rearmTimerAbs(uint64_t absUs) noexcept
        { _isrHal.rearmTimerAbs(absUs); }
    [[gnu::always_inline]] void setTxDominantFast() noexcept
        { _isrHal.setTxDominant(); }
    [[gnu::always_inline]] void setTxRecessiveFast() noexcept
        { _isrHal.setTxRecessive(); }

    // Trivial timing getters: always inlined so the compiler eliminates the call
    // overhead and can hoist the _config.serialBitTimeUs load into a register.
    [[gnu::always_inline]] uint64_t bitTimeUs() const noexcept
        { return _config.serialBitTimeUs; }
    [[gnu::always_inline]] uint64_t interByteTimeUs() const noexcept
        { return _config.serialBitTimeUs * 2u; }
    [[gnu::always_inline]] uint64_t ackTimeoutUs() const noexcept
        { return _config.serialBitTimeUs * 35u; }
    [[gnu::always_inline]] uint64_t waitMoreDataTimeoutUs() const noexcept
        { return _config.serialBitTimeUs * 3u; }
    // An RX edge in RX_WAIT_MORE_DATA later than this after the previous byte's
    // start bit means the telegram-complete timeout (~13.5 bit-times after that
    // start bit) was lost or starved: the edge is the NEXT frame, not the next
    // byte. Within a frame the next byte's edge arrives ≤ 13 bit-times after
    // the previous start bit (11-bit character + 2-bit inter-byte gap).
    [[gnu::always_inline]] uint64_t rxInterByteStaleUs() const noexcept
        { return _config.serialBitTimeUs * 16u; }
    // ── Protocol quantity ────────────────────────────────────────────────────
    // TP1 requires the L2 acknowledgement character to start t_ack after the end
    // of the received frame — this is exactly when a TPUART-based coupler samples
    // for it (03_02_02).
    static constexpr uint32_t ACK_START_FROM_FRAME_END_BT = 15u;
    // How far the telegram-complete timeout sits past the true frame end: it
    // runs waitMoreDataTimeoutUs (3 BT) after the FCS stop-bit sample, which is
    // itself bitSamplingOffsetUs into the 11th bit of the character.
    static constexpr uint32_t TELEGRAM_COMPLETE_FROM_FRAME_END_BT = 2u;

    // ── Derived schedule offset ──────────────────────────────────────────────
    // Applied from the moment the telegram-complete timeout runs, like every
    // other ISR state transition, so the DL-ACK inherits that alarm's
    // start-bit-locked schedule rather than an ISR-entry timestamp:
    //   delay = t_ack − (telegram-complete − frame end)
    //         = 15 BT − (bitSamplingOffsetUs + 2 BT)
    [[gnu::always_inline]] uint64_t telegramCompleteToAckStartDelayUs() const noexcept
    {
        return _config.serialBitTimeUs
                   * (ACK_START_FROM_FRAME_END_BT - TELEGRAM_COMPLETE_FROM_FRAME_END_BT)
               - _config.bitSamplingOffsetUs;
    }

    static void timerAlarmShim(void* context);
    static void rxEdgeShim(void* context);
};

} // namespace physical
} // namespace knx

// ── Platform alias ───────────────────────────────────────────────────────────────────
// BitBangDriverTimerIsr is the name used everywhere in application code.
// The IsrPolicy template parameter is an implementation detail.
#ifdef ESP_PLATFORM
#include "knx/physical/isr_hal_policy_espidf.hpp"
#else
#include "knx/physical/isr_hal_policy_virtual.hpp"
#endif

namespace knx {
namespace physical {

#ifdef ESP_PLATFORM
using BitBangDriverTimerIsr = BitBangDriverTimerIsrT<EspIdfIsrHalPolicy>;
#else
using BitBangDriverTimerIsr = BitBangDriverTimerIsrT<VirtualIsrHalPolicy>;
#endif

} // namespace physical
} // namespace knx
