// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tpuart_medium_backend.hpp
 * @brief TP1 medium backend for a transceiver with a host UART (Siemens TP-UART2)
 *
 * The chip owns bit timing, bus access, collisions, sending the L2
 * acknowledge, observing the acknowledge of our own frame, and repetition.
 * The backend owns the host protocol over a knx_uart_hal_t, the acknowledge
 * decision for group addresses (made from the UART receive interrupt), and
 * the chip's lifecycle and health. Design: docs/design/cortex-m-tpuart2-support.md §3.
 *
 * Concurrency model
 * -----------------
 * Two contexts:
 *
 *   - Interrupt context: the UART HAL's RX callback (onRxOctet). It assembles
 *     indications into a fixed single-producer, single-consumer ring of frame
 *     slots, decides the acknowledge and writes U_AckInformation, and records
 *     confirmations and chip indications in atomics. It never allocates, locks
 *     or logs, and it runs from RAM where the platform needs it (KNX_ISR_RAM).
 *   - Main context: every Tp1MediumBackend method. service() drains the ring
 *     and turns what the interrupt recorded into Tp1RxEvents, exactly as the
 *     bit-bang pump does, so Tp1MacPhysical sees the same event contract.
 *
 * The acknowledge tables are written by the main context and read by the
 * interrupt under a sequence counter. An interrupt that meets a table being
 * rewritten does not acknowledge that frame; the sender repeats it. This
 * needs no interrupt masking, and it is also correct where the interrupt runs
 * on another core.
 *
 * Flash erases (platform::FlashEraseGuard)
 * -----------------------------------------
 * A product that masks interrupts while its flash erases (instead of keeping
 * the receive path in RAM) passes the backend to FlashLog::setEraseGuard().
 * Before each erase the backend sends U_ActivateBusyMode: the chip answers
 * every frame addressed to the device BUSY, so its sender repeats it. Octets
 * that arrive late, after the erase, are discarded until the host link has
 * been silent for FRAME_GAP_US (it then knows where a frame starts again);
 * only then does service() send U_ResetBusyMode. The chip also leaves busy
 * mode by itself after 700 ms. TP-UART2 manual §3.2.3.1.5 and §3.2.3.1.6.
 *
 * Protocol values: Siemens TP-UART2 technical manual, cross-checked against
 * thelsing/knx (tpuart_data_link_layer.cpp). The table in the design note
 * lists them. Items still to confirm on a real chip are in §3.8 there.
 */

#pragma once

#include "knx/config.hpp"
#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/physical/uart_hal.h"
#include "knx/platform/flash_erase_guard.hpp"
#include "knx/platform/platform.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace knx {
namespace physical {

/**
 * @brief The transceiver family, for the few host-protocol differences
 *
 * Only the TP-UART2 is implemented. An NCN5120 differs in U_SetAddress (one
 * more dummy octet) and in how repetition counts are set (U_SetRepetition,
 * 0xF2 + 3 octets); it becomes another value here once a board can test it.
 */
enum class TpUartFamily : uint8_t {
    TpUart2,
};

struct TpUartConfig {
    TpUartFamily family{TpUartFamily::TpUart2};
    /// Host link; 19200 8E1 for a TP-UART2 (verify against the board, B5).
    uint32_t baud{19200u};
    knx_uart_hal_parity_t parity{KNX_UART_HAL_PARITY_EVEN};
    /// Repetitions the chip makes after BUSY and after NACK or no acknowledge.
    /// The KNX defaults are 3 and 3 (03_02_02 §2.4.2).
    uint8_t busyRepetitions{3u};
    uint8_t nackRepetitions{3u};
    /// Octets the transmit path keeps queued in the UART at most (at least
    /// 2). It does not bound the acknowledge's latency: the main context
    /// writes nothing while a received frame waits for its acknowledge
    /// decision, so the U_AckInformation finds the UART empty.
    uint8_t txQueueLimitOctets{4u};
    /// No L_Data.con within this time: the frame is reported as a deadline
    /// miss and the chip is reset (it is assumed lost).
    uint32_t confirmTimeoutMs{1000u};
    /// U_State.req interval while idle. Two unanswered requests in a row
    /// report the link Down.
    uint32_t statePollIntervalMs{1000u};
    /// Wait for U_Reset.ind after each U_Reset.req, and how often to try.
    /// The H8i8o's TP-UART2 answered after 6.9 ms (15 Nov 2026), so 10 ms was too tight.
    uint32_t resetTimeoutMs{50u};
    uint8_t resetAttempts{3u};
};

/// Counters for diagnostics and tests. Interrupt-side counts are read with
/// relaxed atomics; a snapshot is consistent per field, not across fields.
struct TpUartStatistics {
    uint32_t framesReceived{0};       ///< correct frames put into the ring
    uint32_t checksumErrors{0};       ///< frames dropped for a bad checksum
    uint32_t octetErrorFrames{0};     ///< frames dropped for parity, framing, noise or overrun
    uint32_t oversizedFrames{0};      ///< frames longer than a ring slot
    uint32_t partialFramesDiscarded{0}; ///< frames cut off by a gap of more than 2 ms
    uint32_t rxOverflowDrops{0};      ///< correct frames dropped because the ring was full
    uint32_t unexpectedOctets{0};     ///< octets outside a frame that are no known indication
    uint32_t acksSent{0};             ///< U_AckInformation, addressed
    uint32_t busySent{0};             ///< U_AckInformation, addressed + busy
    uint32_t ackDecisionsDeferred{0}; ///< no acknowledge: the table was being rewritten
    uint32_t echoesDropped{0};        ///< our own frames the chip passed back while sending them
    uint32_t ackWriteFailures{0};     ///< the UART refused the acknowledge octet
    uint32_t erasesGuarded{0};        ///< flash erases taken with the chip in busy mode
    uint32_t erasesDeclined{0};       ///< erases asked to wait (a frame of ours or a reply outstanding)
    uint32_t resyncDiscards{0};       ///< octets dropped after an erase, before the link was silent
    uint32_t confirmsPositive{0};
    uint32_t confirmsNegative{0};
    uint32_t confirmTimeouts{0};
    uint32_t unexpectedConfirms{0};   ///< L_Data.con while no frame was outstanding
    uint32_t chipResets{0};           ///< U_Reset.ind the backend did not ask for
    uint32_t stateIndications{0};
    uint32_t missedStatePolls{0};
    uint8_t lastStateBits{0};         ///< last U_State.ind
};

class TpUartMediumBackend final : public Tp1MediumBackend, public platform::FlashEraseGuard {
public:
    /// Ring slots: one is always the frame being assembled, so the ring holds
    /// RX_SLOT_COUNT - 1 complete frames for service() to drain.
    static constexpr size_t RX_SLOT_COUNT = 4u;
    /// The TP-UART2 handles frames of up to 64 octets.
    static constexpr size_t CHIP_MAX_FRAME = 64u;
    static constexpr size_t MAX_FRAME = std::min(config::MAX_TP1_FRAME_LENGTH, CHIP_MAX_FRAME);
    /// Silence that ends a frame on the host link (TP-UART2 manual).
    static constexpr uint32_t FRAME_GAP_US = 2000u;

    // Host protocol octets (TP-UART2 technical manual).
    static constexpr uint8_t U_RESET_REQ = 0x01u;
    static constexpr uint8_t U_STATE_REQ = 0x02u;
    static constexpr uint8_t U_BUSMON_REQ = 0x05u;
    static constexpr uint8_t U_ACK_INFORMATION = 0x10u;
    static constexpr uint8_t U_ACK_ADDRESSED = 0x01u;
    static constexpr uint8_t U_ACK_BUSY = 0x02u;
    static constexpr uint8_t U_MXRSTCNT_REQ = 0x24u;
    /// Busy mode: the chip answers addressed frames BUSY for 700 ms, or until
    /// U_ResetBusyMode or a U_AckInformation (manual §3.2.3.1.5).
    static constexpr uint8_t U_ACTIVATE_BUSY_MODE = 0x21u;
    static constexpr uint8_t U_RESET_BUSY_MODE = 0x22u;
    /// Longest wait for U_ActivateBusyMode to leave the UART before an erase.
    static constexpr uint32_t BUSY_MODE_DRAIN_MS = 10u;
    /// Longest wait for our frame to finish feeding when an erase cannot wait.
    static constexpr uint32_t FORCED_ERASE_FEED_MS = 100u;
    /// Silence after an erase that puts the receive path back in step: the
    /// 2 ms frame gap plus a millisecond for the platform clock's resolution.
    static constexpr uint32_t RESYNC_SILENCE_MS = 3u;
    /// Longest the main context holds its writes for a received frame's
    /// acknowledge decision. Octets 0 to 5 take 6.8 ms on the bus; a frame
    /// still undecided after this was cut off.
    static constexpr uint32_t ACK_DECISION_HOLD_MS = 10u;
    static constexpr uint8_t U_L_DATA_START_CONTINUE = 0x80u;
    static constexpr uint8_t U_L_DATA_END = 0x40u;
    /// 28h on the TP-UART2 (confirmed on the H8i8o: frames to the address are
    /// then acknowledged by the chip). F1h is the NCN5120's; the TP-UART2
    /// ignores it and raises its receive-error flag.
    static constexpr uint8_t U_SET_ADDRESS_REQ = 0x28u;
    static constexpr uint8_t U_RESET_IND = 0x03u;
    static constexpr uint8_t U_STATE_IND_MASK = 0x07u;
    static constexpr uint8_t U_STATE_IND = 0x07u;
    static constexpr uint8_t U_STATE_TEMPERATURE_WARNING = 0x08u;
    static constexpr uint8_t L_DATA_CON_MASK = 0x7Fu;
    static constexpr uint8_t L_DATA_CON = 0x0Bu;
    static constexpr uint8_t L_DATA_CON_POSITIVE = 0x80u;

    /**
     * @param hal    The board's UART; must outlive the backend. Copied.
     * @param timing Clock for the timeouts and the reset wait; must outlive the backend.
     */
    TpUartMediumBackend(const knx_uart_hal_t& hal, platform::TimingPlatform& timing, const TpUartConfig& config = {});
    ~TpUartMediumBackend() override;

    TpUartMediumBackend(const TpUartMediumBackend&) = delete;
    TpUartMediumBackend& operator=(const TpUartMediumBackend&) = delete;
    TpUartMediumBackend(TpUartMediumBackend&&) = delete;
    TpUartMediumBackend& operator=(TpUartMediumBackend&&) = delete;

    /**
     * @brief Open the UART, reset the chip and configure it
     *
     * Blocks for the reset: U_Reset.req, then up to resetTimeoutMs for
     * U_Reset.ind, resetAttempts times. Then U_SetAddress, the repetition
     * counts, U_Busmon when asked for, and U_State.req.
     *
     * @return InvalidParameter for an incomplete HAL; ResourceUnavailable if
     *         the UART does not open or takes no requests; Timeout if the chip
     *         never answers the reset. On failure the UART is closed again.
     */
    util::Result<void> init(const Tp1MediumConfig& config) override;
    void close() override;

    /**
     * @brief Start sending one frame
     *
     * Copies the frame (FCS included) and starts feeding it to the chip as
     * U_L_Data pairs; service() continues. The outcome arrives as a
     * TxAckResponse (L_Data.con) or a TxAckDeadlineMiss event.
     *
     * @return Busy while a frame is outstanding; InvalidFrameSize outside 8 to
     *         MAX_FRAME octets; OperationNotSupported in bus monitor mode.
     */
    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) override;

    void setEventCallback(Tp1EventCallback callback, void* context) override;

    Tp1MediumState getState() const override;
    Tp1CapabilityProfile getCapabilities() const override;
    LinkState getLinkState() const override { return _linkState; }

    /// Entering sends U_Busmon.req; leaving resets the chip (the only way out).
    util::Result<void> setBusMonitorMode(bool enabled) override;
    /// Drains received frames, reports confirmations, continues sending,
    /// polls U_State, follows SAVE and resets a lost chip. Call it often:
    /// every main-loop pass, at least every few milliseconds while sending.
    util::Result<void> service() override;
    /// Sent to the chip (U_SetAddress) from service() while no frame is
    /// being fed to it; the chip acknowledges this address by itself.
    util::Result<void> setOwnAddress(uint16_t addressRaw) override;

    /// Reads the list where it is kept, from the RX interrupt, under the
    /// owner's generation: a frame that arrives while the list changes is not
    /// acknowledged (the sender repeats it). Always true.
    bool setAckGroupAddressView(const util::SharedGroupAddressView& view) override;
    /// The copy path is not supported: this backend keeps no list of its own.
    /// Clears the view, so only the broadcast is acknowledged until a view is set.
    void setAckGroupAddresses(std::span<const uint16_t> addresses) override;
    void setAckIndividualAddresses(std::span<const uint16_t> addresses) override;
    void setLocalBusy(bool busy) override { _localBusy.store(busy ? 1u : 0u, std::memory_order_relaxed); }
#if KNX_FEATURE_COUPLER
    void setGroupAckFilter(const GroupAckFilter* filter) override
    {
        _groupAckFilter.store(filter, std::memory_order_release);
    }
#endif

    /**
     * @brief FlashEraseGuard: put the chip into busy mode before an erase
     *
     * Declines (returns false) while a frame of ours is with the chip, a
     * frame is being received or a U_State.req waits for its answer, unless
     * mustProceed: then it first finishes feeding our frame (at most
     * FORCED_ERASE_FEED_MS). Sends U_ActivateBusyMode and waits until it has
     * left the UART. From then on received octets are discarded until the
     * host link is silent after endFlashErase(). Not initialised: true, and
     * nothing to do.
     */
    bool beginFlashErase(bool mustProceed) override;
    /// Lets the receive path find the next frame start; service() then sends
    /// U_ResetBusyMode. sendFrame() returns Busy until it has.
    void endFlashErase() override;

    TpUartStatistics statistics() const;

private:
    enum class RxPhase : uint8_t { Idle, InFrame };
    /// Receive path state around a flash erase. Lost: octets are dropped (the
    /// interrupt may run late, with octets missing). Armed: the erase is over;
    /// the first octet after more than FRAME_GAP_US of silence starts afresh.
    enum class RxSync : uint8_t { InSync, Lost, Armed };
    enum class TxPhase : uint8_t { None, Feeding, AwaitConfirm };

    struct RxSlot {
        uint8_t length{0};
        std::array<uint8_t, MAX_FRAME> data{};
    };

    /// A counter only the interrupt writes and anything may read.
    class IsrCounter {
    public:
        [[gnu::always_inline]] void increment()
        {
            _value.store(_value.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        }
        uint32_t read() const { return _value.load(std::memory_order_relaxed); }
        void clear() { _value.store(0u, std::memory_order_relaxed); }

    private:
        std::atomic<uint32_t> _value{0};
    };

    static void rxTrampoline(void* context, uint8_t octet, uint8_t flags, uint32_t timestampUs);
    void onRxOctet(uint8_t octet, uint8_t flags, uint32_t timestampUs);
    void onIdleOctet(uint8_t octet, uint8_t flags);
    void decideAck(uint16_t destination, bool groupAddressed);
    void finishFrame();
    /// stable is cleared when the owner changed the list while it was read.
    bool groupTableContains(uint16_t destination, bool& stable) const;
    bool individualTableContains(uint16_t destination) const;
    static size_t nextSlot(size_t index) { return (index + 1u) % RX_SLOT_COUNT; }

    util::Result<void> resetChip();
    util::Result<void> configureChip();
    util::Result<void> writeRequest(std::span<const uint8_t> octets);
    void resetRxState();
    void drainRx();
    void handleIndications();
    void pumpTx();
    void setTxPhase(TxPhase phase);
    void handleTxTimeout();
    void pollState();
    void updateLinkState();
    void leaveBusyMode();
    /// True while a received frame waits for its acknowledge decision: the
    /// U_AckInformation must reach the chip within 1.7 ms of the address-type
    /// octet (manual §3.2.3.1.8), so nothing may queue ahead of it.
    bool holdForAckDecision();
    void emit(const Tp1RxEvent& event);
    void emitTxOutcome(Tp1RxEventType type, Tp1AckClass ackClass);
    size_t txQueued() const;

    knx_uart_hal_t _hal;
    platform::TimingPlatform& _timing;
    TpUartConfig _config;

    Tp1EventCallback _eventCallback{};
    void* _eventContext{nullptr};
    bool _initialized{false};
    bool _error{false};
    bool _busMonitor{false};
    bool _addressPending{false};
    size_t _txCapacity{0};

    // --- Transmit (main context) ---------------------------------------------
    TxPhase _txPhase{TxPhase::None};
    std::array<uint8_t, MAX_FRAME> _txFrame{};
    size_t _txLength{0};
    size_t _txPosition{0};
    uint32_t _txStartMs{0};

    // --- Health (main context) -----------------------------------------------
    LinkState _linkState{LinkState::Unknown};
    uint32_t _linkTransitions{0};
    uint32_t _lastStatePollMs{0};
    bool _statePollOutstanding{false};
    uint8_t _missedStatePollsInRow{0};
    uint8_t _confirmsSeen{0};
    uint8_t _resetsSeen{0};
    uint8_t _stateIndsSeen{0};
    bool _expectingReset{false};
    uint32_t _confirmsPositive{0};
    uint32_t _confirmsNegative{0};
    uint32_t _confirmTimeouts{0};
    uint32_t _unexpectedConfirms{0};
    uint32_t _chipResets{0};
    uint32_t _stateIndications{0};
    uint32_t _missedStatePolls{0};
    uint8_t _lastStateBits{0};

    // --- Flash erase (main context) ------------------------------------------
    bool _busyModeActive{false};   ///< U_ActivateBusyMode sent, U_ResetBusyMode not yet
    uint32_t _resyncDiscardsSeen{0};
    uint32_t _resyncQuietSinceMs{0};
    uint32_t _erasesGuarded{0};
    uint32_t _erasesDeclined{0};

    // --- Acknowledge decision hold (main context) -----------------------------
    uint8_t _ackHoldFrameSeen{0};
    uint32_t _ackHoldSinceMs{0};

    // --- Receive (interrupt context only, except the ring indices) ----------
    std::array<RxSlot, RX_SLOT_COUNT> _rxSlots{};
    std::atomic<uint8_t> _rxHead{0};   ///< next slot service() reads; main writes
    std::atomic<uint8_t> _rxTail{0};   ///< slot being assembled; the interrupt writes
    std::atomic<RxPhase> _rxPhase{RxPhase::Idle};
    uint16_t _rxCount{0};
    uint16_t _rxExpected{0};
    uint8_t _rxXor{0};
    bool _rxBad{false};
    bool _rxEcho{false};  ///< the frame being assembled is our own, echoed by the chip
    uint8_t _rxFrameStarts{0};  ///< frame starts seen, wrapping, never 0
    uint32_t _rxLastUs{0};
    std::atomic<RxSync> _rxSync{RxSync::InSync};

    // --- Interrupt to main ----------------------------------------------------
    std::atomic<uint8_t> _confirmCount{0};
    std::atomic<uint8_t> _confirmPositive{0}; // uint8_t, not bool: its load inlines at -Os (no call out of RAM)
    std::atomic<uint8_t> _resetIndCount{0};
    /// _rxFrameStarts of the frame that waits for its acknowledge decision; 0: none.
    std::atomic<uint8_t> _rxAwaitingDecision{0};
    std::atomic<uint8_t> _stateIndCount{0};
    std::atomic<uint8_t> _stateIndValue{0};
    IsrCounter _framesReceived;
    IsrCounter _checksumErrors;
    IsrCounter _octetErrorFrames;
    IsrCounter _oversizedFrames;
    IsrCounter _partialFramesDiscarded;
    IsrCounter _rxOverflowDrops;
    IsrCounter _unexpectedOctets;
    IsrCounter _acksSent;
    IsrCounter _busySent;
    IsrCounter _ackDecisionsDeferred;
    IsrCounter _echoesDropped;
    IsrCounter _ackWriteFailures;
    IsrCounter _resyncDiscards;

    // --- Main to interrupt: the acknowledge inputs ---------------------------
    std::atomic<uint32_t> _ackTableSequence{0}; ///< odd while the inputs below are being replaced
    /// The device's group address list where its owner keeps it (no copy);
    /// read under the owner's generation as well as _ackTableSequence.
    util::SharedGroupAddressView _ackGroups{};
    std::array<std::atomic<uint16_t>, config::ADDITIONAL_INDIVIDUAL_ADDRESSES> _ackIndividuals{};
    std::atomic<uint8_t> _ackIndividualCount{0};
    std::atomic<uint16_t> _ownAddress{0};
    std::atomic<uint8_t> _localBusy{0};
    std::atomic<uint8_t> _busMonitorIsr{0};
    /// Non-zero while a frame of ours is with the chip (fed, not yet confirmed).
    std::atomic<uint8_t> _txOutstanding{0};
#if KNX_FEATURE_COUPLER
    std::atomic<const GroupAckFilter*> _groupAckFilter{nullptr};
#endif
};

} // namespace physical
} // namespace knx
