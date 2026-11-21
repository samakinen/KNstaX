// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tpuart_medium_backend.cpp
 * @brief TpUartMediumBackend against a scripted TP-UART2 on UartHalVirtual
 *
 * ScriptedChip answers the host protocol the way the TP-UART2 manual
 * describes it: U_Reset.ind for a reset, U_State.ind for a state request,
 * L_Data.con once a U_L_Data frame is complete. It decodes everything the
 * backend sends, so the tests check the octets on the host link. The chip
 * simulator with a bus and fault injection is K4; this is only what the
 * backend's own behaviour needs.
 *
 * Time is a counter: CooperativePlatform's idle hook advances it by 1 ms and
 * lets the chip answer, so the backend's blocking reset wait and the data
 * link layer's send loop run against it.
 */

#include "unity.h"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/objects/address_table_object.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tpuart_medium_backend.hpp"
#include "knx/physical/uart_hal_virtual.hpp"
#include "knx/platform/cooperative_platform.hpp"
#include "knx/types.hpp"

#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

using namespace knx;
using namespace knx::physical;
using knx::datalink::LDataFrame;

namespace {

constexpr uint16_t OWN_ADDRESS = 0x110A;  // 1.1.10
constexpr uint16_t PEER_ADDRESS = 0x110B; // 1.1.11
constexpr uint16_t GROUP_IN_TABLE = 0x0801;
constexpr uint16_t GROUP_NOT_IN_TABLE = 0x0901;
constexpr uint8_t ACK_ADDRESSED = 0x11u;
constexpr uint8_t ACK_BUSY = 0x13u;
constexpr uint32_t OCTET_US = 573u;  // one 8E1 octet at 19200 baud

class ScriptedChip {
public:
    enum class Confirm { Positive, Negative, None };

    explicit ScriptedChip(UartHalVirtual& uart)
        : _uart(uart)
    {
    }

    bool responsive{true};
    bool echo{true};  // pass our own frames back, as the real chip does
    Confirm confirm{Confirm::Positive};
    uint8_t stateValue{0x07u};  // U_State.ind, no warning bits

    std::vector<uint8_t> hostOctets;  // everything the backend sent, in order
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint8_t> acks;
    std::optional<uint16_t> address;
    std::optional<uint8_t> repetitions;
    int resets{0};
    int stateRequests{0};
    bool busmon{false};
    int busyModeOn{0};   // U_ActivateBusyMode
    int busyModeOff{0};  // U_ResetBusyMode

    /// Take up to maxOctets off the host link and act on them.
    void pump(size_t maxOctets = SIZE_MAX)
    {
        for (uint8_t octet : _uart.transmit(maxOctets)) {
            hostOctets.push_back(octet);
            onHostOctet(octet);
        }
    }

private:
    void onHostOctet(uint8_t octet)
    {
        if (_argsPending > 0u) {
            _args.push_back(octet);
            if (--_argsPending == 0u) {
                finishRequest();
            }
            return;
        }
        if (_dataIndex >= 0) {
            if (_building.size() <= static_cast<size_t>(_dataIndex)) {
                _building.resize(static_cast<size_t>(_dataIndex) + 1u);
            }
            _building[static_cast<size_t>(_dataIndex)] = octet;
            _dataIndex = -1;
            if (_frameEnds) {
                _frameEnds = false;
                frames.push_back(_building);
                if (echo) {
                    // The TP-UART2 passes the frame back while it is on the bus
                    // (seen on the H8i8o), an octet every 1.35 ms.
                    for (const uint8_t octet : _building) {
                        _uart.advanceUs(1354u);
                        (void)_uart.deliverRx(octet);
                    }
                }
                _building.clear();
                if (confirm == Confirm::Positive) {
                    (void)_uart.deliverRx(0x8Bu);
                } else if (confirm == Confirm::Negative) {
                    (void)_uart.deliverRx(0x0Bu);
                }
            }
            return;
        }

        _request = octet;
        _args.clear();
        if (octet == 0x01u) {
            ++resets;
            busmon = false;
            if (responsive) {
                (void)_uart.deliverRx(0x03u);
            }
        } else if (octet == 0x02u) {
            ++stateRequests;
            if (responsive) {
                (void)_uart.deliverRx(stateValue);
            }
        } else if (octet == 0x05u) {
            busmon = true;
        } else if ((octet & 0xF8u) == 0x10u) {
            acks.push_back(octet);
        } else if (octet == 0x21u) {
            ++busyModeOn;
        } else if (octet == 0x22u) {
            ++busyModeOff;
        } else if (octet == 0x24u) {
            _argsPending = 1u;
        } else if (octet == 0x28u) {
            _argsPending = 2u;
        } else if ((octet & 0xC0u) == 0x80u) {
            _dataIndex = octet & 0x3F;
        } else if ((octet & 0xC0u) == 0x40u) {
            _dataIndex = octet & 0x3F;
            _frameEnds = true;
        }
    }

    void finishRequest()
    {
        if (_request == 0x24u) {
            repetitions = _args[0];
        } else if (_request == 0x28u) {
            address = static_cast<uint16_t>((_args[0] << 8) | _args[1]);
        }
    }

    UartHalVirtual& _uart;
    uint8_t _request{0};
    std::vector<uint8_t> _args;
    size_t _argsPending{0};
    int _dataIndex{-1};
    bool _frameEnds{false};
    std::vector<uint8_t> _building;
};

uint32_t g_ms = 0;
UartHalVirtual* g_uart = nullptr;
ScriptedChip* g_chip = nullptr;

void advanceMs(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; ++i) {
        ++g_ms;
        g_uart->setNowUs(static_cast<uint64_t>(g_ms) * 1000u);
        g_chip->pump();
    }
}

knx::platform::CooperativeHooks makeHooks()
{
    knx::platform::CooperativeHooks hooks{};
    hooks.millis = [](void*) -> uint32_t { return g_ms; };
    hooks.micros = [](void*) -> uint64_t { return static_cast<uint64_t>(g_ms) * 1000u; };
    hooks.idle = [](void*) { advanceMs(1u); };
    return hooks;
}

/// A group address list kept the way the address table keeps it: fixed
/// storage, and every change bracketed for an interrupt-side reader.
struct SharedList {
    static constexpr uint16_t CAPACITY = 16;
    std::array<GroupAddress, CAPACITY> entries{};
    util::SharedGroupAddressState state;

    void set(std::initializer_list<uint16_t> addresses, bool sorted = true)
    {
        state.beginChange();
        size_t count = 0;
        for (const uint16_t address : addresses) {
            entries[count++] = GroupAddress(address);
        }
        (void)sorted;  // the reader scans; order does not matter to it
        state.endChange(count);
    }

    util::SharedGroupAddressView view() const { return state.view(entries.data(), CAPACITY); }
};

struct Fixture {
    SharedList groups;
    UartHalVirtual uart{16};
    ScriptedChip chip{uart};
    knx::platform::CooperativePlatform platform{makeHooks()};
    knx_uart_hal_t hal{};
    std::unique_ptr<TpUartMediumBackend> backend;
    std::vector<Tp1RxEvent> events;
    std::vector<std::vector<uint8_t>> telegrams;

    explicit Fixture(UartHalVirtual::Pins pins = {}, TpUartConfig config = {})
    {
        g_ms = 0;
        g_uart = &uart;
        g_chip = &chip;
        uart.bind(hal, pins);
        backend = std::make_unique<TpUartMediumBackend>(hal, platform, config);
        backend->setEventCallback(
            [this](const Tp1RxEvent& event, void*) {
                events.push_back(event);
                if (event.type == Tp1RxEventType::TelegramEnd) {
                    telegrams.emplace_back(event.frame.begin(), event.frame.end());
                }
            },
            nullptr);
    }

    /// Set the list and hand the backend its view, as the data link layer does.
    void ackGroups(std::initializer_list<uint16_t> addresses, bool sorted = true)
    {
        groups.set(addresses, sorted);
        TEST_ASSERT_TRUE(backend->setAckGroupAddressView(groups.view()));
    }

    util::Result<void> initOnly(uint16_t ownAddress = OWN_ADDRESS)
    {
        Tp1MediumConfig config;
        config.ownIndividualAddressRaw = ownAddress;
        return backend->init(config);
    }

    /// init(), then the first service() an owner's loop gives it: the address
    /// goes to the chip there.
    util::Result<void> init(uint16_t ownAddress = OWN_ADDRESS)
    {
        auto result = initOnly(ownAddress);
        if (result.isOk()) {
            TEST_ASSERT_TRUE(backend->service().isOk());
        }
        return result;
    }

    /// The chip answers what is queued, then the backend services.
    void serviceRound()
    {
        chip.pump();
        TEST_ASSERT_TRUE(backend->service().isOk());
        chip.pump();
    }

    void deliver(const std::vector<uint8_t>& octets, uint32_t gapUs = OCTET_US, uint8_t flagsOn = 0xFF,
                 uint8_t flags = 0u)
    {
        for (size_t i = 0; i < octets.size(); ++i) {
            uart.advanceUs(gapUs);
            TEST_ASSERT_TRUE(uart.deliverRx(octets[i], i == flagsOn ? flags : 0u));
        }
    }

    size_t countEvents(Tp1RxEventType type) const
    {
        size_t n = 0;
        for (const auto& e : events) {
            n += e.type == type ? 1u : 0u;
        }
        return n;
    }

    std::optional<Tp1RxEvent> lastEvent(Tp1RxEventType type) const
    {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->type == type) {
                return *it;
            }
        }
        return std::nullopt;
    }
};

std::vector<uint8_t> encode(const LDataFrame& frame)
{
    std::vector<uint8_t> buffer(300);
    auto encoded = datalink::FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(encoded.isOk());
    buffer.resize(encoded.value());
    return buffer;
}

LDataFrame groupWrite(uint16_t source, uint16_t destination, uint8_t value)
{
    LDataFrame frame;
    frame.standardFrame = true;
    frame.priority = Priority::Low;
    frame.ackRequested = false;  // certified senders keep CTRL bits 1..0 clear on the wire
    frame.source = IndividualAddress(source);
    frame.destination = GroupAddress(destination);
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;
    frame.setTpdu(protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {value});
    return frame;
}

LDataFrame individualRead(uint16_t source, uint16_t destination)
{
    LDataFrame frame;
    frame.standardFrame = true;
    frame.priority = Priority::Low;
    frame.ackRequested = false;  // certified senders keep CTRL bits 1..0 clear on the wire
    frame.source = IndividualAddress(source);
    frame.destination = GroupAddress(destination);
    frame.destinationType = AddressType::Individual;
    frame.hopCount = 6;
    frame.setTpdu(protocol::TPCI::UnnumberedData, application::APCIService::DeviceDescriptorRead, {0x00u});
    return frame;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- Lifecycle ---------------------------------------------------------------

void test_init_resets_and_configures_the_chip(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.chip.pump();

    TEST_ASSERT_TRUE(f.uart.isOpen());
    TEST_ASSERT_EQUAL_UINT32(19200u, f.uart.config()->baud);
    TEST_ASSERT_EQUAL_INT(KNX_UART_HAL_PARITY_EVEN, f.uart.config()->parity);
    // U_Reset.req, U_MxRstCnt (3 BUSY, 3 NACK), U_State.req; U_SetAddress
    // from the first service().
    const std::vector<uint8_t> expected{0x01, 0x24, 0x63, 0x02, 0x28, 0x11, 0x0A};
    TEST_ASSERT_EQUAL_UINT32(expected.size(), f.chip.hostOctets.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), f.chip.hostOctets.data(), expected.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1MediumState::Idle), static_cast<int>(f.backend->getState()));

    const auto caps = f.backend->getCapabilities();
    TEST_ASSERT_TRUE(caps.transceiverRepeats);
    TEST_ASSERT_TRUE(caps.supportsDetailedTxConfirm);
    TEST_ASSERT_TRUE(caps.supportsHardwareAutoAck);
}

void test_address_restored_after_init_is_the_only_one_the_chip_hears(void)
{
    // The BAU initialises the medium with its default address, restores the
    // real one from persistence and only then runs its loop.
    Fixture f;
    TEST_ASSERT_TRUE(f.initOnly(0xFFFFu).isOk());
    f.chip.pump();
    TEST_ASSERT_FALSE(f.chip.address.has_value());
    TEST_ASSERT_TRUE(f.backend->setOwnAddress(OWN_ADDRESS).isOk());
    f.serviceRound();

    size_t setAddressCount = 0;
    for (size_t i = 0; i + 2 < f.chip.hostOctets.size(); ++i) {
        if (f.chip.hostOctets[i] == 0x28u) {
            ++setAddressCount;
            TEST_ASSERT_EQUAL_HEX8(OWN_ADDRESS >> 8, f.chip.hostOctets[i + 1]);
            TEST_ASSERT_EQUAL_HEX8(OWN_ADDRESS & 0xFFu, f.chip.hostOctets[i + 2]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1u, setAddressCount);
    TEST_ASSERT_EQUAL_HEX16(OWN_ADDRESS, *f.chip.address);
}

void test_init_fails_with_timeout_when_the_chip_never_answers(void)
{
    Fixture f;
    f.chip.responsive = false;
    const auto result = f.init();
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::Timeout), static_cast<int>(result.error()));
    TEST_ASSERT_EQUAL_INT(3, f.chip.resets);
    TEST_ASSERT_FALSE(f.uart.isOpen());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Down), static_cast<int>(f.backend->getLinkState()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::NotInitialized),
                          static_cast<int>(f.backend->sendFrame(encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1)))
                                               .error()));
}

void test_init_pulses_the_reset_pin_when_the_board_wires_it(void)
{
    Fixture f(UartHalVirtual::Pins{true, false});
    TEST_ASSERT_TRUE(f.init().isOk());
    TEST_ASSERT_FALSE(f.uart.resetAsserted());  // released again
}

void test_init_refuses_an_incomplete_hal(void)
{
    UartHalVirtual uart;
    ScriptedChip chip(uart);
    g_uart = &uart;
    g_chip = &chip;
    knx::platform::CooperativePlatform platform(makeHooks());
    knx_uart_hal_t hal{};
    TpUartMediumBackend backend(hal, platform);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::InvalidParameter),
                          static_cast<int>(backend.init(Tp1MediumConfig{}).error()));
}

// --- Transmit ------------------------------------------------------------------

void test_frame_is_fed_as_u_l_data_pairs_a_few_octets_at_a_time(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.chip.pump();
    f.chip.hostOctets.clear();
    f.chip.confirm = ScriptedChip::Confirm::None;

    const auto frame = encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 0x01));
    TEST_ASSERT_EQUAL_UINT32(frame.size(), f.backend->sendFrame(frame).value());
    // The default limit keeps at most 4 octets (two pairs) in the UART.
    TEST_ASSERT_EQUAL_UINT32(4u, f.uart.txQueued());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1MediumState::Transmitting), static_cast<int>(f.backend->getState()));

    for (int i = 0; i < 20 && f.chip.frames.empty(); ++i) {
        f.chip.pump(2);  // one pair leaves the wire
        TEST_ASSERT_TRUE(f.backend->service().isOk());
        TEST_ASSERT_TRUE(f.uart.txQueued() <= 4u);
    }
    TEST_ASSERT_EQUAL_UINT32(1u, f.chip.frames.size());
    TEST_ASSERT_EQUAL_UINT32(frame.size(), f.chip.frames[0].size());
    TEST_ASSERT_EQUAL_MEMORY(frame.data(), f.chip.frames[0].data(), frame.size());

    // The control octets: Start (80h), Continue (80h | index), End (40h | last index).
    TEST_ASSERT_EQUAL_HEX8(0x80u, f.chip.hostOctets[0]);
    TEST_ASSERT_EQUAL_HEX8(0x81u, f.chip.hostOctets[2]);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(0x40u | (frame.size() - 1u)),
                           f.chip.hostOctets[2u * (frame.size() - 1u)]);
}

// LDataFrame defaults to ackRequested, which the codec writes into control bit
// 1 for the bit-bang MAC. The TP-UART2 answers a control field with bits 1..0
// set with a protocol error and a reset, so the backend clears them, and keeps
// the checksum right.
void test_control_bits_1_0_are_cleared_before_the_chip_sees_the_frame(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.chip.pump();
    f.chip.confirm = ScriptedChip::Confirm::None;

    LDataFrame frame = individualRead(OWN_ADDRESS, PEER_ADDRESS);
    frame.priority = Priority::System;
    frame.ackRequested = true;
    const auto encoded = encode(frame);
    TEST_ASSERT_EQUAL_HEX8(0xB2u, encoded[0]);  // what the codec produces

    TEST_ASSERT_TRUE(f.backend->sendFrame(encoded).isOk());
    for (int i = 0; i < 20 && f.chip.frames.empty(); ++i) {
        f.chip.pump(2);
        TEST_ASSERT_TRUE(f.backend->service().isOk());
    }
    TEST_ASSERT_EQUAL_UINT32(1u, f.chip.frames.size());
    const auto& wire = f.chip.frames[0];
    TEST_ASSERT_EQUAL_HEX8(0xB0u, wire[0]);
    TEST_ASSERT_TRUE(datalink::FrameCodec::verifyChecksum(std::span<const uint8_t>(wire)).isOk());
    TEST_ASSERT_EQUAL_MEMORY(encoded.data() + 1, wire.data() + 1, encoded.size() - 2u);
}

void test_positive_and_negative_confirmations_become_tx_outcomes(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    const auto frame = encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 0x01));

    TEST_ASSERT_TRUE(f.backend->sendFrame(frame).isOk());
    for (int i = 0; i < 30 && f.countEvents(Tp1RxEventType::TxAckResponse) == 0u; ++i) {
        f.serviceRound();
    }
    auto outcome = f.lastEvent(Tp1RxEventType::TxAckResponse);
    TEST_ASSERT_TRUE(outcome.has_value());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1AckClass::Ack), static_cast<int>(outcome->ackClass));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1MediumState::Idle), static_cast<int>(f.backend->getState()));

    f.chip.confirm = ScriptedChip::Confirm::Negative;
    TEST_ASSERT_TRUE(f.backend->sendFrame(frame).isOk());
    for (int i = 0; i < 30 && f.countEvents(Tp1RxEventType::TxAckResponse) < 2u; ++i) {
        f.serviceRound();
    }
    outcome = f.lastEvent(Tp1RxEventType::TxAckResponse);
    TEST_ASSERT_EQUAL_UINT32(2u, f.countEvents(Tp1RxEventType::TxAckResponse));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1AckClass::None), static_cast<int>(outcome->ackClass));
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().confirmsPositive);
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().confirmsNegative);
}

void test_send_is_refused_while_a_frame_is_outstanding_or_malformed(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.chip.confirm = ScriptedChip::Confirm::None;
    const auto frame = encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 0x01));
    TEST_ASSERT_TRUE(f.backend->sendFrame(frame).isOk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::Busy), static_cast<int>(f.backend->sendFrame(frame).error()));

    Fixture g;
    TEST_ASSERT_TRUE(g.init().isOk());
    const std::vector<uint8_t> tooShort(7u, 0xBCu);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::InvalidFrameSize),
                          static_cast<int>(g.backend->sendFrame(tooShort).error()));
    const std::vector<uint8_t> tooLong(TpUartMediumBackend::MAX_FRAME + 1u, 0xBCu);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::InvalidFrameSize),
                          static_cast<int>(g.backend->sendFrame(tooLong).error()));
}

void test_missing_confirmation_is_a_deadline_miss_and_resets_the_chip(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.chip.confirm = ScriptedChip::Confirm::None;
    TEST_ASSERT_TRUE(f.backend->sendFrame(encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 0x01))).isOk());

    for (int i = 0; i < 40; ++i) {
        f.serviceRound();
    }
    TEST_ASSERT_EQUAL_UINT32(0u, f.countEvents(Tp1RxEventType::TxAckDeadlineMiss));
    const int resetsBefore = f.chip.resets;

    advanceMs(1000u);
    f.serviceRound();
    TEST_ASSERT_EQUAL_UINT32(1u, f.countEvents(Tp1RxEventType::TxAckDeadlineMiss));
    TEST_ASSERT_EQUAL_INT(resetsBefore + 1, f.chip.resets);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1MediumState::Idle), static_cast<int>(f.backend->getState()));
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().confirmTimeouts);
}

void test_confirmation_without_a_frame_is_counted_and_ignored(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.deliver({0x8Bu});
    f.serviceRound();
    TEST_ASSERT_EQUAL_UINT32(0u, f.countEvents(Tp1RxEventType::TxAckResponse));
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().unexpectedConfirms);
}

void test_own_frame_echoed_by_the_chip_is_neither_acknowledged_nor_delivered(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({GROUP_IN_TABLE});
    TEST_ASSERT_TRUE(f.backend->sendFrame(encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1))).isOk());
    for (int i = 0; i < 30 && f.countEvents(Tp1RxEventType::TxAckResponse) == 0u; ++i) {
        f.serviceRound();
    }
    TEST_ASSERT_EQUAL_UINT32(1u, f.countEvents(Tp1RxEventType::TxAckResponse));
    TEST_ASSERT_EQUAL_UINT32(0u, f.chip.acks.size());  // we did not acknowledge our own frame
    TEST_ASSERT_EQUAL_UINT32(0u, f.telegrams.size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().echoesDropped);
}

void test_frame_from_own_address_with_nothing_outstanding_is_not_an_echo(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({GROUP_IN_TABLE});
    // Another device with our address (a misconfiguration): received like any frame.
    f.deliver(encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1)));
    f.serviceRound();
    TEST_ASSERT_EQUAL_UINT32(1u, f.telegrams.size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.chip.acks.size());
    TEST_ASSERT_EQUAL_UINT32(0u, f.backend->statistics().echoesDropped);
}

// --- Receive -------------------------------------------------------------------

void test_received_frame_is_reported_whole(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    const auto frame = encode(groupWrite(PEER_ADDRESS, GROUP_NOT_IN_TABLE, 0x01));
    f.deliver(frame);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1MediumState::Idle), static_cast<int>(f.backend->getState()));
    f.serviceRound();

    TEST_ASSERT_EQUAL_UINT32(1u, f.telegrams.size());
    TEST_ASSERT_EQUAL_UINT32(frame.size(), f.telegrams[0].size());
    TEST_ASSERT_EQUAL_MEMORY(frame.data(), f.telegrams[0].data(), frame.size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().framesReceived);
}

void test_frame_with_bad_checksum_or_octet_error_is_dropped(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    auto frame = encode(groupWrite(PEER_ADDRESS, GROUP_NOT_IN_TABLE, 0x01));
    auto corrupted = frame;
    corrupted.back() = static_cast<uint8_t>(corrupted.back() ^ 0x01u);
    f.deliver(corrupted);
    f.deliver(frame, OCTET_US, 7u, KNX_UART_HAL_RX_PARITY_ERROR);
    f.deliver(frame);
    f.serviceRound();

    TEST_ASSERT_EQUAL_UINT32(1u, f.telegrams.size());
    const auto stats = f.backend->statistics();
    TEST_ASSERT_EQUAL_UINT32(1u, stats.checksumErrors);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.octetErrorFrames);
}

void test_gap_of_more_than_2ms_discards_a_partial_frame(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    const auto frame = encode(groupWrite(PEER_ADDRESS, GROUP_NOT_IN_TABLE, 0x01));
    f.deliver(std::vector<uint8_t>(frame.begin(), frame.begin() + 4));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1MediumState::Receiving), static_cast<int>(f.backend->getState()));
    f.uart.advanceUs(2500u);
    f.deliver(frame);
    f.serviceRound();

    TEST_ASSERT_EQUAL_UINT32(1u, f.telegrams.size());
    TEST_ASSERT_EQUAL_MEMORY(frame.data(), f.telegrams[0].data(), frame.size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().partialFramesDiscarded);
}

void test_extended_frame_is_received(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    // CTRL 3Ch (extended, low priority), CTRLE E0h (group, hop count 6),
    // source, destination, length 1, TPCI/APCI GroupValue_Write, data, FCS.
    std::vector<uint8_t> frame{0x3C, 0xE0, 0x11, 0x0B, 0x08, 0x01, 0x01, 0x00, 0x81};
    uint8_t fcs = 0xFFu;
    for (uint8_t b : frame) {
        fcs = static_cast<uint8_t>(fcs ^ b);
    }
    frame.push_back(fcs);
    f.ackGroups({GROUP_IN_TABLE});
    f.deliver(frame);
    f.serviceRound();

    TEST_ASSERT_EQUAL_UINT32(1u, f.telegrams.size());
    TEST_ASSERT_EQUAL_MEMORY(frame.data(), f.telegrams[0].data(), frame.size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.chip.acks.size());
    TEST_ASSERT_EQUAL_HEX8(ACK_ADDRESSED, f.chip.acks[0]);
}

void test_frame_longer_than_a_slot_is_neither_acknowledged_nor_delivered(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({GROUP_IN_TABLE});
    const uint8_t length = static_cast<uint8_t>(TpUartMediumBackend::MAX_FRAME);  // length + 9 > MAX_FRAME
    std::vector<uint8_t> frame{0x3C, 0xE0, 0x11, 0x0B, 0x08, 0x01, length};
    frame.resize(static_cast<size_t>(length) + 8u, 0x00u);
    uint8_t fcs = 0xFFu;
    for (uint8_t b : frame) {
        fcs = static_cast<uint8_t>(fcs ^ b);
    }
    frame.push_back(fcs);
    f.deliver(frame);
    f.serviceRound();

    TEST_ASSERT_EQUAL_UINT32(0u, f.telegrams.size());
    TEST_ASSERT_EQUAL_UINT32(0u, f.chip.acks.size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().oversizedFrames);
}

// --- Acknowledge decision ------------------------------------------------------

void test_group_frames_are_acknowledged_from_the_rx_interrupt_when_in_the_table(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    // Unsorted on purpose: the backend sorts for its binary search.
    f.ackGroups({0x0A01, GROUP_IN_TABLE, 0x0805, 0x0001}, false);

    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1)));
    TEST_ASSERT_EQUAL_UINT32(1u, f.uart.writesFromRxCallback());  // before service() ran
    // service() between frames, so the ring never fills (that answers BUSY).
    for (const uint16_t destination : {uint16_t{0x0A01}, uint16_t{0x0001}, GROUP_NOT_IN_TABLE, uint16_t{0x0000}}) {
        f.serviceRound();
        f.deliver(encode(groupWrite(PEER_ADDRESS, destination, 1)));  // 0000h: broadcast
    }
    f.chip.pump();

    const std::vector<uint8_t> expected(4u, ACK_ADDRESSED);
    TEST_ASSERT_EQUAL_UINT32(expected.size(), f.chip.acks.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), f.chip.acks.data(), expected.size());
    TEST_ASSERT_EQUAL_UINT32(4u, f.backend->statistics().acksSent);
}

void test_acknowledge_follows_a_rewritten_table(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({GROUP_IN_TABLE});
    f.groups.set({GROUP_NOT_IN_TABLE});  // changed in place, not handed over again
    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1)));
    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_NOT_IN_TABLE, 1)));
    f.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(1u, f.chip.acks.size());
    TEST_ASSERT_EQUAL_UINT32(0u, f.backend->statistics().ackDecisionsDeferred);
}

void test_sorted_list_is_binary_searched(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({0x0001, 0x0805, GROUP_IN_TABLE + 0x100u, 0x0A01, 0x0B01, 0x0C01});
    for (const uint16_t destination : {uint16_t{0x0001}, uint16_t{0x0C01}, uint16_t{0x0A01}, uint16_t{0x0A02}}) {
        f.serviceRound();
        f.deliver(encode(groupWrite(PEER_ADDRESS, destination, 1)));
    }
    f.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(3u, f.chip.acks.size());  // 0A02h is not in the list
}

void test_no_acknowledge_while_the_list_is_being_changed(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({GROUP_IN_TABLE});

    f.groups.state.beginChange();  // the owner is in the middle of a change
    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1)));
    f.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(0u, f.chip.acks.size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().ackDecisionsDeferred);

    f.groups.state.endChange(1u);
    f.serviceRound();
    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1)));  // the repetition
    f.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(1u, f.chip.acks.size());
}

void test_acknowledge_reads_the_address_table_itself(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    objects::AddressTableObject table;
    const std::array<GroupAddress, 2> loaded{GroupAddress(uint16_t{0x0801}), GroupAddress(uint16_t{0x0805})};
    TEST_ASSERT_TRUE(table.loadTable(loaded).isOk());
    TEST_ASSERT_TRUE(f.backend->setAckGroupAddressView(table.sharedGroupAddressView()));

    f.deliver(encode(groupWrite(PEER_ADDRESS, 0x0805, 1)));
    f.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(1u, f.chip.acks.size());

    // An unload empties the table; no new view is handed over.
    table.clearEntries();
    f.serviceRound();
    f.deliver(encode(groupWrite(PEER_ADDRESS, 0x0805, 1)));
    f.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(1u, f.chip.acks.size());
}

void test_copy_path_is_refused_and_clears_the_view(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({GROUP_IN_TABLE});
    const std::array<uint16_t, 1> copy{GROUP_IN_TABLE};
    f.backend->setAckGroupAddresses(copy);
    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1)));
    f.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(0u, f.chip.acks.size());
}

void test_individual_frames_leave_the_own_address_to_the_chip(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.deliver(encode(individualRead(PEER_ADDRESS, OWN_ADDRESS)));
    f.deliver(encode(individualRead(PEER_ADDRESS, 0x1199)));
    f.deliver(encode(individualRead(PEER_ADDRESS, 0xFFFF)));
    f.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(0u, f.chip.acks.size());
    f.serviceRound();
    TEST_ASSERT_EQUAL_UINT32(3u, f.telegrams.size());
}

void test_local_busy_answers_addressed_frames_with_busy(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({GROUP_IN_TABLE});
    f.backend->setLocalBusy(true);
    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1)));
    f.deliver(encode(individualRead(PEER_ADDRESS, OWN_ADDRESS)));
    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_NOT_IN_TABLE, 1)));
    f.chip.pump();

    const std::vector<uint8_t> expected{ACK_BUSY, ACK_BUSY};
    TEST_ASSERT_EQUAL_UINT32(expected.size(), f.chip.acks.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), f.chip.acks.data(), expected.size());
}

void test_full_ring_answers_busy_and_drops_nothing_it_acknowledged(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({GROUP_IN_TABLE});
    // RX_SLOT_COUNT - 1 frames fill the ring while service() does not run.
    for (size_t i = 0; i < TpUartMediumBackend::RX_SLOT_COUNT - 1u; ++i) {
        f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, static_cast<uint8_t>(i))));
    }
    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 9)));
    f.chip.pump();

    TEST_ASSERT_EQUAL_UINT32(TpUartMediumBackend::RX_SLOT_COUNT, f.chip.acks.size());
    TEST_ASSERT_EQUAL_HEX8(ACK_BUSY, f.chip.acks.back());
    f.serviceRound();
    TEST_ASSERT_EQUAL_UINT32(TpUartMediumBackend::RX_SLOT_COUNT - 1u, f.telegrams.size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().rxOverflowDrops);  // the one refused with BUSY
}

// --- Flash erase guard (busy mode) -------------------------------------------

void test_erase_guard_sends_busy_mode_before_the_erase(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.serviceRound();  // the start-up U_State.req is answered: nothing outstanding
    TEST_ASSERT_TRUE(f.backend->beginFlashErase(false));
    // Out on the UART before begin returns: the erase may mask its interrupts.
    TEST_ASSERT_EQUAL_INT(1, f.chip.busyModeOn);
    TEST_ASSERT_EQUAL_HEX8(0x21u, f.chip.hostOctets.back());
    TEST_ASSERT_EQUAL_INT(0, f.chip.busyModeOff);
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().erasesGuarded);
}

void test_octets_after_an_erase_are_dropped_until_the_link_is_silent(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.serviceRound();  // the start-up U_State.req is answered: nothing outstanding
    f.ackGroups({GROUP_IN_TABLE});
    const auto frame = encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 0x01));
    const auto before = f.backend->statistics();
    TEST_ASSERT_TRUE(f.backend->beginFlashErase(false));
    // The interrupt ran late: the head of a frame arrives during the erase,
    // its tail after it, without the octets in between and with no gap that
    // shows where the frame was cut. Some tail octets look like indications.
    f.deliver(std::vector<uint8_t>(frame.begin(), frame.begin() + 3));
    f.backend->endFlashErase();
    f.deliver({0x8Bu, 0x03u, 0x07u, frame[7], frame[8]});
    f.serviceRound();

    TEST_ASSERT_EQUAL_UINT32(0u, f.chip.acks.size());
    const auto stats = f.backend->statistics();
    TEST_ASSERT_EQUAL_UINT32(8u, stats.resyncDiscards);
    TEST_ASSERT_EQUAL_UINT32(before.unexpectedConfirms, stats.unexpectedConfirms);
    TEST_ASSERT_EQUAL_UINT32(before.chipResets, stats.chipResets);
    TEST_ASSERT_EQUAL_UINT32(before.stateIndications, stats.stateIndications);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.framesReceived);
    TEST_ASSERT_EQUAL_INT(0, f.chip.busyModeOff);

    // After 2 ms of silence a whole frame is taken and acknowledged again.
    f.uart.advanceUs(2500u);
    f.deliver(frame);
    f.serviceRound();
    TEST_ASSERT_EQUAL_UINT32(1u, f.telegrams.size());
    TEST_ASSERT_EQUAL_MEMORY(frame.data(), f.telegrams[0].data(), frame.size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.chip.acks.size());
    TEST_ASSERT_EQUAL_HEX8(ACK_ADDRESSED, f.chip.acks[0]);
    // In step again: busy mode ends.
    TEST_ASSERT_EQUAL_INT(1, f.chip.busyModeOff);
}

void test_busy_mode_ends_once_the_link_has_been_silent(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.serviceRound();  // the start-up U_State.req is answered: nothing outstanding
    TEST_ASSERT_TRUE(f.backend->beginFlashErase(false));
    f.deliver({0x55u, 0x66u});
    f.backend->endFlashErase();
    f.deliver({0x77u});
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(0, f.chip.busyModeOff);  // still receiving: not in step yet

    advanceMs(TpUartMediumBackend::RESYNC_SILENCE_MS);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(1, f.chip.busyModeOff);
    TEST_ASSERT_EQUAL_HEX8(0x22u, f.chip.hostOctets.back());
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(1, f.chip.busyModeOff);  // once
}

void test_send_waits_until_busy_mode_has_ended(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.serviceRound();  // the start-up U_State.req is answered: nothing outstanding
    const auto frame = encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 0x01));
    TEST_ASSERT_TRUE(f.backend->beginFlashErase(false));
    f.backend->endFlashErase();
    TEST_ASSERT_TRUE(f.backend->sendFrame(frame).error() == util::ErrorCode::Busy);

    advanceMs(TpUartMediumBackend::RESYNC_SILENCE_MS);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(1, f.chip.busyModeOff);
    TEST_ASSERT_TRUE(f.backend->sendFrame(frame).isOk());
}

void test_erase_guard_declines_while_our_frame_or_a_received_one_is_in_flight(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    advanceMs(60u);
    f.serviceRound();  // the start-up U_State.req is answered
    f.chip.confirm = ScriptedChip::Confirm::None;
    TEST_ASSERT_TRUE(f.backend->sendFrame(encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 0x01))).isOk());
    f.serviceRound();
    TEST_ASSERT_FALSE(f.backend->beginFlashErase(false));
    TEST_ASSERT_EQUAL_INT(0, f.chip.busyModeOn);
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().erasesDeclined);

    // An erase that cannot wait goes ahead anyway.
    TEST_ASSERT_TRUE(f.backend->beginFlashErase(true));
    TEST_ASSERT_EQUAL_INT(1, f.chip.busyModeOn);
    f.backend->endFlashErase();

    Fixture g;
    TEST_ASSERT_TRUE(g.init().isOk());
    advanceMs(60u);
    g.serviceRound();
    const auto frame = encode(groupWrite(PEER_ADDRESS, GROUP_NOT_IN_TABLE, 0x01));
    g.deliver(std::vector<uint8_t>(frame.begin(), frame.begin() + 4));  // a frame is coming in
    TEST_ASSERT_FALSE(g.backend->beginFlashErase(false));
}

void test_erase_guard_before_init_has_nothing_to_do(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.backend->beginFlashErase(false));
    f.backend->endFlashErase();
    TEST_ASSERT_EQUAL_INT(0, f.chip.busyModeOn);
}

void test_bus_monitor_mode_acknowledges_nothing_and_leaves_through_a_reset(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.ackGroups({GROUP_IN_TABLE});
    TEST_ASSERT_TRUE(f.backend->setBusMonitorMode(true).isOk());
    f.chip.pump();
    TEST_ASSERT_TRUE(f.chip.busmon);
    f.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1)));
    f.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(0u, f.chip.acks.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::OperationNotSupported),
                          static_cast<int>(f.backend->sendFrame(encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1))).error()));

    const int resetsBefore = f.chip.resets;
    TEST_ASSERT_TRUE(f.backend->setBusMonitorMode(false).isOk());
    TEST_ASSERT_EQUAL_INT(resetsBefore + 1, f.chip.resets);
    TEST_ASSERT_FALSE(f.chip.busmon);
}

// --- Chip state and link health ------------------------------------------------

void test_state_indication_reports_the_link_up_then_degraded(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.serviceRound();
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), static_cast<int>(f.backend->getLinkState()));
    auto event = f.lastEvent(Tp1RxEventType::LinkStateChanged);
    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), static_cast<int>(event->linkStatus.state));

    f.chip.stateValue = 0x0Fu;  // temperature warning
    advanceMs(1000u);
    f.serviceRound();
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Degraded), static_cast<int>(f.backend->getLinkState()));
    TEST_ASSERT_EQUAL_HEX8(0x0Fu, f.backend->statistics().lastStateBits);
}

void test_two_unanswered_state_requests_report_the_link_down(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.serviceRound();
    f.serviceRound();
    f.chip.responsive = false;
    for (int i = 0; i < 3; ++i) {
        advanceMs(1000u);
        f.serviceRound();
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Down), static_cast<int>(f.backend->getLinkState()));
    TEST_ASSERT_TRUE(f.backend->statistics().missedStatePolls >= 2u);

    f.chip.responsive = true;
    advanceMs(1000u);
    f.serviceRound();
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), static_cast<int>(f.backend->getLinkState()));
}

// SAVE is debounced like the bitbang driver's KNX_OK: the same LinkMonitor,
// the same default windows (LinkSignalPolicy).
constexpr uint32_t SAVE_DOWN_MS = LinkSignalPolicy{}.downDebounceUs / 1000u;
constexpr uint32_t SAVE_UP_MS = LinkSignalPolicy{}.upDebounceUs / 1000u;

int linkState(const Fixture& f)
{
    return static_cast<int>(f.backend->getLinkState());
}

void bringLinkUp(Fixture& f)
{
    TEST_ASSERT_TRUE(f.init().isOk());
    f.serviceRound();
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), linkState(f));
}

void test_save_reports_the_link_down_after_the_down_window(void)
{
    Fixture f(UartHalVirtual::Pins{false, true});
    bringLinkUp(f);
    f.uart.setSave(true);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), linkState(f));
    advanceMs(SAVE_DOWN_MS - 1u);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), linkState(f));
    advanceMs(1u);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Down), linkState(f));
    auto event = f.lastEvent(Tp1RxEventType::LinkStateChanged);
    TEST_ASSERT_TRUE(event.has_value());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Down), static_cast<int>(event->linkStatus.state));
}

void test_save_dip_shorter_than_the_down_window_is_filtered(void)
{
    Fixture f(UartHalVirtual::Pins{false, true});
    bringLinkUp(f);
    const size_t eventsBefore = f.events.size();
    f.uart.setSave(true);
    f.serviceRound();
    advanceMs(SAVE_DOWN_MS / 2u);
    f.serviceRound();
    f.uart.setSave(false);
    f.serviceRound();
    advanceMs(SAVE_DOWN_MS * 2u);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), linkState(f));
    for (size_t i = eventsBefore; i < f.events.size(); ++i) {
        TEST_ASSERT_TRUE(f.events[i].type != Tp1RxEventType::LinkStateChanged);
    }
}

void test_save_release_reports_the_link_up_after_the_longer_up_window(void)
{
    Fixture f(UartHalVirtual::Pins{false, true});
    bringLinkUp(f);
    f.uart.setSave(true);
    f.serviceRound();
    advanceMs(SAVE_DOWN_MS);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Down), linkState(f));

    f.uart.setSave(false);
    f.serviceRound();
    advanceMs(SAVE_UP_MS - 1u);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Down), linkState(f));
    advanceMs(1u);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), linkState(f));
}

void test_save_windows_come_from_the_configuration(void)
{
    TpUartConfig config;
    config.saveSignal.downDebounceUs = 2000u;
    Fixture f(UartHalVirtual::Pins{false, true}, config);
    bringLinkUp(f);
    f.uart.setSave(true);
    f.serviceRound();
    advanceMs(2u);
    f.serviceRound();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Down), linkState(f));
}

void test_save_asserted_at_init_reports_the_link_down_at_once(void)
{
    // The first sample is adopted without a window, as for KNX_OK: at init
    // there is no earlier state to debounce against.
    Fixture f(UartHalVirtual::Pins{false, true});
    f.uart.setSave(true);
    TEST_ASSERT_TRUE(f.init().isOk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Down), linkState(f));
}

struct PowerFailLog {
    int calls{0};
    uint64_t timestampUs{0};
    void* context{nullptr};
};

void recordPowerFail(void* context, uint64_t timestampUs)
{
    auto* log = static_cast<PowerFailLog*>(context);
    ++log->calls;
    log->timestampUs = timestampUs;
    log->context = context;
}

void test_save_edge_warns_of_power_failure_ahead_of_the_debounce(void)
{
    TpUartConfig config;
    config.saveSignal.notifyPowerFailFromIsr = true;
    Fixture f(UartHalVirtual::Pins{false, true, true}, config);
    PowerFailLog log;
    f.backend->setPowerFailIsrHandler(&recordPowerFail, &log);  // before init(): kept
    bringLinkUp(f);
    TEST_ASSERT_TRUE(f.uart.saveEdgeInstalled());

    advanceMs(5u);
    f.uart.setSave(true);
    TEST_ASSERT_EQUAL_INT(1, log.calls);
    TEST_ASSERT_TRUE(log.context == &log);
    TEST_ASSERT_EQUAL_UINT64(f.uart.nowUs(), log.timestampUs);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), linkState(f));  // not debounced yet
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().powerFailWarnings);

    f.uart.setSave(false);  // the edge back is no warning
    TEST_ASSERT_EQUAL_INT(1, log.calls);

    f.backend->close();
    TEST_ASSERT_FALSE(f.uart.saveEdgeInstalled());
}

void test_save_edge_is_not_used_unless_the_warning_is_enabled(void)
{
    Fixture f(UartHalVirtual::Pins{false, true, true});
    PowerFailLog log;
    f.backend->setPowerFailIsrHandler(&recordPowerFail, &log);
    bringLinkUp(f);
    TEST_ASSERT_FALSE(f.uart.saveEdgeInstalled());
    f.uart.setSave(true);
    TEST_ASSERT_EQUAL_INT(0, log.calls);
}

void test_power_fail_handler_registers_through_the_mac(void)
{
    // The application's hook is backend-neutral: Tp1MacPhysical forwards it.
    TpUartConfig config;
    config.saveSignal.notifyPowerFailFromIsr = true;
    Fixture f(UartHalVirtual::Pins{false, true, true}, config);
    Tp1MacPhysical mac(std::move(f.backend));
    mac.setTimingPlatform(&f.platform);
    PowerFailLog log;
    mac.setPowerFailIsrHandler(&recordPowerFail, &log);
    TEST_ASSERT_TRUE(mac.init().isOk());
    f.uart.setSave(true);
    TEST_ASSERT_EQUAL_INT(1, log.calls);
    mac.close();
}

void test_spontaneous_chip_reset_reconfigures_and_fails_the_outstanding_frame(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.chip.pump();
    f.chip.confirm = ScriptedChip::Confirm::None;
    TEST_ASSERT_TRUE(f.backend->sendFrame(encode(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1))).isOk());
    f.chip.address.reset();

    f.deliver({0x03u});  // U_Reset.ind nobody asked for
    f.serviceRound();
    TEST_ASSERT_EQUAL_UINT32(1u, f.countEvents(Tp1RxEventType::TxAckDeadlineMiss));
    TEST_ASSERT_EQUAL_UINT32(1u, f.backend->statistics().chipResets);
    TEST_ASSERT_TRUE(f.chip.address.has_value());
    TEST_ASSERT_EQUAL_HEX16(OWN_ADDRESS, *f.chip.address);
}

void test_new_own_address_is_sent_between_frames(void)
{
    Fixture f;
    TEST_ASSERT_TRUE(f.init().isOk());
    f.chip.pump();
    TEST_ASSERT_TRUE(f.backend->setOwnAddress(0x1203).isOk());
    f.serviceRound();
    TEST_ASSERT_EQUAL_HEX16(0x1203, *f.chip.address);
}

// --- Over the data link layer ----------------------------------------------------

struct StackFixture : Fixture {
    std::unique_ptr<Tp1MacPhysical> mac;
    std::unique_ptr<datalink::Tp1DataLinkLayer> dataLink;

    StackFixture()
    {
        auto owned = std::move(backend);
        mac = std::make_unique<Tp1MacPhysical>(std::move(owned));
        mac->setTimingPlatform(&platform);
        auto config = datalink::Tp1DataLinkConfig::defaults();
        config.enableRxTask = false;
        dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(platform, *mac, config);
        TEST_ASSERT_TRUE(dataLink->init(IndividualAddress(OWN_ADDRESS)).isOk());
    }
};

void test_data_link_layer_sends_once_and_takes_a_negative_group_confirmation_as_sent(void)
{
    StackFixture s;
    s.chip.confirm = ScriptedChip::Confirm::Negative;
    const auto result = s.dataLink->sendFrame(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1));
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, s.chip.frames.size());  // the chip repeats, not the stack
    TEST_ASSERT_EQUAL_UINT32(0u, s.dataLink->getStatistics().txRepetitions);
}

void test_data_link_layer_fails_an_individual_frame_on_a_negative_confirmation_without_repeating(void)
{
    StackFixture s;
    s.chip.confirm = ScriptedChip::Confirm::Negative;
    const auto result = s.dataLink->sendFrame(individualRead(OWN_ADDRESS, PEER_ADDRESS));
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::TransmissionFailed), static_cast<int>(result.error()));
    TEST_ASSERT_EQUAL_UINT32(1u, s.chip.frames.size());
}

void test_data_link_layer_succeeds_on_a_positive_confirmation(void)
{
    StackFixture s;
    const auto result = s.dataLink->sendFrame(individualRead(OWN_ADDRESS, PEER_ADDRESS));
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, s.chip.frames.size());
    TEST_ASSERT_EQUAL_UINT32(1u, s.dataLink->getStatistics().txFrames);
}

void test_data_link_layer_publishes_its_group_addresses_to_the_acknowledge_table(void)
{
    StackFixture s;
    TEST_ASSERT_TRUE(s.dataLink->addGroupAddress(GroupAddress(GROUP_IN_TABLE)).isOk());
    s.deliver(encode(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1)));
    s.chip.pump();
    TEST_ASSERT_EQUAL_UINT32(1u, s.chip.acks.size());
    TEST_ASSERT_EQUAL_HEX8(ACK_ADDRESSED, s.chip.acks[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_resets_and_configures_the_chip);
    RUN_TEST(test_address_restored_after_init_is_the_only_one_the_chip_hears);
    RUN_TEST(test_init_fails_with_timeout_when_the_chip_never_answers);
    RUN_TEST(test_init_pulses_the_reset_pin_when_the_board_wires_it);
    RUN_TEST(test_init_refuses_an_incomplete_hal);
    RUN_TEST(test_frame_is_fed_as_u_l_data_pairs_a_few_octets_at_a_time);
    RUN_TEST(test_control_bits_1_0_are_cleared_before_the_chip_sees_the_frame);
    RUN_TEST(test_positive_and_negative_confirmations_become_tx_outcomes);
    RUN_TEST(test_send_is_refused_while_a_frame_is_outstanding_or_malformed);
    RUN_TEST(test_missing_confirmation_is_a_deadline_miss_and_resets_the_chip);
    RUN_TEST(test_confirmation_without_a_frame_is_counted_and_ignored);
    RUN_TEST(test_own_frame_echoed_by_the_chip_is_neither_acknowledged_nor_delivered);
    RUN_TEST(test_frame_from_own_address_with_nothing_outstanding_is_not_an_echo);
    RUN_TEST(test_received_frame_is_reported_whole);
    RUN_TEST(test_frame_with_bad_checksum_or_octet_error_is_dropped);
    RUN_TEST(test_gap_of_more_than_2ms_discards_a_partial_frame);
    RUN_TEST(test_extended_frame_is_received);
    RUN_TEST(test_frame_longer_than_a_slot_is_neither_acknowledged_nor_delivered);
    RUN_TEST(test_group_frames_are_acknowledged_from_the_rx_interrupt_when_in_the_table);
    RUN_TEST(test_acknowledge_follows_a_rewritten_table);
    RUN_TEST(test_sorted_list_is_binary_searched);
    RUN_TEST(test_no_acknowledge_while_the_list_is_being_changed);
    RUN_TEST(test_acknowledge_reads_the_address_table_itself);
    RUN_TEST(test_copy_path_is_refused_and_clears_the_view);
    RUN_TEST(test_individual_frames_leave_the_own_address_to_the_chip);
    RUN_TEST(test_local_busy_answers_addressed_frames_with_busy);
    RUN_TEST(test_full_ring_answers_busy_and_drops_nothing_it_acknowledged);
    RUN_TEST(test_erase_guard_sends_busy_mode_before_the_erase);
    RUN_TEST(test_octets_after_an_erase_are_dropped_until_the_link_is_silent);
    RUN_TEST(test_busy_mode_ends_once_the_link_has_been_silent);
    RUN_TEST(test_send_waits_until_busy_mode_has_ended);
    RUN_TEST(test_erase_guard_declines_while_our_frame_or_a_received_one_is_in_flight);
    RUN_TEST(test_erase_guard_before_init_has_nothing_to_do);
    RUN_TEST(test_bus_monitor_mode_acknowledges_nothing_and_leaves_through_a_reset);
    RUN_TEST(test_state_indication_reports_the_link_up_then_degraded);
    RUN_TEST(test_two_unanswered_state_requests_report_the_link_down);
    RUN_TEST(test_save_reports_the_link_down_after_the_down_window);
    RUN_TEST(test_save_dip_shorter_than_the_down_window_is_filtered);
    RUN_TEST(test_save_release_reports_the_link_up_after_the_longer_up_window);
    RUN_TEST(test_save_windows_come_from_the_configuration);
    RUN_TEST(test_save_asserted_at_init_reports_the_link_down_at_once);
    RUN_TEST(test_save_edge_warns_of_power_failure_ahead_of_the_debounce);
    RUN_TEST(test_save_edge_is_not_used_unless_the_warning_is_enabled);
    RUN_TEST(test_power_fail_handler_registers_through_the_mac);
    RUN_TEST(test_spontaneous_chip_reset_reconfigures_and_fails_the_outstanding_frame);
    RUN_TEST(test_new_own_address_is_sent_between_frames);
    RUN_TEST(test_data_link_layer_sends_once_and_takes_a_negative_group_confirmation_as_sent);
    RUN_TEST(test_data_link_layer_fails_an_individual_frame_on_a_negative_confirmation_without_repeating);
    RUN_TEST(test_data_link_layer_succeeds_on_a_positive_confirmation);
    RUN_TEST(test_data_link_layer_publishes_its_group_addresses_to_the_acknowledge_table);
    return UNITY_END();
}
