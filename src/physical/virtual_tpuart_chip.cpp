// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/virtual_tpuart_chip.hpp"

#include <algorithm>
#include <limits>

namespace knx {
namespace physical {

namespace {

// Host protocol (TP-UART2 technical manual §3.2.3).
constexpr uint8_t U_RESET_REQ = 0x01u;
constexpr uint8_t U_STATE_REQ = 0x02u;
constexpr uint8_t U_BUSMON_REQ = 0x05u;
constexpr uint8_t U_ACK_INFORMATION_MASK = 0xF8u;
constexpr uint8_t U_ACK_INFORMATION = 0x10u;
constexpr uint8_t U_ACK_ADDRESSED = 0x01u;
constexpr uint8_t U_ACK_BUSY = 0x02u;
constexpr uint8_t U_ACK_NACK = 0x04u;
constexpr uint8_t U_PRODUCT_ID_REQ = 0x20u;
constexpr uint8_t U_ACTIVATE_BUSY_MODE = 0x21u;
constexpr uint8_t U_RESET_BUSY_MODE = 0x22u;
constexpr uint8_t U_MXRSTCNT = 0x24u;
constexpr uint8_t U_SET_ADDRESS = 0x28u;
constexpr uint8_t U_L_DATA_MASK = 0xC0u;
constexpr uint8_t U_L_DATA_START_CONTINUE = 0x80u;
constexpr uint8_t U_L_DATA_END = 0x40u;
constexpr uint8_t U_L_DATA_INDEX_MASK = 0x3Fu;
constexpr uint8_t U_RESET_IND = 0x03u;
constexpr uint8_t U_STATE_IND = 0x07u;
constexpr uint8_t L_DATA_CON_POSITIVE = 0x8Bu;
constexpr uint8_t L_DATA_CON_NEGATIVE = 0x0Bu;
constexpr uint8_t REPETITION_MASK = 0x07u;
constexpr uint8_t BUSY_REPETITION_SHIFT = 5u;

// Frame layout (03_02_02 §2.2).
constexpr uint8_t CTRL_STANDARD = 0x80u;
constexpr uint8_t CTRL_NOT_REPEATED = 0x20u;
constexpr uint8_t ADDRESS_TYPE_GROUP = 0x80u;
constexpr uint8_t STANDARD_LENGTH_MASK = 0x0Fu;
constexpr size_t STANDARD_OVERHEAD = 8u;
constexpr size_t EXTENDED_OVERHEAD = 9u;
constexpr size_t ADDRESS_TYPE_INDEX = 5u;  // the octet after which the destination and its type are known
constexpr size_t MIN_FRAME = 8u;
constexpr uint8_t FCS_RESULT = 0xFFu;

constexpr uint8_t CORRUPTION_BIT = 0x01u;
// The reset glitch arrives 1.4 ms after the request was written on the H8i8o:
// the request's octet time, this, and the glitch's own octet time.
constexpr uint32_t RESET_GLITCH_DELAY_US = 250u;

void refreshChecksum(std::vector<uint8_t>& frame)
{
    frame.back() = tp1_frame::checksum(std::vector<uint8_t>(frame.begin(), frame.end() - 1));
}

bool lengthMatches(const std::vector<uint8_t>& frame)
{
    if (frame.size() < MIN_FRAME) {
        return false;
    }
    const size_t expected = tp1_frame::isStandard(frame) ? (frame[5] & STANDARD_LENGTH_MASK) + STANDARD_OVERHEAD
                                                         : frame[6] + EXTENDED_OVERHEAD;
    return expected == frame.size();
}

} // namespace

// ============================================================================
// Frame helpers
// ============================================================================

namespace tp1_frame {

uint8_t checksum(const std::vector<uint8_t>& withoutFcs)
{
    uint8_t x = 0u;
    for (const uint8_t octet : withoutFcs) {
        x = static_cast<uint8_t>(x ^ octet);
    }
    return static_cast<uint8_t>(x ^ FCS_RESULT);
}

bool checksumOk(const std::vector<uint8_t>& frame)
{
    uint8_t x = 0u;
    for (const uint8_t octet : frame) {
        x = static_cast<uint8_t>(x ^ octet);
    }
    return !frame.empty() && x == FCS_RESULT;
}

bool isStandard(const std::vector<uint8_t>& frame)
{
    return !frame.empty() && (frame[0] & CTRL_STANDARD) != 0u;
}

std::optional<std::pair<uint16_t, bool>> destination(const std::vector<uint8_t>& frame)
{
    if (frame.size() <= ADDRESS_TYPE_INDEX) {
        return std::nullopt;
    }
    if (isStandard(frame)) {
        return std::make_pair(static_cast<uint16_t>((frame[3] << 8) | frame[4]), (frame[5] & ADDRESS_TYPE_GROUP) != 0u);
    }
    return std::make_pair(static_cast<uint16_t>((frame[4] << 8) | frame[5]), (frame[1] & ADDRESS_TYPE_GROUP) != 0u);
}

} // namespace tp1_frame

// ============================================================================
// VirtualTp1CharacterBus
// ============================================================================

void VirtualTp1CharacterBus::at(uint64_t atUs, std::function<void()> fn)
{
    _events.emplace(std::make_pair(std::max(atUs, _nowUs), _sequence++), std::move(fn));
}

void VirtualTp1CharacterBus::runUntil(uint64_t untilUs)
{
    while (!_events.empty()) {
        auto it = _events.begin();
        if (it->first.first > untilUs) {
            break;
        }
        _nowUs = it->first.first;
        auto fn = std::move(it->second);
        _events.erase(it);
        fn();
    }
    _nowUs = std::max(_nowUs, untilUs);
}

void VirtualTp1CharacterBus::attach(Node& node)
{
    if (std::find(_nodes.begin(), _nodes.end(), &node) == _nodes.end()) {
        _nodes.push_back(&node);
    }
}

void VirtualTp1CharacterBus::detach(Node& node)
{
    _nodes.erase(std::remove(_nodes.begin(), _nodes.end(), &node), _nodes.end());
    _queue.erase(std::remove_if(_queue.begin(), _queue.end(), [&node](const Pending& p) { return p.sender == &node; }),
                 _queue.end());
}

void VirtualTp1CharacterBus::send(Node& sender, std::vector<uint8_t> frame, std::optional<size_t> corruptIndex)
{
    _queue.push_back(Pending{&sender, std::move(frame), corruptIndex});
    kick();
}

void VirtualTp1CharacterBus::kick()
{
    if (_transmitting || _startScheduled || _queue.empty()) {
        return;
    }
    _startScheduled = true;
    at(std::max(_nowUs, _idleSinceUs + bitsUs(FRAME_IDLE_BITS)), [this] {
        _startScheduled = false;
        startFrame();
    });
}

void VirtualTp1CharacterBus::startFrame()
{
    if (_transmitting || _queue.empty()) {
        return;
    }
    Pending pending = std::move(_queue.front());
    _queue.pop_front();
    if (pending.frame.empty()) {
        kick();
        return;
    }
    if (pending.corruptIndex && *pending.corruptIndex < pending.frame.size()) {
        pending.frame[*pending.corruptIndex] ^= CORRUPTION_BIT;
    }

    _transmitting = true;
    const size_t recordIndex = _log.size();
    Record record;
    record.startUs = _nowUs;
    record.endUs = _nowUs + bitsUs(static_cast<uint32_t>((pending.frame.size() - 1u) * CHARACTER_SPACING_BITS +
                                                         CHARACTER_BITS));
    record.frame = std::move(pending.frame);
    record.sender = pending.sender;
    _log.push_back(std::move(record));

    const Record& started = _log[recordIndex];
    for (size_t i = 0; i < started.frame.size(); ++i) {
        at(started.startUs + bitsUs(static_cast<uint32_t>(i * CHARACTER_SPACING_BITS + CHARACTER_BITS)),
           [this, recordIndex, i] {
               const std::vector<Node*> nodes = _nodes;
               for (Node* node : nodes) {
                   node->onBusCharacter(i, _log[recordIndex].frame[i], _log[recordIndex].sender);
               }
           });
    }
    at(started.endUs + bitsUs(ACK_GAP_BITS), [this, recordIndex] { acknowledgeSlot(recordIndex); });
}

void VirtualTp1CharacterBus::acknowledgeSlot(size_t recordIndex)
{
    // Every receiver answers at once; the line is a wired AND (a dominant 0
    // wins), so ACK and BUSY together read as BUSY, and ACK and NACK as NACK.
    std::optional<uint8_t> line;
    const std::vector<Node*> nodes = _nodes;
    for (Node* node : nodes) {
        if (node == _log[recordIndex].sender) {
            continue;
        }
        const std::optional<uint8_t> answer = node->acknowledgeFor(_log[recordIndex].frame);
        if (answer) {
            line = line ? static_cast<uint8_t>(*line & *answer) : *answer;
        }
    }
    _log[recordIndex].ack = line;

    // The slot ends after one character either way; with no answer the line
    // has been idle since the frame's end.
    const uint64_t slotEndUs = _nowUs + bitsUs(CHARACTER_BITS);
    const uint64_t idleSinceUs = line ? slotEndUs : _log[recordIndex].endUs;
    at(slotEndUs, [this, recordIndex, line, idleSinceUs] {
        _transmitting = false;
        _idleSinceUs = idleSinceUs;
        const Node* sender = _log[recordIndex].sender;
        const std::vector<Node*> listeners = _nodes;
        for (Node* node : listeners) {
            node->onAcknowledge(line, node == sender);
        }
        kick();
    });
}

// ============================================================================
// VirtualTpUartChip
// ============================================================================

VirtualTpUartChip::VirtualTpUartChip(VirtualTp1CharacterBus& bus,
                                     UartHalVirtual& uart,
                                     const VirtualTpUartChipConfig& config)
    : _bus(bus)
    , _uart(uart)
    , _config(config)
{
    _uart.setTxListener(&txListener, this);
    _bus.attach(*this);
}

VirtualTpUartChip::~VirtualTpUartChip()
{
    _uart.setTxListener(nullptr, nullptr);
    _bus.detach(*this);
}

bool VirtualTpUartChip::busyModeActive() const
{
    return _busyModeUntilUs != 0u && _bus.nowUs() < _busyModeUntilUs;
}

void VirtualTpUartChip::faultFrameOctetToHost(size_t index, bool drop, uint8_t mask, uint8_t flags)
{
    _frameFault = FrameOctetFault{index, drop, mask, flags, false};
}

void VirtualTpUartChip::pauseHostInput(uint64_t durationUs)
{
    _hostInPausedUntilUs = _bus.nowUs() + durationUs;
    _bus.at(_hostInPausedUntilUs, [this] { pullHostOctet(); });
}

void VirtualTpUartChip::setTemperatureWarning(bool warning)
{
    _temperatureWarning = warning ? STATE_TEMPERATURE_WARNING : 0u;
    answerLater(stateOctet());
}

// --- Host link -----------------------------------------------------------

void VirtualTpUartChip::txListener(void* context)
{
    static_cast<VirtualTpUartChip*>(context)->pullHostOctet();
}

void VirtualTpUartChip::pullHostOctet()
{
    // One octet in the shift register at a time; it leaves the host's queue
    // when it starts, as with a real UART.
    if (_hostInShifting || _bus.nowUs() < _hostInPausedUntilUs || _uart.txQueued() == 0u) {
        return;
    }
    const std::vector<uint8_t> octet = _uart.transmit(1u);
    if (octet.empty()) {
        return;
    }
    _hostInShifting = true;
    _bus.after(_config.hostOctetUs, [this, value = octet[0]] {
        _hostInShifting = false;
        onHostOctet(value);
        pullHostOctet();
    });
}

uint64_t VirtualTpUartChip::toHost(uint8_t octet, uint8_t flags)
{
    const uint64_t arrivesUs = std::max(_bus.nowUs(), _hostOutFreeAtUs) + _config.hostOctetUs;
    _hostOutFreeAtUs = arrivesUs;
    if (_dropToHost > 0u) {
        --_dropToHost;
        return arrivesUs;
    }
    _bus.at(arrivesUs, [this, octet, flags, arrivesUs] {
        _uart.setNowUs(arrivesUs);
        (void)_uart.deliverRx(octet, flags);
    });
    return arrivesUs;
}

void VirtualTpUartChip::answerLater(uint8_t octet)
{
    _bus.after(_config.responseDelayUs, [this, octet, generation = _resetGeneration] {
        if (generation == _resetGeneration && !_resetting) {
            (void)toHost(octet);
        }
    });
}

uint8_t VirtualTpUartChip::stateOctet() const
{
    return static_cast<uint8_t>(U_STATE_IND | _temperatureWarning);
}

void VirtualTpUartChip::reportError(uint8_t flag)
{
    // Error flags are events: the chip reports each with a U_State.ind of its
    // own (manual §3.2.3.1.12.3); a later U_State.req no longer shows it.
    if (flag == STATE_PROTOCOL_ERROR) {
        ++_stats.protocolErrors;
    }
    answerLater(static_cast<uint8_t>(stateOctet() | flag));
}

void VirtualTpUartChip::onHostOctet(uint8_t octet)
{
    _hostOctets.push_back(octet);
    if (_resetting) {
        ++_stats.hostOctetsIgnored;
        return;
    }
    if (_argumentsPending > 0u) {
        _arguments.push_back(octet);
        if (--_argumentsPending == 0u) {
            finishArguments();
        }
        return;
    }
    if (_dataIndex >= 0) {
        // The data octet of a U_L_Data pair.
        const auto index = static_cast<size_t>(_dataIndex);
        _dataIndex = -1;
        if (index != _expectedIndex) {
            _hostFrameBroken = true;
        }
        if (_hostFrame.size() <= index) {
            _hostFrame.resize(index + 1u);
        }
        _hostFrame[index] = octet;
        _expectedIndex = index + 1u;
        if (_dataIsEnd) {
            _dataIsEnd = false;
            finishHostFrame();
        }
        return;
    }
    onHostRequest(octet);
}

void VirtualTpUartChip::onHostRequest(uint8_t octet)
{
    _request = octet;
    _arguments.clear();

    if ((octet & U_L_DATA_MASK) == U_L_DATA_START_CONTINUE) {
        const uint8_t index = static_cast<uint8_t>(octet & U_L_DATA_INDEX_MASK);
        if (index == 0u) {
            _hostFrame.clear();
            _expectedIndex = 0u;
            _hostFrameBroken = false;
        }
        _dataIndex = index;
        _dataIsEnd = false;
        return;
    }
    if ((octet & U_L_DATA_MASK) == U_L_DATA_END) {
        _dataIndex = octet & U_L_DATA_INDEX_MASK;
        _dataIsEnd = true;
        return;
    }
    if ((octet & U_ACK_INFORMATION_MASK) == U_ACK_INFORMATION) {
        onAckInformation(octet);
        return;
    }

    switch (octet) {
    case U_RESET_REQ:
        reset(true);
        break;
    case U_STATE_REQ:
        ++_stats.stateRequests;
        answerLater(stateOctet());
        break;
    case U_BUSMON_REQ:
        _busMonitor = true;
        break;
    case U_PRODUCT_ID_REQ:
        answerLater(PRODUCT_ID);
        break;
    case U_ACTIVATE_BUSY_MODE:
        _busyModeUntilUs = _bus.nowUs() + _config.busyModeUs;
        break;
    case U_RESET_BUSY_MODE:
        _busyModeUntilUs = 0u;
        break;
    case U_MXRSTCNT:
        _argumentsPending = 1u;
        break;
    case U_SET_ADDRESS:
        _argumentsPending = 2u;
        break;
    default:
        reportError(STATE_PROTOCOL_ERROR);
        break;
    }
}

void VirtualTpUartChip::finishArguments()
{
    if (_request == U_MXRSTCNT) {
        _busyRepetitions = static_cast<uint8_t>((_arguments[0] >> BUSY_REPETITION_SHIFT) & REPETITION_MASK);
        _nackRepetitions = static_cast<uint8_t>(_arguments[0] & REPETITION_MASK);
    } else if (_request == U_SET_ADDRESS) {
        // Active once no frame is being received (manual §3.2.3.1.7).
        const auto address = static_cast<uint16_t>((_arguments[0] << 8) | _arguments[1]);
        if (_rxActive) {
            _pendingAddress = address;
        } else {
            _address = address;
        }
    }
}

void VirtualTpUartChip::onAckInformation(uint8_t octet)
{
    // Any U_AckInformation ends busy mode (manual §3.2.3.1.5).
    _busyModeUntilUs = 0u;
    if (!_rxActive) {
        ++_stats.strayAckInformation;
        return;
    }
    if (_bus.nowUs() > _rxDeadlineUs) {
        ++_stats.lateAckInformation;
        return;
    }
    ++_stats.ackInformation;
    _rxHostAck = HostAck{(octet & U_ACK_ADDRESSED) != 0u, (octet & U_ACK_NACK) != 0u, (octet & U_ACK_BUSY) != 0u};
}

void VirtualTpUartChip::finishHostFrame()
{
    // One frame buffer: a second frame before the first is confirmed is
    // refused with the protocol error flag (manual §3.2.3.1.9).
    if (_txActive) {
        reportError(STATE_PROTOCOL_ERROR);
        return;
    }
    if (_hostFrameBroken || !lengthMatches(_hostFrame) || !tp1_frame::checksumOk(_hostFrame)) {
        reportError(STATE_RECEIVE_ERROR);  // U_L_DataEnd with a wrong checksum (§3.2.3.1.9.3)
        return;
    }
    ++_stats.framesFromHost;
    _txActive = true;
    _txFrame = _hostFrame;
    // A cleared repeat flag means "send once", and the frame then goes out
    // with the flag set (§3.2.3.1.9.1).
    _txRepeats = (_txFrame[0] & CTRL_NOT_REPEATED) != 0u;
    if (!_txRepeats) {
        _txFrame[0] = static_cast<uint8_t>(_txFrame[0] | CTRL_NOT_REPEATED);
        refreshChecksum(_txFrame);
    }
    _busyLeft = _busyRepetitions;
    _nackLeft = _nackRepetitions;
    sendToBus();
}

void VirtualTpUartChip::sendToBus()
{
    ++_stats.busTransmissions;
    _bus.send(*this, _txFrame);
}

void VirtualTpUartChip::confirm(bool positive)
{
    _txActive = false;
    ++(positive ? _stats.confirmsPositive : _stats.confirmsNegative);
    if (_dropConfirm) {
        _dropConfirm = false;
        return;
    }
    (void)toHost(positive ? L_DATA_CON_POSITIVE : L_DATA_CON_NEGATIVE);
}

void VirtualTpUartChip::reset(bool requested)
{
    ++_stats.resets;
    _resetting = true;
    const uint32_t generation = ++_resetGeneration;

    _address.reset();
    _pendingAddress.reset();
    _busMonitor = false;
    _busyModeUntilUs = 0u;
    _busyRepetitions = 3u;
    _nackRepetitions = 3u;
    _argumentsPending = 0u;
    _dataIndex = -1;
    _dataIsEnd = false;
    _hostFrame.clear();
    _hostFrameBroken = true;
    _txActive = false;
    _rxActive = false;
    _rxHostAck.reset();

    if (requested && _config.resetGlitch) {
        _bus.after(RESET_GLITCH_DELAY_US, [this, generation] {
            if (generation == _resetGeneration) {
                (void)toHost(0x00u, KNX_UART_HAL_RX_FRAMING_ERROR);
            }
        });
    }
    _bus.after(_config.resetIndicationDelayUs, [this, generation] {
        if (generation == _resetGeneration) {
            _resetting = false;
            (void)toHost(U_RESET_IND);
        }
    });
}

// --- Bus side --------------------------------------------------------------

void VirtualTpUartChip::onBusCharacter(size_t index, uint8_t octet, const VirtualTp1CharacterBus::Node* sender)
{
    if (_resetting) {
        return;
    }
    if (sender == this) {
        // Our own frame comes back octet by octet as it goes out (seen on the
        // H8i8o); the chip does not evaluate it (§3.2.3.1.7).
        if (_txActive) {
            (void)toHost(octet);
        }
        return;
    }

    if (index == 0u) {
        _rxActive = true;
        _rxFrame.clear();
        _rxChipAddressed = false;
        _rxDeadlineUs = std::numeric_limits<uint64_t>::max();
        _rxHostAck.reset();
    }
    if (!_rxActive) {
        return;  // came out of reset in the middle of a frame
    }
    _rxFrame.push_back(octet);

    uint8_t toHostOctet = octet;
    uint8_t flags = 0u;
    bool dropped = false;
    if (_frameFault) {
        _frameFault->armed = _frameFault->armed || index == 0u;
        if (_frameFault->armed && index == _frameFault->index) {
            dropped = _frameFault->drop;
            toHostOctet = static_cast<uint8_t>(octet ^ _frameFault->mask);
            flags = _frameFault->flags;
            _frameFault.reset();
        }
    }
    if (dropped) {
        ++_dropToHost;  // the octet still takes its time on the link
    }
    const uint64_t deliveredUs = toHost(toHostOctet, flags);

    if (index == ADDRESS_TYPE_INDEX) {
        _rxDeadlineUs = deliveredUs + _config.ackInformationDeadlineUs;
        // Address evaluation, once U_SetAddress has given the chip an address:
        // every group frame, and individual frames to that address.
        const auto dest = tp1_frame::destination(_rxFrame);
        if (_address && dest) {
            _rxChipAddressed = dest->second || dest->first == *_address;
        }
    }
}

std::optional<uint8_t> VirtualTpUartChip::acknowledgeFor(const std::vector<uint8_t>& frame)
{
    const bool wasActive = _rxActive;
    _rxActive = false;
    if (_pendingAddress) {
        _address = _pendingAddress;
        _pendingAddress.reset();
    }
    if (!wasActive || _resetting || _busMonitor) {
        return std::nullopt;  // bus monitor mode is silent on the line
    }

    bool addressed = _rxChipAddressed;
    bool nack = false;
    bool busy = false;
    if (_rxHostAck) {
        if (_rxHostAck->addressed) {
            addressed = true;
            nack = _rxHostAck->nack;
            busy = _rxHostAck->busy;
        } else if (_config.clearedAddressedBitWithdraws) {
            addressed = false;
        }
    }

    const bool intact = lengthMatches(frame) && tp1_frame::checksumOk(frame);
    if (!intact) {
        reportError(STATE_RECEIVE_ERROR);
    }
    if (!addressed) {
        return std::nullopt;
    }
    if (!intact || nack) {
        ++_stats.nacksSent;
        return VirtualTp1CharacterBus::NACK;
    }
    if (busy || busyModeActive()) {
        ++_stats.busyAnswers;
        return VirtualTp1CharacterBus::BUSY;
    }
    ++_stats.acksSent;
    return VirtualTp1CharacterBus::ACK;
}

void VirtualTpUartChip::onAcknowledge(std::optional<uint8_t> ack, bool ownFrame)
{
    if (!ownFrame || !_txActive) {
        return;
    }
    if (ack == VirtualTp1CharacterBus::ACK) {
        confirm(true);
        return;
    }
    // BUSY uses the BUSY count; NACK, no answer and anything garbled the NACK
    // count (manual §3.2.3.1.9.4).
    uint8_t& left = ack == VirtualTp1CharacterBus::BUSY ? _busyLeft : _nackLeft;
    if (!_txRepeats || left == 0u) {
        confirm(false);
        return;
    }
    --left;
    if ((_txFrame[0] & CTRL_NOT_REPEATED) != 0u) {
        _txFrame[0] = static_cast<uint8_t>(_txFrame[0] & ~CTRL_NOT_REPEATED);
        refreshChecksum(_txFrame);
    }
    sendToBus();
}

// ============================================================================
// VirtualTp1Partner
// ============================================================================

VirtualTp1Partner::VirtualTp1Partner(VirtualTp1CharacterBus& bus, uint16_t individualAddress)
    : _bus(bus)
    , _address(individualAddress)
{
    _bus.attach(*this);
}

VirtualTp1Partner::~VirtualTp1Partner()
{
    _bus.detach(*this);
}

void VirtualTp1Partner::send(std::vector<uint8_t> frame, std::optional<size_t> corruptIndex)
{
    _bus.send(*this, std::move(frame), corruptIndex);
}

void VirtualTp1Partner::onBusCharacter(size_t, uint8_t, const VirtualTp1CharacterBus::Node*) {}

std::optional<uint8_t> VirtualTp1Partner::acknowledgeFor(const std::vector<uint8_t>& frame)
{
    _received.push_back(frame);
    if (answer) {
        return answer(frame);
    }
    const auto dest = tp1_frame::destination(frame);
    if (!dest) {
        return std::nullopt;
    }
    const bool addressed = dest->second ? std::find(groups.begin(), groups.end(), dest->first) != groups.end()
                                        : dest->first == _address;
    if (!addressed) {
        return std::nullopt;
    }
    return lengthMatches(frame) && tp1_frame::checksumOk(frame) ? VirtualTp1CharacterBus::ACK
                                                                : VirtualTp1CharacterBus::NACK;
}

void VirtualTp1Partner::onAcknowledge(std::optional<uint8_t> ack, bool ownFrame)
{
    if (ownFrame) {
        _acks.push_back(ack);
    }
}

} // namespace physical
} // namespace knx
