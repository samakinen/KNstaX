// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tpuart_medium_backend.cpp
 * @brief TP1 medium backend for a transceiver with a host UART (Siemens TP-UART2)
 */

#include "knx/physical/tpuart_medium_backend.hpp"

#include "knx/platform/isr_ram.hpp"
#include "knx/util/timing_utils.hpp"

namespace knx {
namespace physical {

namespace {

// TP1 frame layout (03_02_02 §2.2):
//   standard: CTRL, source (2), destination (2), AT/hop/length, TPDU (length + 1), FCS
//   extended: CTRL, CTRLE, source (2), destination (2), length, TPDU (length + 1), FCS
// The FCS makes the XOR of all octets FFh.
constexpr uint8_t CTRL_STANDARD = 0x80u;
constexpr uint8_t ADDRESS_TYPE_GROUP = 0x80u;
constexpr uint8_t STANDARD_LENGTH_MASK = 0x0Fu;
constexpr uint16_t STANDARD_OVERHEAD = 8u;
constexpr uint16_t EXTENDED_OVERHEAD = 9u;
constexpr uint16_t STANDARD_LENGTH_COUNT = 6u;  // octets in once the length octet (index 5) is
constexpr uint16_t EXTENDED_LENGTH_COUNT = 7u;  // octets in once the length octet (index 6) is
constexpr uint8_t FCS_RESULT = 0xFFu;
constexpr size_t MIN_FRAME = 8u;
constexpr uint16_t INDIVIDUAL_BROADCAST = 0xFFFFu;
constexpr uint16_t GROUP_BROADCAST = 0x0000u;

// Indication control octets (TP-UART2 manual): (b & D3h) == 90h is a standard
// frame, == 10h an extended one.
constexpr uint8_t FRAME_START_MASK = 0xD3u;
constexpr uint8_t FRAME_START_STANDARD = 0x90u;
constexpr uint8_t FRAME_START_EXTENDED = 0x10u;

constexpr uint8_t RX_OCTET_ERRORS =
    KNX_UART_HAL_RX_PARITY_ERROR | KNX_UART_HAL_RX_FRAMING_ERROR | KNX_UART_HAL_RX_NOISE;

constexpr uint8_t REPETITION_MASK = 0x07u;
constexpr uint8_t BUSY_REPETITION_SHIFT = 5u;

} // namespace

TpUartMediumBackend::TpUartMediumBackend(const knx_uart_hal_t& hal,
                                         platform::TimingPlatform& timing,
                                         const TpUartConfig& config)
    : _hal(hal)
    , _timing(timing)
    , _config(config)
{
    if (_config.txQueueLimitOctets < 2u) {
        _config.txQueueLimitOctets = 2u;  // one U_L_Data pair must always fit
    }
}

TpUartMediumBackend::~TpUartMediumBackend()
{
    close();
}

// ============================================================================
// Interrupt context
// ============================================================================

// Everything below rxTrampoline() is inlined into it, so the whole receive
// path lands in the one function KNX_ISR_RAM places; the definitions come
// first because a body must be visible to be inlined.

[[gnu::always_inline]] inline bool TpUartMediumBackend::groupTableContains(uint16_t destination, bool& stable) const
{
    // The owner's storage, read between two loads of its generation: what was
    // read counts only if the list did not change meanwhile.
    const util::SharedGroupAddressView& view = _ackGroups;
    if (!view.isValid()) {
        return false;
    }
    const uint32_t generation = view.generation->load(std::memory_order_acquire);
    const uint16_t count = view.count->load(std::memory_order_relaxed);
    if ((generation & 1u) != 0u || count > view.capacity) {
        stable = false;
        return false;
    }

    // A linear scan, whatever the order: 128 entries take about 10 us at
    // 48 MHz against a deadline of milliseconds, and one loop is less code in
    // RAM than a binary search plus the scan an unsorted list still needs.
    bool found = false;
    for (uint16_t i = 0; i < count; ++i) {
        if (view.entries[i].raw == destination) {
            found = true;
            break;
        }
    }

    std::atomic_thread_fence(std::memory_order_acquire);
    if (view.generation->load(std::memory_order_relaxed) != generation) {
        stable = false;
        return false;
    }
    return found;
}

[[gnu::always_inline]] inline bool TpUartMediumBackend::individualTableContains(uint16_t destination) const
{
    const uint8_t count = _ackIndividualCount.load(std::memory_order_relaxed);
    for (size_t i = 0; i < count && i < _ackIndividuals.size(); ++i) {
        if (_ackIndividuals[i].load(std::memory_order_relaxed) == destination) {
            return true;
        }
    }
    return false;
}

// The acknowledge decision (03_02_02 §2.4.2), as the bit-bang ISR makes it:
// group frames to an address in the table or to the broadcast 0/0/0, and
// individual frames to the own or an assumed address (never FFFFh). The chip
// acknowledges the own address by itself (U_SetAddress), so it is answered
// here only to refuse it with BUSY.
[[gnu::always_inline]] inline void TpUartMediumBackend::decideAck(uint16_t destination, bool groupAddressed)
{
    if (_busMonitorIsr.load(std::memory_order_relaxed) != 0u) {
        return;
    }
    if (_rxExpected > MAX_FRAME) {
        return;  // a frame this backend cannot take is not acknowledged
    }

    const uint32_t sequenceBefore = _ackTableSequence.load(std::memory_order_acquire);
    bool addressed = false;
    bool own = false;
    bool stable = true;
    if (groupAddressed) {
        addressed = destination == GROUP_BROADCAST || groupTableContains(destination, stable);
#if KNX_FEATURE_COUPLER
        if (!addressed) {
            const GroupAckFilter* coupler = _groupAckFilter.load(std::memory_order_acquire);
            addressed = coupler != nullptr && coupler->acknowledges(destination);
        }
#endif
    } else if (destination != INDIVIDUAL_BROADCAST) {
        const uint16_t ownAddress = _ownAddress.load(std::memory_order_relaxed);
        own = ownAddress != 0u && destination == ownAddress;
        addressed = own || individualTableContains(destination);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (!stable || (sequenceBefore & 1u) != 0u ||
        _ackTableSequence.load(std::memory_order_relaxed) != sequenceBefore) {
        _ackDecisionsDeferred.increment();
        return;
    }
    if (!addressed) {
        return;
    }

    // Busy when the stack asks for it, or when no slot is free for this frame
    // after the one being assembled: the sender then repeats it.
    const bool ringFull = nextSlot(_rxTail.load(std::memory_order_relaxed)) == _rxHead.load(std::memory_order_acquire);
    const bool busy = _localBusy.load(std::memory_order_relaxed) != 0u || ringFull;
    if (own && !busy) {
        return;
    }

    const uint8_t ack = static_cast<uint8_t>(U_ACK_INFORMATION | U_ACK_ADDRESSED | (busy ? U_ACK_BUSY : 0u));
    if (_hal.ops.write(_hal.context, &ack, 1u)) {
        (busy ? _busySent : _acksSent).increment();
    } else {
        _ackWriteFailures.increment();
    }
}

[[gnu::always_inline]] inline void TpUartMediumBackend::finishFrame()
{
    _rxPhase.store(RxPhase::Idle, std::memory_order_relaxed);
    if (_rxEcho) {
        _echoesDropped.increment();
        return;
    }
    if (_rxBad) {
        _octetErrorFrames.increment();
        return;
    }
    if (_rxExpected > MAX_FRAME) {
        _oversizedFrames.increment();
        return;
    }
    if (_rxXor != FCS_RESULT) {
        _checksumErrors.increment();
        return;
    }

    const uint8_t tail = _rxTail.load(std::memory_order_relaxed);
    const uint8_t next = static_cast<uint8_t>(nextSlot(tail));
    if (next == _rxHead.load(std::memory_order_acquire)) {
        _rxOverflowDrops.increment();
        return;
    }
    _rxSlots[tail].length = static_cast<uint8_t>(_rxExpected);
    _rxTail.store(next, std::memory_order_release);
    _framesReceived.increment();
}

[[gnu::always_inline]] inline void TpUartMediumBackend::onIdleOctet(uint8_t octet, uint8_t flags)
{
    if ((flags & RX_OCTET_ERRORS) != 0u) {
        _unexpectedOctets.increment();  // a damaged octet outside a frame means nothing
        return;
    }

    const uint8_t startPattern = static_cast<uint8_t>(octet & FRAME_START_MASK);
    if (startPattern == FRAME_START_STANDARD || startPattern == FRAME_START_EXTENDED) {
        _rxCount = 1u;
        _rxExpected = 0u;
        _rxXor = octet;
        _rxBad = false;
        _rxEcho = false;
        _rxSlots[_rxTail.load(std::memory_order_relaxed)].data[0] = octet;
        _rxPhase.store(RxPhase::InFrame, std::memory_order_relaxed);
        // The main context writes nothing from now until the decision.
        _rxFrameStarts = static_cast<uint8_t>(_rxFrameStarts + 1u);
        _rxFrameStarts = _rxFrameStarts == 0u ? 1u : _rxFrameStarts;
        _rxAwaitingDecision.store(_rxFrameStarts, std::memory_order_release);
        return;
    }
    if ((octet & L_DATA_CON_MASK) == L_DATA_CON) {
        _confirmPositive.store((octet & L_DATA_CON_POSITIVE) != 0u ? 1u : 0u, std::memory_order_relaxed);
        _confirmCount.store(static_cast<uint8_t>(_confirmCount.load(std::memory_order_relaxed) + 1u),
                            std::memory_order_release);
        return;
    }
    if (octet == U_RESET_IND) {
        _resetIndCount.store(static_cast<uint8_t>(_resetIndCount.load(std::memory_order_relaxed) + 1u),
                             std::memory_order_release);
        return;
    }
    if ((octet & U_STATE_IND_MASK) == U_STATE_IND) {
        _stateIndValue.store(octet, std::memory_order_relaxed);
        _stateIndCount.store(static_cast<uint8_t>(_stateIndCount.load(std::memory_order_relaxed) + 1u),
                             std::memory_order_release);
        return;
    }
    _unexpectedOctets.increment();
}

[[gnu::always_inline]] inline void TpUartMediumBackend::onRxOctet(uint8_t octet, uint8_t flags, uint32_t timestampUs)
{
    const uint32_t lastUs = _rxLastUs;
    _rxLastUs = timestampUs;

    // Around a flash erase the interrupt may have run late, with octets lost
    // and timestamps that no longer show where frames end: drop everything
    // until, after the erase, an octet follows real silence. That octet is a
    // frame start or an indication (frames to us were answered BUSY meanwhile).
    const RxSync sync = _rxSync.load(std::memory_order_acquire);
    if (sync != RxSync::InSync) {
        if (sync == RxSync::Lost || static_cast<uint32_t>(timestampUs - lastUs) <= FRAME_GAP_US) {
            _resyncDiscards.increment();
            return;
        }
        // A plain store, not a compare-exchange (no library call on cores
        // without LDREX): only Armed -> InSync is written here, and service()
        // writes the same transition.
        _rxSync.store(RxSync::InSync, std::memory_order_release);
        _rxPhase.store(RxPhase::Idle, std::memory_order_relaxed);
        _rxAwaitingDecision.store(0u, std::memory_order_release);
    }

    // More than 2 ms of silence ends whatever was being assembled.
    if (_rxPhase.load(std::memory_order_relaxed) == RxPhase::InFrame &&
        static_cast<uint32_t>(timestampUs - lastUs) > FRAME_GAP_US) {
        _partialFramesDiscarded.increment();
        _rxPhase.store(RxPhase::Idle, std::memory_order_relaxed);
        _rxAwaitingDecision.store(0u, std::memory_order_release);
    }

    if (_rxPhase.load(std::memory_order_relaxed) == RxPhase::Idle) {
        onIdleOctet(octet, flags);
        return;
    }

    if ((flags & (RX_OCTET_ERRORS | KNX_UART_HAL_RX_OVERRUN)) != 0u) {
        _rxBad = true;
    }
    RxSlot& slot = _rxSlots[_rxTail.load(std::memory_order_relaxed)];
    if (_rxCount < MAX_FRAME) {
        slot.data[_rxCount] = octet;
    }
    ++_rxCount;
    _rxXor = static_cast<uint8_t>(_rxXor ^ octet);

    // The length is known, and with it the destination and its type: decide
    // the acknowledge now, while the rest of the frame is still on the bus.
    // One call site, so the decision is inlined once into the RAM function.
    const bool standard = (slot.data[0] & CTRL_STANDARD) != 0u;
    if (_rxCount == (standard ? STANDARD_LENGTH_COUNT : EXTENDED_LENGTH_COUNT)) {
        _rxExpected = standard ? static_cast<uint16_t>((octet & STANDARD_LENGTH_MASK) + STANDARD_OVERHEAD)
                               : static_cast<uint16_t>(octet + EXTENDED_OVERHEAD);
        // The TP-UART2 passes our own frame back while it is on the bus
        // (seen on the H8i8o, 15 November 2026). A frame from our own address
        // while one of ours is with the chip is that echo: it is neither
        // acknowledged (the chip would acknowledge its own frame and confirm
        // positively whoever listens) nor delivered.
        const size_t source = standard ? 1u : 2u;
        _rxEcho = _txOutstanding.load(std::memory_order_acquire) != 0u &&
                  static_cast<uint16_t>((slot.data[source] << 8) | slot.data[source + 1u]) ==
                      _ownAddress.load(std::memory_order_relaxed);
        if (!_rxEcho) {
            const size_t destination = source + 2u;
            const uint8_t addressType = standard ? octet : slot.data[1];
            decideAck(static_cast<uint16_t>((slot.data[destination] << 8) | slot.data[destination + 1u]),
                      (addressType & ADDRESS_TYPE_GROUP) != 0u);
        }
        _rxAwaitingDecision.store(0u, std::memory_order_release);
    }

    if (_rxExpected != 0u && _rxCount >= _rxExpected) {
        finishFrame();
    }
}

KNX_ISR_RAM void TpUartMediumBackend::rxTrampoline(void* context, uint8_t octet, uint8_t flags, uint32_t timestampUs)
{
    static_cast<TpUartMediumBackend*>(context)->onRxOctet(octet, flags, timestampUs);
}

// ============================================================================
// Main context
// ============================================================================

util::Result<void> TpUartMediumBackend::init(const Tp1MediumConfig& config)
{
    if (!knx_uart_hal_is_valid(&_hal)) {
        return util::ErrorCode::InvalidParameter;
    }
    if (_initialized) {
        close();
    }

    _ownAddress.store(config.ownIndividualAddressRaw, std::memory_order_relaxed);
    _busMonitor = config.busMonitorMode;
    _busMonitorIsr.store(_busMonitor ? 1u : 0u, std::memory_order_relaxed);
    resetRxState();
    setTxPhase(TxPhase::None);
    _error = false;
    _addressPending = false;
    _configurePending = false;

    const knx_uart_hal_config_t uartConfig{_config.baud, _config.parity};
    if (!knx_uart_hal_open(&_hal, &uartConfig, &rxTrampoline, this)) {
        _error = true;
        return util::ErrorCode::ResourceUnavailable;
    }
    // Nothing is queued yet, so what is free now is the whole TX buffer.
    _txCapacity = knx_uart_hal_tx_free(&_hal);

    // The address the owner passes here is usually its default: the real one
    // comes from persistence after init() (setOwnAddress). U_SetAddress waits
    // for the first service(), so the chip is given one address, not two.
    auto reset = resetChip(true);
    if (reset.isError()) {
        knx_uart_hal_close(&_hal);
        _error = true;
        _linkState = LinkState::Down;
        return reset;
    }

    _initialized = true;
    _lastStatePollMs = util::nowMs(&_timing);
    return util::Result<void>::ok();
}

void TpUartMediumBackend::close()
{
    if (!_initialized) {
        return;
    }
    knx_uart_hal_close(&_hal);
    _initialized = false;
    setTxPhase(TxPhase::None);
    resetRxState();
}

void TpUartMediumBackend::resetRxState()
{
    // Only called with the UART closed or before it is opened: the interrupt
    // does not run concurrently.
    _rxPhase.store(RxPhase::Idle, std::memory_order_relaxed);
    _rxHead.store(0u, std::memory_order_relaxed);
    _rxTail.store(0u, std::memory_order_relaxed);
    _rxCount = 0u;
    _rxExpected = 0u;
    _rxSync.store(RxSync::InSync, std::memory_order_relaxed);
    _rxAwaitingDecision.store(0u, std::memory_order_relaxed);
    _busyModeActive = false;  // a reset ends busy mode in the chip too
    _confirmsSeen = _confirmCount.load(std::memory_order_relaxed);
    _resetsSeen = _resetIndCount.load(std::memory_order_relaxed);
    _stateIndsSeen = _stateIndCount.load(std::memory_order_relaxed);
}

util::Result<void> TpUartMediumBackend::writeRequest(std::span<const uint8_t> octets)
{
    return knx_uart_hal_write(&_hal, octets.data(), octets.size()) ? util::Result<void>::ok()
                                                                   : util::Result<void>(util::ErrorCode::ResourceUnavailable);
}

util::Result<void> TpUartMediumBackend::resetChip(bool deferAddress)
{
    if (knx_uart_hal_has_reset(&_hal)) {
        // The reset input, where wired, also clears a chip that no longer
        // listens to its UART.
        (void)knx_uart_hal_set_reset(&_hal, true);
        util::delayMs(&_timing, 1u);
        (void)knx_uart_hal_set_reset(&_hal, false);
    }

    _expectingReset = true;
    bool answered = false;
    for (uint8_t attempt = 0; attempt < _config.resetAttempts && !answered; ++attempt) {
        const uint8_t seen = _resetIndCount.load(std::memory_order_acquire);
        const uint8_t request = U_RESET_REQ;
        auto written = writeRequest(std::span<const uint8_t>(&request, 1u));
        if (written.isError()) {
            _expectingReset = false;
            return written;
        }
        const uint32_t start = util::nowMs(&_timing);
        while (static_cast<uint32_t>(util::nowMs(&_timing) - start) < _config.resetTimeoutMs) {
            if (_resetIndCount.load(std::memory_order_acquire) != seen) {
                answered = true;
                break;
            }
            util::delayMs(&_timing, 1u);
        }
    }
    _resetsSeen = _resetIndCount.load(std::memory_order_acquire);
    _expectingReset = false;
    if (!answered) {
        return util::ErrorCode::Timeout;
    }
    return configureChip(deferAddress);
}

util::Result<void> TpUartMediumBackend::sendAddress()
{
    const uint16_t address = _ownAddress.load(std::memory_order_relaxed);
    const std::array<uint8_t, 3> setAddress{U_SET_ADDRESS_REQ, static_cast<uint8_t>(address >> 8),
                                            static_cast<uint8_t>(address & 0xFFu)};
    auto result = writeRequest(setAddress);
    if (result.isOk()) {
        _addressPending = false;
    }
    return result;
}

util::Result<void> TpUartMediumBackend::configureChip(bool deferAddress)
{
    util::Result<void> result = util::Result<void>::ok();
    if (deferAddress) {
        _addressPending = true;
    } else {
        result = sendAddress();
        if (result.isError()) {
            return result;
        }
    }

    const std::array<uint8_t, 2> repetitions{
        U_MXRSTCNT_REQ,
        static_cast<uint8_t>(((_config.busyRepetitions & REPETITION_MASK) << BUSY_REPETITION_SHIFT) |
                             (_config.nackRepetitions & REPETITION_MASK))};
    result = writeRequest(repetitions);
    if (result.isError()) {
        return result;
    }

    if (_busMonitor) {
        const uint8_t busmon = U_BUSMON_REQ;
        result = writeRequest(std::span<const uint8_t>(&busmon, 1u));
        if (result.isError()) {
            return result;
        }
    }

    const uint8_t state = U_STATE_REQ;
    result = writeRequest(std::span<const uint8_t>(&state, 1u));
    if (result.isError()) {
        return result;
    }
    _statePollOutstanding = true;
    _lastStatePollMs = util::nowMs(&_timing);
    _configurePending = false;
    return util::Result<void>::ok();
}

util::Result<size_t> TpUartMediumBackend::sendFrame(std::span<const uint8_t> frame)
{
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (_busMonitor) {
        return util::ErrorCode::OperationNotSupported;
    }
    if (_txPhase != TxPhase::None || _error || _busyModeActive) {
        return util::ErrorCode::Busy;  // busy mode: until U_ResetBusyMode is out
    }
    if (frame.size() < MIN_FRAME || frame.size() > MAX_FRAME) {
        return util::ErrorCode::InvalidFrameSize;
    }

    std::copy(frame.begin(), frame.end(), _txFrame.begin());
    _txLength = frame.size();
    _txPosition = 0u;
    _txStartMs = util::nowMs(&_timing);
    setTxPhase(TxPhase::Feeding);
    // A confirmation the chip sent before this frame is not its outcome.
    _confirmsSeen = _confirmCount.load(std::memory_order_acquire);
    pumpTx();
    return frame.size();
}

size_t TpUartMediumBackend::txQueued() const
{
    const size_t free = knx_uart_hal_tx_free(&_hal);
    return free >= _txCapacity ? 0u : _txCapacity - free;
}

void TpUartMediumBackend::setTxPhase(TxPhase phase)
{
    // Set before the first octet goes to the chip, cleared only once the
    // outcome is in: every echo (repetitions included) arrives in between.
    _txPhase = phase;
    _txOutstanding.store(phase != TxPhase::None ? 1u : 0u, std::memory_order_release);
}

void TpUartMediumBackend::pumpTx()
{
    // One U_L_Data request pair per octet: Start/Continue carries the octet's
    // index, End (on the FCS) the index of the last octet. A few octets at a
    // time, and none while a received frame waits for its acknowledge
    // decision, so the acknowledge from the RX interrupt finds the UART empty
    // (design note §3.9).
    if (holdForAckDecision()) {
        return;
    }
    while (_txPhase == TxPhase::Feeding && _txPosition < _txLength) {
        if (txQueued() + 2u > _config.txQueueLimitOctets) {
            return;
        }
        const bool last = _txPosition + 1u == _txLength;
        const uint8_t index = static_cast<uint8_t>(_txPosition);
        const std::array<uint8_t, 2> pair{
            static_cast<uint8_t>((last ? U_L_DATA_END : U_L_DATA_START_CONTINUE) | index), _txFrame[_txPosition]};
        if (!knx_uart_hal_write(&_hal, pair.data(), pair.size())) {
            return;
        }
        ++_txPosition;
    }
    if (_txPhase == TxPhase::Feeding && _txPosition == _txLength) {
        setTxPhase(TxPhase::AwaitConfirm);
    }
}

void TpUartMediumBackend::emit(const Tp1RxEvent& event)
{
    if (_eventCallback) {
        _eventCallback(event, _eventContext);
    }
}

void TpUartMediumBackend::emitTxOutcome(Tp1RxEventType type, Tp1AckClass ackClass)
{
    Tp1RxEvent event;
    event.type = type;
    event.ackClass = ackClass;
    emit(event);
}

void TpUartMediumBackend::drainRx()
{
    uint8_t head = _rxHead.load(std::memory_order_relaxed);
    while (head != _rxTail.load(std::memory_order_acquire)) {
        const RxSlot& slot = _rxSlots[head];
        Tp1RxEvent event;
        event.type = Tp1RxEventType::TelegramEnd;
        event.frame = std::span<const uint8_t>(slot.data.data(), slot.length);
        emit(event);
        head = static_cast<uint8_t>(nextSlot(head));
        _rxHead.store(head, std::memory_order_release);
    }
}

void TpUartMediumBackend::handleIndications()
{
    const uint8_t confirms = _confirmCount.load(std::memory_order_acquire);
    if (confirms != _confirmsSeen) {
        _confirmsSeen = confirms;
        const bool positive = _confirmPositive.load(std::memory_order_relaxed) != 0u;
        if (_txPhase == TxPhase::AwaitConfirm) {
            setTxPhase(TxPhase::None);
            // The chip has made its repetitions; a negative confirmation does
            // not say whether the peer answered NACK, BUSY or nothing.
            if (positive) {
                ++_confirmsPositive;
                emitTxOutcome(Tp1RxEventType::TxAckResponse, Tp1AckClass::Ack);
            } else {
                ++_confirmsNegative;
                emitTxOutcome(Tp1RxEventType::TxAckResponse, Tp1AckClass::None);
            }
        } else {
            ++_unexpectedConfirms;
        }
    }

    const uint8_t resets = _resetIndCount.load(std::memory_order_acquire);
    if (resets != _resetsSeen && !_expectingReset) {
        // The chip reset on its own (a supply dip, a watchdog): it has lost its
        // address and settings, and any frame it was given.
        _resetsSeen = resets;
        ++_chipResets;
        if (_txPhase != TxPhase::None) {
            setTxPhase(TxPhase::None);
            ++_confirmTimeouts;
            emitTxOutcome(Tp1RxEventType::TxAckDeadlineMiss, Tp1AckClass::None);
        }
        if (configureChip().isError()) {
            _configurePending = true;  // retried from service()
        }
    }

    const uint8_t states = _stateIndCount.load(std::memory_order_acquire);
    if (states != _stateIndsSeen) {
        _stateIndsSeen = states;
        ++_stateIndications;
        _lastStateBits = _stateIndValue.load(std::memory_order_relaxed);
        _statePollOutstanding = false;
        _missedStatePollsInRow = 0u;
    }
}

void TpUartMediumBackend::handleTxTimeout()
{
    if (_txPhase == TxPhase::None) {
        return;
    }
    if (static_cast<uint32_t>(util::nowMs(&_timing) - _txStartMs) < _config.confirmTimeoutMs) {
        return;
    }
    setTxPhase(TxPhase::None);
    ++_confirmTimeouts;
    emitTxOutcome(Tp1RxEventType::TxAckDeadlineMiss, Tp1AckClass::None);

    // The chip is assumed lost.
    auto reset = resetChip();
    _error = reset.isError();
}

bool TpUartMediumBackend::holdForAckDecision()
{
    const uint8_t frame = _rxAwaitingDecision.load(std::memory_order_acquire);
    if (frame == 0u) {
        return false;
    }
    const uint32_t now = util::nowMs(&_timing);
    if (frame != _ackHoldFrameSeen) {
        _ackHoldFrameSeen = frame;
        _ackHoldSinceMs = now;
    }
    // A frame cut off before its destination leaves the mark behind until the
    // next octet; do not wait for it longer than its octets could take.
    return static_cast<uint32_t>(now - _ackHoldSinceMs) < ACK_DECISION_HOLD_MS;
}

void TpUartMediumBackend::pollState()
{
    if (_txPhase != TxPhase::None) {
        return;  // no requests between the U_L_Data pairs of a frame
    }
    if (holdForAckDecision()) {
        return;  // the address, the state request: after the acknowledge
    }
    if (_error) {
        // The chip did not come back after a reset: keep trying, once per interval.
        const uint32_t now = util::nowMs(&_timing);
        if (static_cast<uint32_t>(now - _lastStatePollMs) >= _config.statePollIntervalMs) {
            _lastStatePollMs = now;
            _error = resetChip().isError();
        }
        return;
    }
    if (_configurePending) {
        if (configureChip().isError()) {
            return;
        }
    } else if (_addressPending) {
        if (sendAddress().isError()) {
            return;
        }
    }

    const uint32_t now = util::nowMs(&_timing);
    if (static_cast<uint32_t>(now - _lastStatePollMs) < _config.statePollIntervalMs) {
        return;
    }
    if (_statePollOutstanding) {
        ++_missedStatePolls;
        if (_missedStatePollsInRow < UINT8_MAX) {
            ++_missedStatePollsInRow;
        }
    }
    const uint8_t request = U_STATE_REQ;
    if (writeRequest(std::span<const uint8_t>(&request, 1u)).isOk()) {
        _statePollOutstanding = true;
    }
    _lastStatePollMs = now;
}

void TpUartMediumBackend::updateLinkState()
{
    LinkState state = LinkState::Up;
    if (_error || _missedStatePollsInRow >= 2u || knx_uart_hal_read_save(&_hal)) {
        state = LinkState::Down;
    } else if (_stateIndications == 0u) {
        state = _linkState;  // no answer yet: nothing new to report
    } else if ((_lastStateBits & U_STATE_TEMPERATURE_WARNING) != 0u) {
        state = LinkState::Degraded;
    }
    // The other U_State bits (slave collision, receive, transmit and protocol
    // errors) report single events; they are counted in lastStateBits, not
    // treated as a link fault.

    if (state == _linkState) {
        return;
    }
    _linkState = state;
    ++_linkTransitions;
    Tp1RxEvent event;
    event.type = Tp1RxEventType::LinkStateChanged;
    event.linkStatus.state = state;
    event.linkStatus.kind = LinkEventKind::StateChanged;
    event.linkStatus.transitionCount = _linkTransitions;
    event.linkStatus.timestampUs = _timing.micros();
    emit(event);
}

util::Result<void> TpUartMediumBackend::service()
{
    if (!_initialized) {
        return util::Result<void>::ok();
    }
    drainRx();
    handleIndications();
    leaveBusyMode();
    pumpTx();
    handleTxTimeout();
    pollState();
    updateLinkState();
    return util::Result<void>::ok();
}

bool TpUartMediumBackend::beginFlashErase(bool mustProceed)
{
    if (!_initialized) {
        return true;
    }
    const uint32_t nowMs = util::nowMs(&_timing);
    // U_State.ind comes within about 1.4 ms; the reset wait bounds it generously.
    const bool stateReplyDue = _statePollOutstanding &&
                               static_cast<uint32_t>(nowMs - _lastStatePollMs) < _config.resetTimeoutMs;
    if (!mustProceed && (_txPhase != TxPhase::None || stateReplyDue ||
                         _rxPhase.load(std::memory_order_relaxed) == RxPhase::InFrame)) {
        // Our frame's L_Data.con, a U_State.ind or the rest of a frame would
        // arrive during the erase and be lost.
        ++_erasesDeclined;
        return false;
    }
    // No request may go between the U_L_Data pairs of a frame: finish feeding it.
    const uint32_t feedStart = nowMs;
    while (_txPhase == TxPhase::Feeding &&
           static_cast<uint32_t>(util::nowMs(&_timing) - feedStart) < FORCED_ERASE_FEED_MS) {
        pumpTx();
        if (_txPhase == TxPhase::Feeding) {
            util::delayMs(&_timing, 1u);
        }
    }

    const uint8_t request = U_ACTIVATE_BUSY_MODE;
    if (writeRequest(std::span<const uint8_t>(&request, 1u)).isOk()) {
        _busyModeActive = true;
        // The chip must have it before the erase stops the interrupts that
        // feed the UART. The last octet may still be in the shift register;
        // the hardware sends it on its own.
        const uint32_t drainStart = util::nowMs(&_timing);
        while (txQueued() != 0u &&
               static_cast<uint32_t>(util::nowMs(&_timing) - drainStart) < BUSY_MODE_DRAIN_MS) {
            util::delayMs(&_timing, 1u);
        }
    }
    // Whatever arrives from now until the link is silent after the erase is dropped.
    _rxSync.store(RxSync::Lost, std::memory_order_release);
    ++_erasesGuarded;
    return true;
}

void TpUartMediumBackend::endFlashErase()
{
    if (!_initialized) {
        return;
    }
    _resyncDiscardsSeen = _resyncDiscards.read();
    _resyncQuietSinceMs = util::nowMs(&_timing);
    _rxSync.store(RxSync::Armed, std::memory_order_release);
}

void TpUartMediumBackend::leaveBusyMode()
{
    // The receive path is back in step once an octet has followed silence
    // (the interrupt then says so), or once the link has been silent for
    // RESYNC_SILENCE_MS. The RX timestamps may come from another clock than
    // the platform's, so silence is judged here from the discard count: it
    // does not move while nothing arrives.
    if (_rxSync.load(std::memory_order_acquire) == RxSync::Armed) {
        const uint32_t now = util::nowMs(&_timing);
        const uint32_t discards = _resyncDiscards.read();
        if (discards != _resyncDiscardsSeen) {
            _resyncDiscardsSeen = discards;
            _resyncQuietSinceMs = now;
        } else if (static_cast<uint32_t>(now - _resyncQuietSinceMs) >= RESYNC_SILENCE_MS) {
            _rxSync.store(RxSync::InSync, std::memory_order_release);
        }
    }
    if (!_busyModeActive || _rxSync.load(std::memory_order_acquire) != RxSync::InSync || holdForAckDecision()) {
        return;
    }
    // The manual asks for exactly this order: synchronise, then U_ResetBusyMode.
    const uint8_t request = U_RESET_BUSY_MODE;
    if (writeRequest(std::span<const uint8_t>(&request, 1u)).isOk()) {
        _busyModeActive = false;
    }
}

void TpUartMediumBackend::setEventCallback(Tp1EventCallback callback, void* context)
{
    _eventCallback = std::move(callback);
    _eventContext = context;
}

Tp1MediumState TpUartMediumBackend::getState() const
{
    if (!_initialized) {
        return Tp1MediumState::Uninitialized;
    }
    if (_error) {
        return Tp1MediumState::Error;
    }
    if (_txPhase != TxPhase::None) {
        return Tp1MediumState::Transmitting;
    }
    if (_rxPhase.load(std::memory_order_relaxed) == RxPhase::InFrame) {
        return Tp1MediumState::Receiving;
    }
    return Tp1MediumState::Idle;
}

Tp1CapabilityProfile TpUartMediumBackend::getCapabilities() const
{
    Tp1CapabilityProfile capabilities;
    capabilities.supportsHardwareAutoAck = true;
    capabilities.supportsDetailedTxConfirm = true;
    capabilities.supportsLinkStateIndication = true;
    capabilities.transceiverRepeats = true;
    return capabilities;
}

util::Result<void> TpUartMediumBackend::setBusMonitorMode(bool enabled)
{
    if (enabled == _busMonitor) {
        return util::Result<void>::ok();
    }
    _busMonitor = enabled;
    _busMonitorIsr.store(enabled ? 1u : 0u, std::memory_order_relaxed);
    if (!_initialized) {
        return util::Result<void>::ok();
    }
    if (enabled) {
        const uint8_t request = U_BUSMON_REQ;
        return writeRequest(std::span<const uint8_t>(&request, 1u));
    }
    // The TP-UART2 leaves bus monitor mode only through a reset.
    auto reset = resetChip();
    _error = reset.isError();
    return reset;
}

util::Result<void> TpUartMediumBackend::setOwnAddress(uint16_t addressRaw)
{
    _ownAddress.store(addressRaw, std::memory_order_relaxed);
    if (_initialized) {
        _addressPending = true;  // sent from service() between frames
    }
    return util::Result<void>::ok();
}

bool TpUartMediumBackend::setAckGroupAddressView(const util::SharedGroupAddressView& view)
{
    // Replace the view under an odd sequence; the interrupt does not
    // acknowledge from a view it saw being replaced.
    _ackTableSequence.store(_ackTableSequence.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    _ackGroups = view;
    _ackTableSequence.store(_ackTableSequence.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
    return true;
}

void TpUartMediumBackend::setAckGroupAddresses(std::span<const uint16_t> addresses)
{
    (void)addresses;
    (void)setAckGroupAddressView(util::SharedGroupAddressView{});
}

void TpUartMediumBackend::setAckIndividualAddresses(std::span<const uint16_t> addresses)
{
    _ackTableSequence.store(_ackTableSequence.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    uint8_t count = 0;
    for (const uint16_t address : addresses) {
        if (count >= _ackIndividuals.size()) {
            break;
        }
        _ackIndividuals[count].store(address, std::memory_order_relaxed);
        ++count;
    }
    _ackIndividualCount.store(count, std::memory_order_relaxed);

    _ackTableSequence.store(_ackTableSequence.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
}

TpUartStatistics TpUartMediumBackend::statistics() const
{
    TpUartStatistics stats;
    stats.framesReceived = _framesReceived.read();
    stats.checksumErrors = _checksumErrors.read();
    stats.octetErrorFrames = _octetErrorFrames.read();
    stats.oversizedFrames = _oversizedFrames.read();
    stats.partialFramesDiscarded = _partialFramesDiscarded.read();
    stats.rxOverflowDrops = _rxOverflowDrops.read();
    stats.unexpectedOctets = _unexpectedOctets.read();
    stats.acksSent = _acksSent.read();
    stats.busySent = _busySent.read();
    stats.ackDecisionsDeferred = _ackDecisionsDeferred.read();
    stats.echoesDropped = _echoesDropped.read();
    stats.ackWriteFailures = _ackWriteFailures.read();
    stats.erasesGuarded = _erasesGuarded;
    stats.erasesDeclined = _erasesDeclined;
    stats.resyncDiscards = _resyncDiscards.read();
    stats.confirmsPositive = _confirmsPositive;
    stats.confirmsNegative = _confirmsNegative;
    stats.confirmTimeouts = _confirmTimeouts;
    stats.unexpectedConfirms = _unexpectedConfirms;
    stats.chipResets = _chipResets;
    stats.stateIndications = _stateIndications;
    stats.missedStatePolls = _missedStatePolls;
    stats.lastStateBits = _lastStateBits;
    return stats;
}

} // namespace physical
} // namespace knx
