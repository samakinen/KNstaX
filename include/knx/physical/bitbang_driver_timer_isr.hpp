// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bitbang_driver_timer_isr.hpp
 * @brief Interrupt-driven TP1 character and acknowledge engine (bit-bang).
 *
 * The state machine, its invariants, the timing schedule and the event
 * contract with the consumer are specified in
 * docs/reference/tp1_bitbang_isr.md. Comments here state what each piece
 * does and why; the document states how the pieces fit together. When the
 * two disagree, the code is wrong until the document is changed on purpose.
 */

#pragma once

#include "knx/config.hpp"
#include "knx/datalink/tp1_dl_common.hpp"
#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/bitbang_timing_stats.hpp"
#include "knx/physical/group_ack_filter.hpp"
#include "knx/physical/timer_gpio_hal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

/// Run-time checking of the state invariants and of the event contract
/// (docs/reference/tp1_bitbang_isr.md §5 and §6) after every interrupt and
/// every send(). On by default in host builds, where every TP1 test then
/// enforces the specification; off on the target, where it would cost ISR
/// time and has no way to report.
#ifndef KNX_TP1_BITBANG_SELF_CHECK
#ifdef ESP_PLATFORM
#define KNX_TP1_BITBANG_SELF_CHECK 0
#else
#define KNX_TP1_BITBANG_SELF_CHECK 1
#endif
#endif

namespace knx {
namespace physical {

/// Production-safe link-quality counters maintained by the ISR.
///
/// Each field is a uint32_t incremented from interrupt context on an already
/// exceptional path, with no timer read and no logging, so the struct stays
/// enabled in release builds. Together they answer what a bare "ACK timeout"
/// cannot: was the acknowledgement absent from the bus, present but rejected
/// by start-bit validation, or received corrupted?
struct BitBangLinkCounters {
    /// Start bit rejected by the validation re-read, outside an ACK window.
    /// A growing value means ISR latency pushed the re-read past the 35 µs
    /// dominant pulse and characters are being dropped.
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
    /// Timer compares that fired for a deadline no longer armed. Stays at zero
    /// when the timer HAL discards a pending compare on rearm; a nonzero value
    /// means it does not, and the owner/deadline guard is what keeps the
    /// schedule correct.
    uint32_t staleTimerAlarms{0};

    void reset() noexcept { *this = BitBangLinkCounters{}; }
};

template<typename IsrPolicy>
class BitBangDriverTimerIsrT {
public:
    /// ISR→task events. The sequences they form are the consumer contract
    /// (docs/reference/tp1_bitbang_isr.md §6).
    enum class MessageType : uint8_t {
        /// One received octet of a telegram. data = the octet.
        Data = 2,
        /// Parity error in the character being received (outside an ACK window).
        ParityError = 3,
        /// Stop bit not recessive (outside an ACK window). Replaces Data.
        FramingError = 4,
        /// Telegram complete. data = RX error flags | END_FLAG_ANSWERED_BUSY.
        End = 5,
        /// Outcome of the ACK window of the frame just sent: data = the
        /// acknowledge octet, or ACK_BYTE_NONE when none was received intact.
        /// Terminal for that send().
        TxAckResponse = 7,
        /// A dominant edge while this node sent a recessive bit: arbitration
        /// lost. The frame is retried by the ISR; not terminal.
        Collision = 8,
        /// Frame sent and no ACK window follows. Terminal for that send().
        TxComplete = 11,
        /// The device is addressed by the telegram being received; the
        /// acknowledge decision is latched. data = ACK or BUSY octet.
        EarlyAckArmed = 12,
        /// An acknowledge was due but the transmitter was busy. data = TX state.
        DlAckSuppressed = 13,
        /// The DL-ACK character starts on the bus. data = the octet.
        DlAckTxStart = 14,
        /// The DL-ACK character is complete. data = 1 if the local bus echo
        /// was seen during a dominant bit, 0 if not.
        DlAckDone = 15,
        /// Diagnostics only (CONFIG_KNX_TP1_BITBANG_TIMING_STATS): the last
        /// stop bit of a frame has completed, whether or not an ACK window
        /// follows. data = worst per-bit alarm lateness in µs, saturated at 255.
        TxFrameEnd = 16,
        /// The frame lost arbitration MAX_TX_ARBITRATION_ATTEMPTS times and
        /// was abandoned. data = the number of losses. Terminal for that send().
        TxArbitrationLost = 17,
    };

    /// One ISR→task event.
    ///
    /// With timing diagnostics enabled the event carries its own timestamps,
    /// captured in the ISR when it is queued, because the task formats it
    /// later and live fields would by then describe a different event. They
    /// are raw free-running timer µs truncated to 32 bits.
    struct Message {
        MessageType type{MessageType::Data};
        uint8_t data{0};
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
        uint32_t t0{0};  ///< event start instant (see MessageType docs)
        uint32_t t1{0};  ///< event end instant, 0 when the event is punctual
#endif
    };

#ifndef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    // With diagnostics off the message is exactly the two protocol octets.
    static_assert(sizeof(Message) == 2,
                  "diagnostic timestamps must not exist in production builds");
#endif

    static constexpr uint8_t ACK_BYTE_NONE = 0xFF;
    static constexpr uint8_t ACK_BYTE_ACK = 0xCC;
    static constexpr uint8_t ACK_BYTE_NACK = 0x0C;
    static constexpr uint8_t ACK_BYTE_BUSY = 0xC0;
    static constexpr uint8_t ACK_BYTE_NACK_BUSY = 0x00;

    /// Set in an End message's data octet, next to the RX error flags, when
    /// the telegram was refused with BUSY. The sender repeats such a frame
    /// (03_02_02 §2.4.2), so the consumer must not pass this copy upwards.
    static constexpr uint8_t END_FLAG_ANSWERED_BUSY = 0x80;

    /// Group addresses the ISR acknowledge matcher holds: the data link's
    /// whole table (Tp1DataLinkLayer::MAX_GROUP_ADDRESSES).
    static constexpr uint16_t MAX_ACK_GROUP_ADDRESSES = 256;
    /// Additional Individual Addresses acknowledged besides the own one
    /// (tunnel addresses of a KNXnet/IP interface).
    static constexpr uint8_t MAX_ACK_INDIVIDUAL_ADDRESSES = 16;
    static_assert(MAX_ACK_GROUP_ADDRESSES >= config::ADDRESS_TABLE_ENTRIES
                      && MAX_ACK_INDIVIDUAL_ADDRESSES >= config::ADDITIONAL_INDIVIDUAL_ADDRESSES,
                  "the ISR acknowledge tables must hold what the data link layer can publish");

    /// Tries for one send() before a frame that keeps losing bitwise
    /// arbitration is abandoned. Losing arbitration is normal bus access, not
    /// a transmission error, so it is retried here rather than charged to the
    /// data link's BUSY or NAK budget; the bound only keeps a jammed bus from
    /// looping forever.
    static constexpr uint8_t MAX_TX_ARBITRATION_ATTEMPTS = 10;

    /// @thread_safety Task context; not concurrently with any other call.
    bool init(const knx_timer_gpio_hal_t& hal, const BitBangConfig& config);
    /// @thread_safety Task context; not concurrently with any other call.
    void shutdown();

    /// Stage a frame for transmission.
    ///
    /// @return false when the transmitter is busy (another frame is staged or
    ///         on the wire, or an ACK window is open) or the frame is empty or
    ///         longer than MAX_TELEGRAM_SIZE.
    /// @post On true, exactly one terminal event (TxComplete, TxAckResponse or
    ///       TxArbitrationLost) follows for this frame.
    /// @thread_safety Task context. Masks the driver's interrupts for the
    ///                check-and-stage sequence (bounded by a ≤263-octet copy).
    bool send(std::span<const uint8_t> frame);

    /// @thread_safety Task context; a 16-bit store the ISR reads atomically.
    void setOwnAddress(uint16_t addr) { _ownAddressRaw = addr; }

    // ── Acknowledge decision inputs (task context) ─────────────────────────
    // The DL-ACK decision is made inside the ISR while the header streams in;
    // a task cannot meet the 15-bit-time t_ack deadline. Tasks only publish
    // the inputs here.

    /// Publish the group addresses the ISR acknowledges: those of the device's
    /// group address table (03_02_02 §2.4.2).
    ///
    /// Sorted here, in task context, so the ISR can binary-search it (at most
    /// eight probes for 256 entries). Published under the ISR lock so the
    /// count and the entries change together; the section is bounded by a
    /// 512-octet copy.
    void setAckGroupAddresses(const uint16_t* addresses, size_t count) {
        if (count > MAX_ACK_GROUP_ADDRESSES) {
            count = MAX_ACK_GROUP_ADDRESSES;
        }
        std::array<uint16_t, MAX_ACK_GROUP_ADDRESSES> sorted{};
        std::copy_n(addresses, count, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count));

        _isrHal.lockFromTask();
        std::copy_n(sorted.begin(), count, _ackGroupAddresses.begin());
        _ackGroupAddressCount = static_cast<uint16_t>(count);
        _isrHal.unlockFromTask();
    }

    /// Publish the further Individual Addresses the device assumes and
    /// therefore acknowledges: a KNXnet/IP Tunnelling server's tunnel
    /// addresses (03_08_04 §2.2.2). Same publication rule as
    /// setAckGroupAddresses(); searched linearly.
    void setAckIndividualAddresses(const uint16_t* addresses, size_t count) {
        if (count > MAX_ACK_INDIVIDUAL_ADDRESSES) {
            count = MAX_ACK_INDIVIDUAL_ADDRESSES;
        }
        _isrHal.lockFromTask();
        std::copy_n(addresses, count, _ackIndividualAddresses.begin());
        _ackIndividualAddressCount = static_cast<uint8_t>(count);
        _isrHal.unlockFromTask();
    }

    /// Answer addressed telegrams with BUSY instead of ACK.
    void setLocalBusy(bool busy) { _localBusy = busy; }

#if KNX_FEATURE_COUPLER
    /// A coupler's group acknowledge rule (see GroupAckFilter), or null for
    /// an end device. Consulted only for group frames the address table does
    /// not already acknowledge: the conditions are OR-ed (03/03/03
    /// §2.4.2.4.4). The filter is owned by the coupler, must outlive its
    /// publication here and must live in internal RAM; a pointer store is a
    /// single write.
    void setGroupAckFilter(const GroupAckFilter* filter) { _groupAckFilter = filter; }
#endif

    /// Bus-monitor mode: acknowledge nothing, so a monitoring tool does not
    /// change the traffic it observes.
    void setBusMonitorMode(bool enabled) { _busMonitor = enabled; }

    /// Consumer side of the event ring.
    /// @thread_safety Exactly one task context at a time.
    bool popMessage(Message& outMessage);

    using QueueNotifyCallback = void(*)(void* context);

    /// Install or clear the ISR→task wake-up.
    ///
    /// Written under the ISR lock, so the interrupt never pairs a callback
    /// with another call's context, and no interrupt uses the old pair once
    /// this returns: a caller may clear it and then free the context. (The
    /// targets are single-core, so no interrupt can be part-way through the
    /// old callback while task code runs.)
    void setQueueNotifyCallback(QueueNotifyCallback callback, void* context) {
        _isrHal.lockFromTask();
        _queueNotifyCallback = callback;
        _queueNotifyContext = context;
        _isrHal.unlockFromTask();
    }

    /// Interrupt entry points. Never call from task context.
    void onTimerAlarm();
    void onRxEdge();

    uint32_t droppedMessageCount() const noexcept;

    /// Link-quality counters (see BitBangLinkCounters). Readable from task
    /// context; single increments are not atomic, which is acceptable for
    /// counters only compared against themselves over time.
    const BitBangLinkCounters& linkCounters() const noexcept { return _linkCounters; }

#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    /// Accumulated ISR timing statistics; best-effort consistent from task context.
    const BitBangTimingStats& timingStats() const noexcept { return _timingStats; }

    /// Reset all timing accumulators. Single updates are not atomic.
    void resetTimingStats() noexcept { _timingStats.reset(); }
#endif

#if KNX_TP1_BITBANG_SELF_CHECK
    /// Turn the self-check off for a white-box test that writes internal state
    /// directly and so produces states the ISR itself never would. init()
    /// turns it back on.
    void setSelfCheckEnabled(bool enabled) noexcept { _selfCheck.enabled = enabled; }
#endif

private:
    // Full L_Data_Extended frame: 8-octet header + 254-octet APDU + check octet.
    // The software bit-bang backend runs on ESP32 and host simulations, not on
    // the small parts, so it stays sized for the longest frame whatever
    // KNX_MAX_APDU_LENGTH is; the MAC layer drops anything above the
    // configured length before it reaches the upper layers.
    static constexpr size_t MAX_TELEGRAM_SIZE = 263;
    static_assert(MAX_TELEGRAM_SIZE >= config::MAX_TP1_FRAME_LENGTH,
                  "the bit-bang telegram buffer must hold the configured longest frame");
    // Power of two so ring wrap-around is an AND mask and the free-running
    // 32-bit producer/consumer indices stay exact across overflow.
    static constexpr size_t MESSAGE_QUEUE_CAPACITY = 128;
    static_assert((MESSAGE_QUEUE_CAPACITY & (MESSAGE_QUEUE_CAPACITY - 1u)) == 0u,
                  "message queue capacity must be a power of two");
    static constexpr uint32_t MESSAGE_QUEUE_MASK = MESSAGE_QUEUE_CAPACITY - 1u;
    // Depth at which a queued message wakes the consumer even when its type
    // would not. A 263-octet telegram produces more Data messages than the
    // ring holds, so waiting for its End would drop the tail.
    static constexpr uint32_t MESSAGE_QUEUE_NOTIFY_WATERMARK = MESSAGE_QUEUE_CAPACITY / 2u;

    /// Receive character state (docs §3.1).
    enum RxState : uint8_t {
        RX_IDLE = 0,              ///< no character in progress, no RX alarm
        RX_START_BIT_PENDING = 1, ///< edge seen, start bit not yet validated
        RX_RECEIVE = 2,           ///< sampling data, parity and stop bits
        RX_WAIT_MORE_DATA = 3,    ///< character done; next start bit or telegram end
    };

    /// Transmit state (docs §3.2).
    enum TxState : uint8_t {
        TX_IDLE = 0,                     ///< nothing staged, no TX alarm
        TX_WAITING_SLOT = 1,             ///< a frame waits for the bus, or a DL-ACK for t_ack
        TX_SENDING_ZERO_ACTIVE = 2,      ///< driving the dominant part of a '0'
        TX_SENDING_ZERO_EQUALIZATION = 3,///< released, rest of a '0' bit cell
        TX_SENDING_ONE = 4,              ///< released, recessive bit; collisions detected
        TX_WAITING_ACK = 5,              ///< frame sent, ACK window open
    };

    /// Which logical deadline the hardware compare is programmed for.
    ///
    /// onTimerAlarm() consumes this record and services the matching deadline
    /// only if it is still the one programmed, so an alarm replaced between
    /// latching and dispatch is never serviced under the wrong timeline.
    enum class AlarmOwner : uint8_t {
        None,
        Rx,
        Tx,
    };

    static constexpr uint8_t RX_ERROR_PARITY = (1u << 0);
    static constexpr uint8_t RX_ERROR_FRAMING = (1u << 1);

    // ── Hot state ──────────────────────────────────────────────────────────
    // Everything an interrupt touches on its common path sits at the start of
    // the object: within the 2 KiB immediate-offset reach of RV32 loads and
    // stores from the object base, and packed into as few cache lines as
    // possible on targets whose RAM is cached. Cold tables and the event ring
    // follow. Keep new per-interrupt fields in this block.
    bool       _initialized{false};
    uint8_t    _rxState{RX_IDLE};
    uint8_t    _txState{TX_IDLE};
    AlarmOwner _armedAlarmOwner{AlarmOwner::None};
    /// A dominant edge was seen since the last bit sample.
    uint8_t    _rxZeroDetected{0};
    uint8_t    _rxByte{0};
    /// Bit position the next RX sample belongs to: 0..7 data, 8 parity, 9
    /// stop; -1 while no character is being sampled.
    int8_t     _rxBitPosition{-1};
    /// Bit position the next TX bit-end services; same numbering as RX.
    int8_t     _txBitPosition{-1};
    uint8_t    _txCurrentByte{0};
    uint8_t    _rxDominantLevel{1};
    /// RX_ERROR_* flags of the telegram being received (or of the ACK
    /// character, while TX_WAITING_ACK).
    uint8_t    _rxErrors{0};
    /// Running XOR of every octet of the telegram, check octet included. A
    /// correct frame leaves 0xFF (03_02_02 §2.2.1).
    uint8_t    _rxCheckXor{0};
    uint8_t    _rxDstHi{0};
    uint8_t    _rxDstLo{0};
    /// ACK or BUSY decided for the telegram being received; 0 = not addressed.
    uint8_t    _rxAckIntent{0};
    /// Octet the armed or in-flight DL-ACK carries; nonzero exactly while
    /// _sendingAck is set.
    uint8_t    _ackSlotByte{0};
    bool       _sendingAck{false};
    /// A Data or error message of the current telegram has been queued, so
    /// an End is owed.
    bool       _rxTelegramOpen{false};
    bool       _rxExtendedFrame{false};      ///< CTRL bits 7..6 == 00
    bool       _rxExtGroupAddressed{false};  ///< CTRLE bit 7 of an extended frame
    bool       _dlAckEchoSeen{false};
    bool       _ackStartEdgeSeen{false};
    bool       _ackStartValidated{false};
    /// _lastCharStartUs is the scheduled start of a character this node
    /// drove, not the stamped edge of a received one.
    bool       _lastCharStartIsOwn{false};
    uint8_t    _txArbitrationLosses{0};
    uint16_t   _rxByteCount{0};
    uint16_t   _txBytePosition{0};
    uint16_t   _txTelegramLength{0};
    uint16_t   _ownAddressRaw{0};

    // Timing, copied from the configuration and derived once in init() so
    // the interrupt reads a precomputed constant instead of multiplying.
    uint32_t _bitUs{0};               ///< serial bit time
    uint32_t _sampleOffsetUs{0};      ///< sample point after a character's start edge
    uint32_t _startValidationUs{0};   ///< start-bit re-read after its edge
    uint32_t _zeroActiveUs{0};        ///< dominant part of a transmitted '0'
    uint32_t _zeroEqualizationUs{0};  ///< recessive rest of a transmitted '0'
    uint32_t _waitMoreDataUs{0};      ///< stop-bit sample → telegram complete (3 bits)
    uint32_t _staleCharGapUs{0};      ///< start-to-start gap that cannot be within one telegram
    uint32_t _ackWindowUs{0};         ///< last TX bit-end → ACK window closes
    uint32_t _ackSlotDelayUs{0};      ///< telegram complete → DL-ACK start bit
    uint32_t _busyHoldoffUs{0};       ///< BUSY start bit → earliest repetition
    uint32_t _txEarlyEdgeUs{0};       ///< end of a TX bit cell: an edge here is the next bit's
    uint32_t _reactionLatencyUs{0};   ///< bus edge → own drive on the bus, beyond the schedule
    uint32_t _firstSampleDelayUs{0};  ///< start-bit re-read → D0 sample
    uint32_t _txIdleWaitUs{0};        ///< last start bit → own start bit, for the staged frame

    uint64_t _armedAlarmUs{0};
    uint64_t _rxAlarmUs{0};
    uint64_t _txAlarmUs{0};
    /// Edge instant of the start bit being validated.
    uint64_t _rxEdgeUs{0};
    /// Start-bit instant of the most recent character on the bus: a
    /// validated received character or one this node drove (DL-ACKs
    /// included). The single reference for the CSMA idle gate.
    uint64_t _lastCharStartUs{0};
    /// Earliest instant the staged frame may start after a BUSY acknowledge,
    /// or 0 (03_02_02 §2.4.2 and A.8).
    uint64_t _txHoldoffUntilUs{0};

    /// Compile-time hardware access for the hot path (see isr_hal_policy_*.hpp).
    IsrPolicy _isrHal{};

    // Event ring: single producer (the ISR, at _msgTail), single consumer
    // (one task, at _msgHead). Free-running 32-bit indices, entries addressed
    // modulo the power-of-two capacity, no shared count — a count would need a
    // read-modify-write from both sides.
    std::atomic<uint32_t> _msgHead{0};
    std::atomic<uint32_t> _msgTail{0};
    QueueNotifyCallback _queueNotifyCallback{nullptr};
    void* _queueNotifyContext{nullptr};

    // Acknowledge decision inputs, written from task context (see setters).
    volatile bool     _localBusy{false};
    volatile bool     _busMonitor{false};
    volatile uint8_t  _ackIndividualAddressCount{0};
    volatile uint16_t _ackGroupAddressCount{0};
#if KNX_FEATURE_COUPLER
    const GroupAckFilter* volatile _groupAckFilter{nullptr};
#endif

    // ── Cold state ─────────────────────────────────────────────────────────
    std::atomic<uint32_t> _msgDropped{0};
    BitBangLinkCounters _linkCounters{};
    std::array<Message, MESSAGE_QUEUE_CAPACITY> _messages{};
    std::array<uint16_t, MAX_ACK_INDIVIDUAL_ADDRESSES> _ackIndividualAddresses{};
    /// Sorted ascending by setAckGroupAddresses().
    std::array<uint16_t, MAX_ACK_GROUP_ADDRESSES> _ackGroupAddresses{};
    std::array<uint8_t, MAX_TELEGRAM_SIZE> _txBuffer{};
    /// Full HAL vtable: configure/install/start/stop (slow path only).
    knx_timer_gpio_hal_t _hal{};
    BitBangConfig _config{};

    // ── Optional TX/RX timing diagnostics (bench bring-up only) ──────────────
    // Gated by CONFIG_KNX_TP1_BITBANG_TIMING_STATS. When disabled every diag*
    // helper is an empty inline and diagNowUs() folds to 0, so the ISR makes
    // no extra timer reads. Values are handed to queueMessage() when their
    // event is published; the fields only bridge "event begins" to "event is
    // queued".
#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    uint64_t _txFrameStartUs{0};  ///< first start bit of the TX frame in flight
    uint32_t _txMaxLateUs{0};     ///< worst per-bit timer-alarm lateness this frame
    uint64_t _rxFrameStartUs{0};  ///< first start bit of the RX telegram in flight
    uint64_t _dlAckStartUs{0};    ///< instant the DL-ACK in flight began
    BitBangTimingStats _timingStats{};

    [[gnu::always_inline]] uint64_t diagNowUs() const noexcept { return timerNow(); }

    // Worst per-bit alarm lateness while driving TX bits. The extra timer read
    // per bit is the reason this family is excluded from production builds.
    [[gnu::always_inline]] void diagTxBitLateness(uint64_t firedAlarmUs) noexcept {
        if (isDrivingBits()) {
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
    /// message's data octet; at 255 µs the exact value no longer matters.
    [[gnu::always_inline]] uint8_t diagTxMaxLateSaturated() const noexcept {
        return static_cast<uint8_t>(_txMaxLateUs > 255u ? 255u : _txMaxLateUs);
    }
    [[gnu::always_inline]] uint64_t diagRxFrameStartUs() const noexcept { return _rxFrameStartUs; }
    [[gnu::always_inline]] uint64_t diagDlAckStartUs() const noexcept { return _dlAckStartUs; }
    [[gnu::always_inline]] void diagQueueTxFrameEnd(uint64_t txEndUs) noexcept {
        (void)queueMessage(MessageType::TxFrameEnd, diagTxMaxLateSaturated(),
                           _txFrameStartUs, txEndUs);
    }
#else
    [[gnu::always_inline]] uint64_t diagNowUs() const noexcept { return 0; }
    [[gnu::always_inline]] void diagTxBitLateness(uint64_t) noexcept {}
    [[gnu::always_inline]] void diagTxFrameStart(uint64_t) noexcept {}
    [[gnu::always_inline]] void diagRxFrameStart(uint64_t) noexcept {}
    [[gnu::always_inline]] void diagDlAckStart(uint64_t) noexcept {}
    [[gnu::always_inline]] uint64_t diagRxFrameStartUs() const noexcept { return 0; }
    [[gnu::always_inline]] uint64_t diagDlAckStartUs() const noexcept { return 0; }
    [[gnu::always_inline]] void diagQueueTxFrameEnd(uint64_t) noexcept {}
#endif

#if KNX_TP1_BITBANG_SELF_CHECK
    /// Independent model of the event contract, advanced by every queued
    /// message (whether or not the ring had room) and every accepted send().
    struct SelfCheckState {
        bool enabled{true};
        bool sendOutstanding{false};
        bool telegramOpen{false};
        bool ackArmedInTelegram{false};
        bool dlAckInFlight{false};
    };
    SelfCheckState _selfCheck{};

    void selfCheckState(const char* where) const noexcept;
    void selfCheckMessage(MessageType type) noexcept;
    void selfCheckSendAccepted() noexcept;
#else
    [[gnu::always_inline]] void selfCheckState(const char*) const noexcept {}
    [[gnu::always_inline]] void selfCheckMessage(MessageType) noexcept {}
    [[gnu::always_inline]] void selfCheckSendAccepted() noexcept {}
#endif

    bool queueMessage(MessageType type, uint8_t data,
                      uint64_t t0Us = 0, uint64_t t1Us = 0) noexcept;

    /// Even parity bit of @p byte, folded with shifts: RV32IMAC has no
    /// popcount instruction and __builtin_popcount becomes a library call.
    [[gnu::always_inline]] static uint8_t parityBit(uint8_t byte) noexcept
    {
        uint8_t v = static_cast<uint8_t>(byte ^ (byte >> 4));
        v = static_cast<uint8_t>(v ^ (v >> 2));
        v = static_cast<uint8_t>(v ^ (v >> 1));
        return static_cast<uint8_t>(v & 1u);
    }

    /// Whether queueing @p type wakes the consumer at once.
    ///
    /// Only messages that complete a unit of work do: a wake-up costs a
    /// context switch out of the bit-timing interrupt, charged to the latency
    /// of the next bit sample. The excluded types are published
    /// mid-transaction, and each is followed by one that notifies: Data by End,
    /// EarlyAckArmed by End, DlAckTxStart by DlAckDone, TxFrameEnd by a
    /// terminal event. The queue-depth watermark covers long telegrams.
    [[gnu::always_inline]] static bool messageNeedsNotify(MessageType type) noexcept
    {
        return type != MessageType::Data
            && type != MessageType::EarlyAckArmed
            && type != MessageType::DlAckTxStart
            && type != MessageType::TxFrameEnd;
    }

    [[gnu::always_inline]] bool isDrivingBits() const noexcept
    {
        return _txState == TX_SENDING_ZERO_ACTIVE
            || _txState == TX_SENDING_ZERO_EQUALIZATION
            || _txState == TX_SENDING_ONE;
    }

    // State handlers. Each returns the delay from the instant it was
    // scheduled for to its timeline's next alarm, or 0 for none; only
    // onTimerAlarm() turns that into a deadline (§4.1). TX handlers receive
    // that scheduled instant as nowUs.
    void rearmTimer();
    uint64_t processRxTimer();
    uint64_t processTxTimer(uint64_t nowUs);

    uint64_t processStartBit();
    uint64_t processReceiveBit();
    uint64_t rxHandleStop(uint8_t bit);
    uint64_t rxHandleTelegramComplete();
    void rxAcceptOctet(uint8_t octet);
    void finishRxTelegram(uint8_t endFlags = 0);
    void abandonReception();

    uint64_t sendBit(uint8_t bit);
    uint64_t processEndOfTxBit(uint64_t nowUs);
    uint64_t txHandleWaitingSlot(uint64_t nowUs);
    uint64_t txHandleWaitingAck();

    /// txEdgeLostPosition() results besides a bit position 0..8.
    static constexpr int8_t TX_EDGE_NOT_LOST = -1;
    static constexpr int8_t TX_EDGE_OUTSIDE_CHARACTER = -2;
    int8_t txEdgeLostPosition() const;
    void loseArbitration(int8_t lostPosition);
    void takeOverReceptionAfterArbitrationLoss(int8_t lostPosition);

    /// What to take off a delay that runs from _lastCharStartUs to something
    /// this node drives (§4.4). A received character was stamped late and
    /// the reaction to it is driven late, by a constant; a character this
    /// node drove carries the same lateness as the next one it drives.
    [[gnu::always_inline]] uint32_t referenceLatencyUs() const noexcept
    {
        return _lastCharStartIsOwn ? 0u : _reactionLatencyUs;
    }

    /// Reset the RX character sampler and drop its alarm. Per-telegram state
    /// is not touched: closing a telegram is finishRxTelegram()'s job.
    [[gnu::always_inline]] void resetRxSamplingState() noexcept
    {
        _rxState = RX_IDLE;
        _rxAlarmUs = 0;
        _rxByte = 0;
        _rxBitPosition = -1;
        _rxZeroDetected = 0;
    }

    /// Open the L2 acknowledgement window for the frame just transmitted. The
    /// ACK character is decoded by the RX byte sampler, which must start
    /// clean, errors included: a stale error would be reported against the
    /// next telegram and turn its DL-ACK into a NAK.
    [[gnu::always_inline]] void openAckWindow() noexcept
    {
        abandonReception();
        _rxErrors = 0;
        _ackStartEdgeSeen = false;
        _ackStartValidated = false;
        _txState = TX_WAITING_ACK;
    }

    /// Close the ACK window with the character just received and return the
    /// outcome. A character with a parity or framing error is not an
    /// acknowledgement: it is reported as ACK_BYTE_NONE, which the data link
    /// answers by repeating the frame — the spec-correct response to an
    /// acknowledgement that could not be read.
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

    /// Whether the frame in _txBuffer opens an ACK window after it is sent.
    ///
    /// "The L_Data service shall be an acknowledged datagram service" for
    /// every destination, group and broadcast included (03_02_02 §2.4.2); TP1
    /// has no meaningful ACK-request bit on the wire. The one exclusion, the
    /// individual broadcast, mirrors armAckIfAddressed().
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
            return true;  // group addressed, including broadcast 0/0/0
        }

        const uint16_t destRaw = static_cast<uint16_t>(
            (static_cast<uint16_t>(_txBuffer[destHiPos]) << 8) | _txBuffer[destHiPos + 1u]);
        return destRaw != 0xFFFFu;
    }

    /// Whether the telegram just received is "correct" (03_02_02 §2.4.2): a
    /// matching check octet and 8–23 octets for a standard or 9–263 for an
    /// extended frame, check octet included. Character errors are tested by
    /// the caller. The spec rules out comparing the length field with the
    /// real length before acknowledging (§2.4.2, Example 1).
    [[gnu::always_inline]] bool rxFrameIsCorrect() const noexcept
    {
        const uint16_t length = _rxByteCount;
        const bool lengthOk = _rxExtendedFrame ? (length >= 9u && length <= 263u)
                                               : (length >= 8u && length <= 23u);
        return lengthOk && _rxCheckXor == 0xFFu;
    }

    /// Binary search of the sorted group-address table, hand-written so the
    /// interrupt reaches no code outside IRAM.
    [[gnu::always_inline]] bool ackGroupTableContains(uint16_t destRaw) const noexcept
    {
        uint16_t low = 0;
        uint16_t high = _ackGroupAddressCount;
        while (low < high) {
            const uint16_t mid = static_cast<uint16_t>((low + high) / 2u);
            const uint16_t entry = _ackGroupAddresses[mid];
            if (entry == destRaw) {
                return true;
            }
            if (entry < destRaw) {
                low = static_cast<uint16_t>(mid + 1u);
            } else {
                high = mid;
            }
        }
        return false;
    }

    [[gnu::always_inline]] bool ackIndividualTableContains(uint16_t destRaw) const noexcept
    {
        const uint8_t count = _ackIndividualAddressCount;
        for (uint8_t i = 0; i < count; ++i) {
            if (_ackIndividualAddresses[i] == destRaw) {
                return true;
            }
        }
        return false;
    }

    /// Acknowledge decision, taken the moment the destination address and
    /// its type are known (03_02_02 §2.4.2). Latches ACK or BUSY in
    /// _rxAckIntent; rxHandleTelegramComplete() downgrades it to NAK if the
    /// telegram turns out not to be correct.
    [[gnu::always_inline]] void armAckIfAddressed(bool isGroupAddressed) noexcept
    {
        if (_busMonitor) {
            return;
        }

        const uint16_t destRaw = static_cast<uint16_t>((_rxDstHi << 8) | _rxDstLo);
        bool addressed;
        if (isGroupAddressed) {
            // Addressed when the destination is in the group address table or
            // is the broadcast 0/0/0. There is no ACK-request bit on the TP1
            // wire: certified senders keep CTRL bits 1..0 at zero.
            addressed = destRaw == 0x0000u || ackGroupTableContains(destRaw);
#if KNX_FEATURE_COUPLER
            if (!addressed) {
                const GroupAckFilter* coupler = _groupAckFilter;
                addressed = coupler != nullptr && coupler->acknowledges(destRaw);
            }
#endif
        } else {
            // Own or assumed Individual Address; the individual broadcast
            // FFFFh is never acknowledged.
            addressed = destRaw != 0xFFFFu
                && ((_ownAddressRaw != 0 && destRaw == _ownAddressRaw)
                    || ackIndividualTableContains(destRaw));
        }

        if (addressed) {
            const uint8_t ackByte = _localBusy ? ACK_BYTE_BUSY : ACK_BYTE_ACK;
            _rxAckIntent = ackByte;
            (void)queueMessage(MessageType::EarlyAckArmed, ackByte);
        }
    }

    // Hot-path hardware delegates; IsrPolicy methods are always_inline on the
    // target, so each collapses to a few instructions at its call site.
    [[gnu::always_inline]] uint64_t timerNow() const noexcept
        { return _isrHal.timerNow(); }
    [[gnu::always_inline]] void rearmTimerAbs(uint64_t absUs) noexcept
        { _isrHal.rearmTimerAbs(absUs); }
    [[gnu::always_inline]] void setTxDominantFast() noexcept
        { _isrHal.setTxDominant(); }
    [[gnu::always_inline]] void setTxRecessiveFast() noexcept
        { _isrHal.setTxRecessive(); }
    [[gnu::always_inline]] bool rxIsDominant() const noexcept
        { return _isrHal.readRxLevel() == static_cast<int>(_rxDominantLevel); }

    // ── Protocol quantities (03_02_02) ───────────────────────────────────────
    /// The L2 acknowledge starts t_ack = 15 bit times after the frame end.
    static constexpr uint32_t ACK_START_FROM_FRAME_END_BT = 15u;
    /// The telegram-complete timeout runs 3 bit times after the stop-bit
    /// sample, which is bitSamplingOffset into the frame's last bit cell —
    /// so 2 bit times plus the sampling offset after the frame end.
    static constexpr uint32_t TELEGRAM_COMPLETE_FROM_FRAME_END_BT = 2u;
    /// A character is 11 bit times; the inter-character gap is 2.
    static constexpr uint32_t CHARACTER_BT = 11u;
    static constexpr uint32_t INTER_CHARACTER_GAP_BT = 2u;
    /// Idle line before a system or urgent frame and before any repeated
    /// frame; normal and low frames wait 3 bit times more (03_02_02 §2.3).
    static constexpr uint32_t BUS_IDLE_FLOOR_BT = 50u;
    /// While transmitting, an edge in the last 1/4 of a bit cell belongs to
    /// the next cell (§3.4). A sender may be up to 7 µs ahead of this node
    /// and still send in parallel (08_02_02 §8.2.2); the rest is interrupt
    /// jitter. The split is not in the middle: interrupt latency only ever
    /// makes an edge look later than it was, and this node's own echo comes
    /// at the start of a cell.
    static constexpr uint32_t TX_EARLY_EDGE_DIVISOR = 4u;
    /// The ACK window stays open this long after the frame end.
    static constexpr uint32_t ACK_WINDOW_BT = 35u;
    /// Wait after a BUSY acknowledge, from the end of the BUSY character
    /// (03_02_02 §2.4.2 and A.8).
    static constexpr uint32_t BUSY_HOLDOFF_BT = 150u;
    /// Start-to-start distance beyond which an edge cannot be the next
    /// character of the same telegram: a character plus the gap is 13 bit
    /// times; 16 leaves margin for late interrupts.
    static constexpr uint32_t STALE_CHARACTER_GAP_BT = 16u;

    static void timerAlarmShim(void* context);
    static void rxEdgeShim(void* context);
};

} // namespace physical
} // namespace knx

// ── Platform alias ───────────────────────────────────────────────────────────
// BitBangDriverTimerIsr is the name used everywhere outside this file; the
// IsrPolicy template parameter is an implementation detail.
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
