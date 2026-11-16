// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file virtual_tpuart_chip.hpp
 * @brief Host-only TP-UART2 simulator and the character-level TP1 bus it sits on (K4)
 *
 * Three pieces, all driven by one microsecond clock that the bus owns:
 *
 *   - VirtualTp1CharacterBus: a TP1 line at the level of characters. A frame
 *     is a run of 11-bit characters 13 bit times apart, followed 15 bit times
 *     later by the acknowledge slot, where the answers of all receivers are
 *     wired-ANDed. A frame starts once the line has been idle for 50 bit
 *     times (03_02_02 §2.3). There is no bit-level arbitration: frames wait
 *     their turn in the order they were asked for.
 *   - VirtualTpUartChip: the chip's host protocol on a UartHalVirtual, as the
 *     Siemens TP-UART2 technical manual (A6V11933793, June 2024) describes it
 *     and as the H8i8o's chip showed it (design note
 *     docs/design/cortex-m-tpuart2-support.md §3.8). It clocks host octets in
 *     and out at the UART's octet time, passes every bus octet to the host as
 *     it arrives (its own frames too), acknowledges on the bus by its own
 *     address evaluation or as U_AckInformation tells it (if that arrives
 *     within 1.7 ms of the address-type octet), repeats unacknowledged frames
 *     and confirms them with L_Data.con.
 *   - VirtualTp1Partner: another device on the line, which sends scripted
 *     frames and answers frames with a scripted acknowledge.
 *
 * Fault injection: lost and corrupted octets towards the host, a missing
 * L_Data.con, BUSY or NACK from partners (their answer function), corrupted
 * octets on the bus, a stalled host input (a late U_AckInformation) and a
 * chip reset at any moment.
 *
 * Not modelled: bit-level timing and collisions, priorities in the idle time,
 * L_PollData, the CRC option, the SAVE output and the chip's 5 V / 20 V
 * supplies.
 *
 * @thread_safety NOT thread-safe; one test thread drives everything. Host
 * octets reach the backend's RX callback synchronously from runUntil().
 */

#pragma once

#include "knx/physical/uart_hal_virtual.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace knx {
namespace physical {

class VirtualTp1CharacterBus {
public:
    /// 9600 bit/s; 104.17 us rounded down, which keeps the arithmetic exact.
    static constexpr uint32_t BIT_US = 104u;
    static constexpr uint32_t CHARACTER_BITS = 11u;          ///< start, 8 data, parity, stop
    static constexpr uint32_t CHARACTER_SPACING_BITS = 13u;  ///< 2 idle bits between characters of a frame
    static constexpr uint32_t ACK_GAP_BITS = 15u;            ///< frame end to acknowledge start
    static constexpr uint32_t FRAME_IDLE_BITS = 50u;         ///< idle line before a frame
    static constexpr uint8_t ACK = 0xCCu;
    static constexpr uint8_t NACK = 0x0Cu;
    static constexpr uint8_t BUSY = 0xC0u;

    class Node {
    public:
        virtual ~Node() = default;
        /// One character of a frame, at the end of its stop bit. Every node
        /// hears every frame, the sender included.
        virtual void onBusCharacter(size_t index, uint8_t octet, const Node* sender) = 0;
        /// At the acknowledge slot: this node's answer to someone else's frame, if any.
        virtual std::optional<uint8_t> acknowledgeFor(const std::vector<uint8_t>& frame) = 0;
        /// The acknowledge slot is over: what the line carried, if anything.
        virtual void onAcknowledge(std::optional<uint8_t> ack, bool ownFrame) = 0;
    };

    /// One frame as it went over the line.
    struct Record {
        uint64_t startUs{0};
        uint64_t endUs{0};  ///< end of the last character's stop bit
        std::vector<uint8_t> frame;
        const Node* sender{nullptr};
        std::optional<uint8_t> ack;
    };

    VirtualTp1CharacterBus() = default;
    VirtualTp1CharacterBus(const VirtualTp1CharacterBus&) = delete;
    VirtualTp1CharacterBus& operator=(const VirtualTp1CharacterBus&) = delete;

    // --- Clock ------------------------------------------------------------

    uint64_t nowUs() const { return _nowUs; }
    /// Run fn at atUs (not before now). Events at the same time run in the
    /// order they were scheduled.
    void at(uint64_t atUs, std::function<void()> fn);
    void after(uint64_t delayUs, std::function<void()> fn) { at(_nowUs + delayUs, std::move(fn)); }
    /// Run every event up to and including untilUs, then stand at untilUs.
    void runUntil(uint64_t untilUs);
    void runFor(uint64_t durationUs) { runUntil(_nowUs + durationUs); }
    bool hasPendingEvents() const { return !_events.empty(); }

    // --- Line -------------------------------------------------------------

    void attach(Node& node);
    void detach(Node& node);
    /// Queue a frame (FCS included) for sending. corruptIndex flips bit 0 of
    /// that octet on the line, as every node hears it.
    void send(Node& sender, std::vector<uint8_t> frame, std::optional<size_t> corruptIndex = std::nullopt);
    bool busy() const { return _transmitting; }

    const std::vector<Record>& log() const { return _log; }
    void clearLog() { _log.clear(); }

    static uint64_t bitsUs(uint32_t bits) { return static_cast<uint64_t>(bits) * BIT_US; }

private:
    struct Pending {
        Node* sender{nullptr};
        std::vector<uint8_t> frame;
        std::optional<size_t> corruptIndex;
    };

    void kick();
    void startFrame();
    void acknowledgeSlot(size_t recordIndex);

    uint64_t _nowUs{0};
    uint64_t _sequence{0};
    std::map<std::pair<uint64_t, uint64_t>, std::function<void()>> _events;

    std::vector<Node*> _nodes;
    std::deque<Pending> _queue;
    bool _transmitting{false};
    bool _startScheduled{false};
    uint64_t _idleSinceUs{0};
    std::vector<Record> _log;
};

struct VirtualTpUartChipConfig {
    /// One 8E1 octet (11 bits) at 19200 baud.
    uint32_t hostOctetUs{573u};
    /// Time the chip takes before it answers a request. U_State.ind came
    /// 1.4 ms after U_State.req was written on the H8i8o: two octet times and this.
    uint32_t responseDelayUs{250u};
    /// U_Reset.req octet received to U_Reset.ind queued. With the two octet
    /// times this gives the 6.9 ms from request written to U_Reset.ind seen
    /// on the H8i8o.
    uint32_t resetIndicationDelayUs{5750u};
    /// After U_Reset.req the H8i8o's chip sent one 00 octet with a framing
    /// error about 1.4 ms later (its TXD glitches while it resets).
    bool resetGlitch{true};
    /// U_AckInformation must be in this long after the host received the
    /// address-type octet (manual §3.2.3.1.8).
    uint32_t ackInformationDeadlineUs{1700u};
    /// Whether U_AckInformation with the addressed bit clear withdraws the
    /// chip's own acknowledgement. The manual does not say (design note §3.9).
    bool clearedAddressedBitWithdraws{true};
    /// Busy mode lasts this long unless ended earlier (manual §3.2.3.1.5).
    uint32_t busyModeUs{700000u};
};

class VirtualTpUartChip final : public VirtualTp1CharacterBus::Node {
public:
    static constexpr uint8_t PRODUCT_ID = 0x41u;  ///< the H8i8o's chip
    // U_State.ind bits (manual §3.2.3.1.12.3).
    static constexpr uint8_t STATE_SLAVE_COLLISION = 0x80u;
    static constexpr uint8_t STATE_RECEIVE_ERROR = 0x40u;
    static constexpr uint8_t STATE_TRANSMIT_ERROR = 0x20u;
    static constexpr uint8_t STATE_PROTOCOL_ERROR = 0x10u;
    static constexpr uint8_t STATE_TEMPERATURE_WARNING = 0x08u;

    struct Statistics {
        uint32_t resets{0};                ///< requested and spontaneous
        uint32_t stateRequests{0};
        uint32_t framesFromHost{0};        ///< complete U_L_Data frames accepted
        uint32_t busTransmissions{0};      ///< frames put on the line, repetitions included
        uint32_t confirmsPositive{0};
        uint32_t confirmsNegative{0};
        uint32_t ackInformation{0};        ///< U_AckInformation in time for its frame
        uint32_t lateAckInformation{0};    ///< after the deadline: ignored
        uint32_t strayAckInformation{0};   ///< with no frame being received: ignored
        uint32_t protocolErrors{0};
        uint32_t hostOctetsIgnored{0};     ///< arrived while the chip was resetting
        uint32_t busyAnswers{0};           ///< BUSY this chip put on the line
        uint32_t acksSent{0};              ///< ACK this chip put on the line
        uint32_t nacksSent{0};             ///< NACK this chip put on the line
    };

    VirtualTpUartChip(VirtualTp1CharacterBus& bus, UartHalVirtual& uart, const VirtualTpUartChipConfig& config = {});
    ~VirtualTpUartChip() override;

    VirtualTpUartChip(const VirtualTpUartChip&) = delete;
    VirtualTpUartChip& operator=(const VirtualTpUartChip&) = delete;

    // --- Observation ------------------------------------------------------

    /// The address from U_SetAddress, once active; reset clears it.
    std::optional<uint16_t> address() const { return _address; }
    bool busMonitor() const { return _busMonitor; }
    bool busyModeActive() const;
    uint8_t busyRepetitions() const { return _busyRepetitions; }
    uint8_t nackRepetitions() const { return _nackRepetitions; }
    bool resetting() const { return _resetting; }
    const Statistics& statistics() const { return _stats; }
    /// Every octet the host sent, in order, as the chip received it.
    const std::vector<uint8_t>& hostOctets() const { return _hostOctets; }

    // --- Faults and conditions -------------------------------------------

    /// The next n octets towards the host, whatever they are, are lost.
    void dropToHost(size_t n = 1u) { _dropToHost += n; }
    /// In the next frame from another device, the octet at index reaches the
    /// host XORed with mask and with the UART error flags given, or not at
    /// all (drop). One fault at a time; the frame on the line is unaffected.
    void faultFrameOctetToHost(size_t index, bool drop, uint8_t mask = 0u, uint8_t flags = 0u);
    /// The next L_Data.con is not sent.
    void dropNextConfirm() { _dropConfirm = true; }
    /// The chip stops reading host octets for durationUs; they wait in the
    /// host's UART (a late U_AckInformation, a stalled link).
    void pauseHostInput(uint64_t durationUs);
    /// Reset as if by a supply dip: state lost, U_Reset.ind follows.
    void resetSpontaneously() { reset(false); }
    /// Sets or clears the temperature warning, with a U_State.ind.
    void setTemperatureWarning(bool warning);

    // --- VirtualTp1CharacterBus::Node -------------------------------------

    void onBusCharacter(size_t index, uint8_t octet, const VirtualTp1CharacterBus::Node* sender) override;
    std::optional<uint8_t> acknowledgeFor(const std::vector<uint8_t>& frame) override;
    void onAcknowledge(std::optional<uint8_t> ack, bool ownFrame) override;

private:
    struct HostAck {
        bool addressed{false};
        bool nack{false};
        bool busy{false};
    };
    struct FrameOctetFault {
        size_t index{0};
        bool drop{false};
        uint8_t mask{0};
        uint8_t flags{0};
        bool armed{false};  ///< set once the frame it applies to has started
    };

    static void txListener(void* context);
    void pullHostOctet();
    void onHostOctet(uint8_t octet);
    void onHostRequest(uint8_t octet);
    void finishArguments();
    void onAckInformation(uint8_t octet);
    void finishHostFrame();
    void sendToBus();
    void confirm(bool positive);
    uint64_t toHost(uint8_t octet, uint8_t flags = 0u);
    void answerLater(uint8_t octet);
    void reportError(uint8_t flag);
    uint8_t stateOctet() const;
    void reset(bool requested);

    VirtualTp1CharacterBus& _bus;
    UartHalVirtual& _uart;
    VirtualTpUartChipConfig _config;
    Statistics _stats;
    std::vector<uint8_t> _hostOctets;

    // Host link
    bool _hostInShifting{false};
    uint64_t _hostInPausedUntilUs{0};
    uint64_t _hostOutFreeAtUs{0};
    size_t _dropToHost{0};
    std::optional<FrameOctetFault> _frameFault;

    // Chip state
    bool _resetting{false};
    uint32_t _resetGeneration{0};
    std::optional<uint16_t> _address;
    std::optional<uint16_t> _pendingAddress;
    bool _busMonitor{false};
    uint64_t _busyModeUntilUs{0};
    uint8_t _busyRepetitions{3u};
    uint8_t _nackRepetitions{3u};
    uint8_t _temperatureWarning{0};

    // Host request decoding
    uint8_t _request{0};
    std::vector<uint8_t> _arguments;
    size_t _argumentsPending{0};
    int _dataIndex{-1};
    bool _dataIsEnd{false};
    size_t _expectedIndex{0};
    bool _hostFrameBroken{true};  ///< until a U_L_DataStart begins a frame
    std::vector<uint8_t> _hostFrame;

    // Transmit (frame buffer)
    bool _txActive{false};
    std::vector<uint8_t> _txFrame;
    bool _txRepeats{false};
    uint8_t _busyLeft{0};
    uint8_t _nackLeft{0};
    bool _dropConfirm{false};

    // Receive (someone else's frame)
    bool _rxActive{false};
    std::vector<uint8_t> _rxFrame;
    bool _rxChipAddressed{false};
    uint64_t _rxDeadlineUs{0};
    std::optional<HostAck> _rxHostAck;
};

class VirtualTp1Partner final : public VirtualTp1CharacterBus::Node {
public:
    /// Decides the answer to a frame on the line; nullopt stays silent.
    using AnswerFn = std::function<std::optional<uint8_t>(const std::vector<uint8_t>& frame)>;

    VirtualTp1Partner(VirtualTp1CharacterBus& bus, uint16_t individualAddress);
    ~VirtualTp1Partner() override;

    VirtualTp1Partner(const VirtualTp1Partner&) = delete;
    VirtualTp1Partner& operator=(const VirtualTp1Partner&) = delete;

    uint16_t address() const { return _address; }
    /// Group addresses this partner acknowledges with the default answer.
    std::vector<uint16_t> groups;
    /// Null: ACK frames to the own address or to a group in groups.
    AnswerFn answer;

    void send(std::vector<uint8_t> frame, std::optional<size_t> corruptIndex = std::nullopt);

    /// Frames from others, as heard (checksum not checked).
    const std::vector<std::vector<uint8_t>>& received() const { return _received; }
    /// What the line carried after each of this partner's own frames.
    const std::vector<std::optional<uint8_t>>& acksToOwnFrames() const { return _acks; }

    void onBusCharacter(size_t index, uint8_t octet, const VirtualTp1CharacterBus::Node* sender) override;
    std::optional<uint8_t> acknowledgeFor(const std::vector<uint8_t>& frame) override;
    void onAcknowledge(std::optional<uint8_t> ack, bool ownFrame) override;

private:
    VirtualTp1CharacterBus& _bus;
    uint16_t _address;
    std::vector<std::vector<uint8_t>> _received;
    std::vector<std::optional<uint8_t>> _acks;
};

/// Helpers for frames as they are on the line (03_02_02 §2.2).
namespace tp1_frame {
/// FCS: the XOR of all octets, FCS included, is FFh.
uint8_t checksum(const std::vector<uint8_t>& withoutFcs);
bool checksumOk(const std::vector<uint8_t>& frame);
bool isStandard(const std::vector<uint8_t>& frame);
/// Destination address and whether it is a group address; nullopt for a frame too short to say.
std::optional<std::pair<uint16_t, bool>> destination(const std::vector<uint8_t>& frame);
} // namespace tp1_frame

} // namespace physical
} // namespace knx
