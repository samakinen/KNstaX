// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tpuart_chip_simulator.cpp
 * @brief TpUartMediumBackend over the simulated TP-UART2 on a virtual TP1 line (K4)
 *
 * The chip simulator (VirtualTpUartChip) answers the host protocol with the
 * timing of a 19200 baud link and puts real frames, acknowledges and
 * repetitions on a character-level TP1 line, where scripted partners send and
 * answer. One clock, the line's, drives everything: the CooperativePlatform's
 * idle hook advances it, and the main loop below services each device every
 * 100 us.
 *
 * Timings checked against the H8i8o's chip (design note
 * docs/design/cortex-m-tpuart2-support.md §3.8): U_Reset.ind 6.9 ms and
 * U_State.ind 1.4 ms after the request, repetitions 17.3 ms apart for a
 * 9-octet frame.
 */

#include "unity.h"

#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tpuart_medium_backend.hpp"
#include "knx/physical/uart_hal_virtual.hpp"
#include "knx/physical/virtual_tpuart_chip.hpp"
#include "knx/platform/cooperative_platform.hpp"
#include "knx/types.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

using namespace knx;
using namespace knx::physical;

// expected - delta <= actual <= expected + delta, in microseconds.
#define ASSERT_US_WITHIN(delta, expected, actual)                                    do {                                                                                 TEST_ASSERT_GREATER_OR_EQUAL((expected) - (delta), (uint64_t)(actual));         TEST_ASSERT_LESS_OR_EQUAL((expected) + (delta), (uint64_t)(actual));        } while (0)

namespace {

constexpr uint16_t OWN_ADDRESS = 0x110A;     // 1.1.10
constexpr uint16_t PEER_ADDRESS = 0x110B;    // 1.1.11
constexpr uint16_t SECOND_ADDRESS = 0x1114;  // 1.1.20
constexpr uint16_t ABSENT_ADDRESS = 0x11FA;  // 1.1.250
constexpr uint16_t GROUP_IN_TABLE = 0x0801;
constexpr uint16_t GROUP_NOT_IN_TABLE = 0x0901;
constexpr uint64_t STEP_US = 100u;
constexpr uint64_t MS = 1000u;

VirtualTp1CharacterBus* g_bus = nullptr;

knx::platform::CooperativeHooks makeHooks()
{
    knx::platform::CooperativeHooks hooks{};
    hooks.millis = [](void*) -> uint32_t { return static_cast<uint32_t>(g_bus->nowUs() / MS); };
    hooks.micros = [](void*) -> uint64_t { return g_bus->nowUs(); };
    hooks.idle = [](void*) { g_bus->runFor(STEP_US); };
    return hooks;
}

// --- Frames as they are on the line ---------------------------------------------

std::vector<uint8_t> frame(uint16_t source, uint16_t destination, bool group, std::vector<uint8_t> tpdu)
{
    std::vector<uint8_t> f{0xBCu,  // standard, not repeated, low priority
                           static_cast<uint8_t>(source >> 8), static_cast<uint8_t>(source & 0xFFu),
                           static_cast<uint8_t>(destination >> 8), static_cast<uint8_t>(destination & 0xFFu),
                           static_cast<uint8_t>((group ? 0x80u : 0x00u) | 0x60u | ((tpdu.size() - 1u) & 0x0Fu))};
    f.insert(f.end(), tpdu.begin(), tpdu.end());
    f.push_back(tp1_frame::checksum(f));
    return f;
}

std::vector<uint8_t> groupWrite(uint16_t source, uint16_t destination, uint8_t value)
{
    return frame(source, destination, true, {0x00u, static_cast<uint8_t>(0x80u | (value & 0x3Fu))});
}

/// A group write with 13 octets of data: 23 octets on the line, 46 on the host link.
std::vector<uint8_t> longGroupWrite(uint16_t source, uint16_t destination)
{
    std::vector<uint8_t> tpdu{0x00u, 0x80u};
    for (uint8_t i = 0; i < 13u; ++i) {
        tpdu.push_back(i);
    }
    return frame(source, destination, true, tpdu);
}

std::vector<uint8_t> deviceDescriptorRead(uint16_t source, uint16_t destination)
{
    return frame(source, destination, false, {0x03u, 0x00u});
}

/// A group address list kept the way the address table keeps it.
struct SharedList {
    static constexpr uint16_t CAPACITY = 16;
    std::array<GroupAddress, CAPACITY> entries{};
    util::SharedGroupAddressState state;

    void set(std::initializer_list<uint16_t> addresses)
    {
        state.beginChange();
        size_t count = 0;
        for (const uint16_t address : addresses) {
            entries[count++] = GroupAddress(address);
        }
        state.endChange(count);
    }

    util::SharedGroupAddressView view() const { return state.view(entries.data(), CAPACITY); }
};

// --- A device: UART, simulated chip, backend, optionally the stack above ---------

struct Device {
    SharedList groups;
    UartHalVirtual uart{16};
    VirtualTpUartChip chip;
    knx_uart_hal_t hal{};
    uint16_t own;
    std::unique_ptr<TpUartMediumBackend> owned;
    TpUartMediumBackend* backend{nullptr};
    std::unique_ptr<Tp1MacPhysical> mac;
    std::unique_ptr<datalink::Tp1DataLinkLayer> dataLink;
    std::vector<Tp1RxEvent> events;
    std::vector<std::vector<uint8_t>> telegrams;
    std::vector<datalink::LDataFrame> received;

    Device(VirtualTp1CharacterBus& bus,
           knx::platform::CooperativePlatform& platform,
           uint16_t ownAddress,
           const TpUartConfig& config = {},
           const VirtualTpUartChipConfig& chipConfig = {})
        : chip(bus, uart, chipConfig)
        , own(ownAddress)
    {
        uart.bind(hal);
        owned = std::make_unique<TpUartMediumBackend>(hal, platform, config);
        backend = owned.get();
        backend->setEventCallback(
            [this](const Tp1RxEvent& event, void*) {
                events.push_back(event);
                if (event.type == Tp1RxEventType::TelegramEnd) {
                    telegrams.emplace_back(event.frame.begin(), event.frame.end());
                }
            },
            nullptr);
    }

    util::Result<void> init()
    {
        Tp1MediumConfig config;
        config.ownIndividualAddressRaw = own;
        return backend->init(config);
    }

    void ackGroups(std::initializer_list<uint16_t> addresses)
    {
        groups.set(addresses);
        TEST_ASSERT_TRUE(backend->setAckGroupAddressView(groups.view()));
    }

    /// Put the data link layer on top; it initialises the backend.
    void withStack(knx::platform::CooperativePlatform& platform)
    {
        mac = std::make_unique<Tp1MacPhysical>(std::move(owned));
        mac->setTimingPlatform(&platform);
        auto config = datalink::Tp1DataLinkConfig::defaults();
        config.enableRxTask = false;
        dataLink = std::make_unique<datalink::Tp1DataLinkLayer>(platform, *mac, config);
        TEST_ASSERT_TRUE(dataLink->init(IndividualAddress(own)).isOk());
        dataLink->setReceiveCallback([this](const datalink::LDataFrame& f) { received.push_back(f); });
    }

    void service()
    {
        if (dataLink) {
            (void)dataLink->processRxAvailable(0u);
        } else {
            TEST_ASSERT_TRUE(backend->service().isOk());
        }
    }

    size_t countEvents(Tp1RxEventType type) const
    {
        return static_cast<size_t>(
            std::count_if(events.begin(), events.end(), [type](const Tp1RxEvent& e) { return e.type == type; }));
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

struct World {
    VirtualTp1CharacterBus bus;
    int bound{(g_bus = &bus, 0)};
    knx::platform::CooperativePlatform platform{makeHooks()};
    std::vector<Device*> devices;

    /// The main loop: the line runs, and every device is serviced every STEP_US.
    void run(uint64_t durationUs)
    {
        const uint64_t end = bus.nowUs() + durationUs;
        while (bus.nowUs() < end) {
            bus.runUntil(std::min(end, bus.nowUs() + STEP_US));
            for (Device* device : devices) {
                device->service();
            }
        }
    }
};

struct Fixture : World {
    Device dev;
    VirtualTp1Partner peer{bus, PEER_ADDRESS};

    explicit Fixture(const TpUartConfig& config = {}, const VirtualTpUartChipConfig& chipConfig = {})
        : dev(bus, platform, OWN_ADDRESS, config, chipConfig)
    {
        devices.push_back(&dev);
    }

    void start()
    {
        TEST_ASSERT_TRUE(dev.init().isOk());
        run(20u * MS);  // the configuration and the first U_State.req go through
    }
};

/// The host side of a UART with nobody but the test behind it, for the
/// chip's own behaviour.
struct RawHost {
    struct Octet {
        uint8_t value;
        uint8_t flags;
        uint64_t atUs;
    };
    UartHalVirtual uart{64};
    knx_uart_hal_t hal{};
    std::vector<Octet> octets;

    RawHost()
    {
        uart.bind(hal);
        const knx_uart_hal_config_t config{19200u, KNX_UART_HAL_PARITY_EVEN};
        TEST_ASSERT_TRUE(knx_uart_hal_open(
            &hal, &config,
            [](void* context, uint8_t octet, uint8_t flags, uint32_t) {
                auto* self = static_cast<RawHost*>(context);
                self->octets.push_back(Octet{octet, flags, g_bus->nowUs()});
                if (self->onOctet) {
                    self->onOctet(*self, octet);
                }
            },
            this));
    }

    void write(std::initializer_list<uint8_t> data)
    {
        const std::vector<uint8_t> v(data);
        TEST_ASSERT_TRUE(knx_uart_hal_write(&hal, v.data(), v.size()));
    }

    void writeFrame(const std::vector<uint8_t>& f)
    {
        for (size_t i = 0; i < f.size(); ++i) {
            const uint8_t service = static_cast<uint8_t>((i + 1u == f.size() ? 0x40u : 0x80u) | i);
            write({service, f[i]});
            // Keep within the UART buffer: let the link drain between pairs.
            g_bus->runFor(2u * 573u);
        }
    }

    void (*onOctet)(RawHost&, uint8_t){nullptr};
    uint8_t ackToWrite{0};
    size_t frameOctetsSeen{0};
};

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- The simulator against the H8i8o's measurements -------------------------------

void test_simulator_reset_and_state_timing_match_the_h8i8o(void)
{
    World w;
    RawHost host;
    VirtualTpUartChip chip(w.bus, host.uart);

    host.write({0x01u});  // U_Reset.req at t = 0
    w.bus.runFor(20u * MS);
    TEST_ASSERT_EQUAL_UINT32(2u, host.octets.size());
    // One 00 with a framing error about 1.4 ms later, then U_Reset.ind at 6.9 ms.
    TEST_ASSERT_EQUAL_HEX8(0x00u, host.octets[0].value);
    TEST_ASSERT_EQUAL_HEX8(KNX_UART_HAL_RX_FRAMING_ERROR, host.octets[0].flags);
    ASSERT_US_WITHIN(150u, 1400u, host.octets[0].atUs);
    TEST_ASSERT_EQUAL_HEX8(0x03u, host.octets[1].value);
    ASSERT_US_WITHIN(150u, 6900u, host.octets[1].atUs);

    const uint64_t requestUs = w.bus.nowUs();
    host.write({0x02u});  // U_State.req
    w.bus.runFor(5u * MS);
    TEST_ASSERT_EQUAL_UINT32(3u, host.octets.size());
    TEST_ASSERT_EQUAL_HEX8(0x07u, host.octets[2].value);
    ASSERT_US_WITHIN(100u, 1400u, host.octets[2].atUs - requestUs);

    host.write({0x20u});  // U_ProductID.req
    w.bus.runFor(5u * MS);
    TEST_ASSERT_EQUAL_HEX8(VirtualTpUartChip::PRODUCT_ID, host.octets.back().value);
}

void test_simulator_repeats_an_unacknowledged_frame_17_ms_apart_with_the_repeat_flag_cleared(void)
{
    World w;
    RawHost host;
    VirtualTpUartChip chip(w.bus, host.uart);
    host.write({0x01u});
    w.bus.runFor(10u * MS);
    host.write({0x28u, OWN_ADDRESS >> 8, OWN_ADDRESS & 0xFFu});
    w.bus.runFor(5u * MS);

    const auto f = deviceDescriptorRead(OWN_ADDRESS, ABSENT_ADDRESS);
    host.writeFrame(f);
    w.bus.runFor(100u * MS);

    const auto& log = w.bus.log();
    TEST_ASSERT_EQUAL_UINT32(4u, log.size());  // the first and 3 repetitions (the default)
    TEST_ASSERT_EQUAL_HEX8(0xBCu, log[0].frame[0]);
    for (size_t i = 1; i < log.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x9Cu, log[i].frame[0]);
        TEST_ASSERT_TRUE(tp1_frame::checksumOk(log[i].frame));
        TEST_ASSERT_FALSE(log[i].ack.has_value());
        // 17.3 ms on the H8i8o; 165 bit times here.
        ASSERT_US_WITHIN(300u, 17300u, log[i].startUs - log[i - 1].startUs);
    }
    TEST_ASSERT_EQUAL_HEX8(0x0Bu, host.octets.back().value);  // L_Data.con negative
    // The confirmation follows the last echoed octet by about 2.7 ms.
    const uint64_t lastEcho = host.octets[host.octets.size() - 2u].atUs;
    ASSERT_US_WITHIN(300u, 2700u, host.octets.back().atUs - lastEcho);
    // The reset glitch, U_Reset.ind, four echoes of the frame and the confirmation.
    TEST_ASSERT_EQUAL_UINT32(2u + 4u * f.size() + 1u, host.octets.size());
}

void test_simulator_refuses_a_second_frame_before_the_first_is_confirmed(void)
{
    World w;
    RawHost host;
    VirtualTpUartChip chip(w.bus, host.uart);
    host.write({0x01u});
    w.bus.runFor(10u * MS);
    const size_t before = host.octets.size();

    host.writeFrame(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1));
    host.writeFrame(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 2));
    w.bus.runFor(60u * MS);
    TEST_ASSERT_EQUAL_UINT32(1u, chip.statistics().framesFromHost);
    TEST_ASSERT_EQUAL_UINT32(1u, chip.statistics().protocolErrors);
    bool sawProtocolError = false;
    for (size_t i = before; i < host.octets.size(); ++i) {
        sawProtocolError = sawProtocolError || host.octets[i].value == 0x17u;
    }
    TEST_ASSERT_TRUE(sawProtocolError);
}

void test_simulator_busy_mode_ends_after_700_ms_or_on_request(void)
{
    World w;
    RawHost host;
    VirtualTpUartChip chip(w.bus, host.uart);
    host.write({0x01u});
    w.bus.runFor(10u * MS);

    host.write({0x21u});
    w.bus.runFor(MS);
    TEST_ASSERT_TRUE(chip.busyModeActive());
    w.bus.runFor(698u * MS);
    TEST_ASSERT_TRUE(chip.busyModeActive());
    w.bus.runFor(2u * MS);
    TEST_ASSERT_FALSE(chip.busyModeActive());

    host.write({0x21u});
    w.bus.runFor(MS);
    host.write({0x22u});
    w.bus.runFor(MS);
    TEST_ASSERT_FALSE(chip.busyModeActive());
}

void test_simulator_ack_information_with_the_addressed_bit_clear_withdraws_the_chip_acknowledge(void)
{
    // The manual does not say whether it does (design note §3.9 item 1); the
    // simulator assumes it, configurably.
    World w;
    RawHost host;
    VirtualTpUartChip chip(w.bus, host.uart);
    VirtualTp1Partner peer(w.bus, PEER_ADDRESS);
    host.write({0x01u});
    w.bus.runFor(10u * MS);
    host.write({0x28u, OWN_ADDRESS >> 8, OWN_ADDRESS & 0xFFu});
    w.bus.runFor(5u * MS);

    host.onOctet = [](RawHost& self, uint8_t) {
        if (++self.frameOctetsSeen == 6u) {
            self.write({0x10u});  // not addressed
        }
    };
    peer.send(groupWrite(PEER_ADDRESS, GROUP_NOT_IN_TABLE, 1));
    w.bus.runFor(30u * MS);
    TEST_ASSERT_EQUAL_UINT32(1u, peer.acksToOwnFrames().size());
    TEST_ASSERT_FALSE(peer.acksToOwnFrames()[0].has_value());
    TEST_ASSERT_EQUAL_UINT32(1u, chip.statistics().ackInformation);
}

// --- The backend over the simulated chip: start-up ----------------------------------

void test_backend_starts_on_the_simulated_chip_through_its_reset_glitch(void)
{
    Fixture f;
    f.start();
    TEST_ASSERT_TRUE(f.dev.chip.address().has_value());
    TEST_ASSERT_EQUAL_HEX16(OWN_ADDRESS, *f.dev.chip.address());
    TEST_ASSERT_EQUAL_UINT8(3u, f.dev.chip.busyRepetitions());
    TEST_ASSERT_EQUAL_UINT8(3u, f.dev.chip.nackRepetitions());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Up), static_cast<int>(f.dev.backend->getLinkState()));
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.backend->statistics().unexpectedOctets);  // the glitch
}

void test_backend_reports_the_temperature_warning_from_a_state_indication(void)
{
    Fixture f;
    f.start();
    f.dev.chip.setTemperatureWarning(true);
    f.run(5u * MS);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Degraded), static_cast<int>(f.dev.backend->getLinkState()));
}

// --- Sending -------------------------------------------------------------------------

void test_group_write_acknowledged_by_a_partner_is_confirmed_positively(void)
{
    Fixture f;
    f.peer.groups = {GROUP_IN_TABLE};
    f.start();
    const auto g = groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1);
    TEST_ASSERT_TRUE(f.dev.backend->sendFrame(g).isOk());
    f.run(50u * MS);

    TEST_ASSERT_EQUAL_UINT32(1u, f.bus.log().size());
    TEST_ASSERT_TRUE(f.bus.log()[0].frame == g);
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::ACK, *f.bus.log()[0].ack);
    const auto outcome = f.dev.lastEvent(Tp1RxEventType::TxAckResponse);
    TEST_ASSERT_TRUE(outcome.has_value());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1AckClass::Ack), static_cast<int>(outcome->ackClass));
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.backend->statistics().echoesDropped);
    TEST_ASSERT_EQUAL_UINT32(0u, f.dev.telegrams.size());  // the echo is not delivered
}

void test_unacknowledged_individual_frame_is_repeated_by_the_chip_and_confirmed_negatively(void)
{
    Fixture f;
    f.start();
    TEST_ASSERT_TRUE(f.dev.backend->sendFrame(deviceDescriptorRead(OWN_ADDRESS, ABSENT_ADDRESS)).isOk());
    f.run(100u * MS);

    TEST_ASSERT_EQUAL_UINT32(4u, f.bus.log().size());
    const auto outcome = f.dev.lastEvent(Tp1RxEventType::TxAckResponse);
    TEST_ASSERT_TRUE(outcome.has_value());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Tp1AckClass::None), static_cast<int>(outcome->ackClass));
    TEST_ASSERT_EQUAL_UINT32(4u, f.dev.backend->statistics().echoesDropped);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.backend->statistics().confirmsNegative);
    TEST_ASSERT_EQUAL_UINT32(0u, f.dev.telegrams.size());
}

void test_busy_partner_uses_the_busy_repetition_count(void)
{
    TpUartConfig config;
    config.busyRepetitions = 1u;
    Fixture f(config);
    f.peer.answer = [](const std::vector<uint8_t>&) -> std::optional<uint8_t> { return VirtualTp1CharacterBus::BUSY; };
    f.start();
    TEST_ASSERT_TRUE(f.dev.backend->sendFrame(deviceDescriptorRead(OWN_ADDRESS, PEER_ADDRESS)).isOk());
    f.run(100u * MS);
    TEST_ASSERT_EQUAL_UINT32(2u, f.bus.log().size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.backend->statistics().confirmsNegative);

    int answers = 0;
    f.peer.answer = [&answers](const std::vector<uint8_t>&) -> std::optional<uint8_t> {
        return ++answers == 1 ? VirtualTp1CharacterBus::BUSY : VirtualTp1CharacterBus::ACK;
    };
    TEST_ASSERT_TRUE(f.dev.backend->sendFrame(deviceDescriptorRead(OWN_ADDRESS, PEER_ADDRESS)).isOk());
    f.run(100u * MS);
    TEST_ASSERT_EQUAL_UINT32(4u, f.bus.log().size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.backend->statistics().confirmsPositive);
}

void test_missing_confirmation_resets_the_chip_and_sets_the_address_again(void)
{
    Fixture f;
    f.peer.groups = {GROUP_IN_TABLE};
    f.start();
    f.dev.chip.dropNextConfirm();
    TEST_ASSERT_TRUE(f.dev.backend->sendFrame(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1)).isOk());
    f.run(1200u * MS);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.countEvents(Tp1RxEventType::TxAckDeadlineMiss));
    TEST_ASSERT_EQUAL_UINT32(2u, f.dev.chip.statistics().resets);
    TEST_ASSERT_TRUE(f.dev.chip.address().has_value());
    TEST_ASSERT_EQUAL_HEX16(OWN_ADDRESS, *f.dev.chip.address());
}

void test_chip_reset_while_our_frame_is_fed_fails_it_and_reconfigures(void)
{
    Fixture f;
    f.peer.groups = {GROUP_IN_TABLE};
    f.start();
    TEST_ASSERT_TRUE(f.dev.backend->sendFrame(longGroupWrite(OWN_ADDRESS, GROUP_IN_TABLE)).isOk());
    f.run(5u * MS);
    f.dev.chip.resetSpontaneously();
    f.run(50u * MS);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.countEvents(Tp1RxEventType::TxAckDeadlineMiss));
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.backend->statistics().chipResets);
    TEST_ASSERT_TRUE(f.dev.chip.address().has_value());
    TEST_ASSERT_EQUAL_UINT32(0u, f.bus.log().size());  // the half-fed frame never reached the line

    TEST_ASSERT_TRUE(f.dev.backend->sendFrame(groupWrite(OWN_ADDRESS, GROUP_IN_TABLE, 1)).isOk());
    f.run(50u * MS);
    TEST_ASSERT_EQUAL_UINT32(1u, f.bus.log().size());
}

// --- Receiving and acknowledging -----------------------------------------------------

void test_group_frame_in_the_table_is_acknowledged_in_time_and_delivered(void)
{
    Fixture f;
    f.start();
    f.dev.ackGroups({GROUP_IN_TABLE});
    const auto g = groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1);
    f.peer.send(g);
    f.run(30u * MS);

    TEST_ASSERT_EQUAL_UINT32(1u, f.peer.acksToOwnFrames().size());
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::ACK, *f.peer.acksToOwnFrames()[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.chip.statistics().ackInformation);
    TEST_ASSERT_EQUAL_UINT32(0u, f.dev.chip.statistics().lateAckInformation);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.telegrams.size());
    TEST_ASSERT_TRUE(f.dev.telegrams[0] == g);
}

void test_chip_acknowledges_group_frames_outside_the_table_once_its_address_is_set(void)
{
    // TP-UART2 manual §3.2.3.1.7: with an address set, "group-addressed
    // telegrams are now generally confirmed". The backend sends nothing for a
    // group it does not listen to, so the chip acknowledges it (design note
    // §3.9 item 1, open).
    Fixture f;
    f.start();
    f.dev.ackGroups({GROUP_IN_TABLE});
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_NOT_IN_TABLE, 1));
    f.run(30u * MS);
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::ACK, *f.peer.acksToOwnFrames()[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, f.dev.chip.statistics().ackInformation);
}

void test_individual_frames_are_acknowledged_by_the_chip_only_for_the_own_address(void)
{
    Fixture f;
    f.start();
    f.peer.send(deviceDescriptorRead(PEER_ADDRESS, OWN_ADDRESS));
    f.peer.send(deviceDescriptorRead(PEER_ADDRESS, ABSENT_ADDRESS));
    f.run(60u * MS);
    TEST_ASSERT_EQUAL_UINT32(2u, f.peer.acksToOwnFrames().size());
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::ACK, *f.peer.acksToOwnFrames()[0]);
    TEST_ASSERT_FALSE(f.peer.acksToOwnFrames()[1].has_value());
    TEST_ASSERT_EQUAL_UINT32(2u, f.dev.telegrams.size());  // the data link layer filters, not the backend
}

void test_local_busy_puts_busy_on_the_line(void)
{
    Fixture f;
    f.start();
    f.dev.ackGroups({GROUP_IN_TABLE});
    f.dev.backend->setLocalBusy(true);
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1));
    f.run(30u * MS);
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::BUSY, *f.peer.acksToOwnFrames()[0]);
}

void test_busy_answer_is_in_time_while_our_own_frame_is_being_fed(void)
{
    // The acknowledge decision is written from the RX interrupt and queues
    // behind whatever the main context has queued for the chip. It must still
    // reach the chip within 1.7 ms of the address-type octet (manual
    // §3.2.3.1.8), or the chip answers by its own address evaluation: ACK for
    // a frame this device cannot take.
    Fixture f;
    f.peer.groups = {GROUP_NOT_IN_TABLE};  // it acknowledges our frame
    f.start();
    f.dev.ackGroups({GROUP_IN_TABLE});
    f.dev.backend->setLocalBusy(true);
    TEST_ASSERT_TRUE(f.dev.backend->sendFrame(longGroupWrite(OWN_ADDRESS, GROUP_NOT_IN_TABLE)).isOk());
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1));
    f.run(80u * MS);

    TEST_ASSERT_EQUAL_UINT32(0u, f.dev.chip.statistics().lateAckInformation);
    TEST_ASSERT_EQUAL_UINT32(1u, f.peer.acksToOwnFrames().size());
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::BUSY, *f.peer.acksToOwnFrames()[0]);
    // Our frame still went out afterwards.
    TEST_ASSERT_EQUAL_UINT32(2u, f.bus.log().size());
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.countEvents(Tp1RxEventType::TxAckResponse));
}

void test_late_ack_information_is_ignored_and_the_chip_answers_by_itself(void)
{
    Fixture f;
    f.start();
    f.dev.ackGroups({GROUP_IN_TABLE});
    f.dev.backend->setLocalBusy(true);
    // The U_AckInformation waits in the UART: it reaches the chip after the
    // deadline (8.5 + 1.7 ms after the frame starts), before the acknowledge
    // slot (13.5 ms).
    f.dev.chip.pauseHostInput(11u * MS);
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1));
    f.run(30u * MS);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.chip.statistics().lateAckInformation);
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::ACK, *f.peer.acksToOwnFrames()[0]);
}

void test_lost_octet_to_the_host_discards_that_frame_and_the_next_is_received(void)
{
    Fixture f;
    f.start();
    f.dev.chip.faultFrameOctetToHost(7u, true);
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1));
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 2));
    f.run(60u * MS);
    // The missing octet leaves a gap of two octet times (2.7 ms), which ends
    // the frame. Here its FCS (30h) then looks like an extended frame start,
    // cut off in turn by the next frame.
    TEST_ASSERT_TRUE(f.dev.backend->statistics().partialFramesDiscarded >= 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.telegrams.size());
    TEST_ASSERT_TRUE(f.dev.telegrams[0] == groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 2));
}

void test_corrupted_octets_to_the_host_are_dropped_as_checksum_or_octet_errors(void)
{
    Fixture f;
    f.start();
    f.dev.chip.faultFrameOctetToHost(6u, false, 0x01u);
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1));
    f.run(30u * MS);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.backend->statistics().checksumErrors);

    f.dev.chip.faultFrameOctetToHost(6u, false, 0x00u, KNX_UART_HAL_RX_PARITY_ERROR);
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1));
    f.run(30u * MS);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.backend->statistics().octetErrorFrames);
    TEST_ASSERT_EQUAL_UINT32(0u, f.dev.telegrams.size());
}

void test_frame_corrupted_on_the_line_is_nacked_and_reported_as_a_receive_error(void)
{
    Fixture f;
    f.start();
    const uint32_t stateBefore = f.dev.backend->statistics().stateIndications;
    f.peer.send(deviceDescriptorRead(PEER_ADDRESS, OWN_ADDRESS), 6u);
    f.run(30u * MS);
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::NACK, *f.peer.acksToOwnFrames()[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.backend->statistics().checksumErrors);
    TEST_ASSERT_EQUAL_UINT32(stateBefore + 1u, f.dev.backend->statistics().stateIndications);
    TEST_ASSERT_EQUAL_HEX8(0x47u, f.dev.backend->statistics().lastStateBits);
}

// --- Flash erases (busy mode) ---------------------------------------------------------

void test_frames_to_the_device_are_answered_busy_during_an_erase_and_received_after_it(void)
{
    Fixture f;
    f.start();
    f.dev.ackGroups({GROUP_IN_TABLE});
    TEST_ASSERT_TRUE(f.dev.backend->beginFlashErase(false));
    // U_ActivateBusyMode has left the UART's queue; the hardware shifts it
    // out during the erase, long before any frame reaches its acknowledge.
    f.bus.runFor(MS);
    TEST_ASSERT_TRUE(f.dev.chip.busyModeActive());
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 1));
    f.run(30u * MS);
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::BUSY, *f.peer.acksToOwnFrames()[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, f.dev.telegrams.size());

    f.dev.backend->endFlashErase();
    f.run(10u * MS);
    TEST_ASSERT_FALSE(f.dev.chip.busyModeActive());
    f.peer.send(groupWrite(PEER_ADDRESS, GROUP_IN_TABLE, 2));
    f.run(30u * MS);
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::ACK, *f.peer.acksToOwnFrames()[1]);
    TEST_ASSERT_EQUAL_UINT32(1u, f.dev.telegrams.size());
}

// --- Two stacks on one line --------------------------------------------------------------

void test_two_devices_exchange_a_group_write_through_their_chips(void)
{
    World w;
    Device a(w.bus, w.platform, OWN_ADDRESS);
    Device b(w.bus, w.platform, SECOND_ADDRESS);
    w.devices = {&a, &b};
    a.withStack(w.platform);
    b.withStack(w.platform);
    TEST_ASSERT_TRUE(b.dataLink->addGroupAddress(GroupAddress(GROUP_IN_TABLE)).isOk());
    w.run(20u * MS);

    datalink::LDataFrame f;
    f.standardFrame = true;
    f.priority = Priority::Low;
    f.ackRequested = false;
    f.source = IndividualAddress(OWN_ADDRESS);
    f.destination = GroupAddress(GROUP_IN_TABLE);
    f.destinationType = AddressType::Group;
    f.hopCount = 6;
    f.setTpdu(protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01u});
    TEST_ASSERT_TRUE(a.dataLink->sendFrame(f).isOk());
    w.run(30u * MS);

    TEST_ASSERT_EQUAL_UINT32(1u, w.bus.log().size());
    TEST_ASSERT_EQUAL_HEX8(VirtualTp1CharacterBus::ACK, *w.bus.log()[0].ack);
    TEST_ASSERT_EQUAL_UINT32(1u, b.received.size());
    TEST_ASSERT_EQUAL_HEX16(GROUP_IN_TABLE, b.received[0].destination.raw);
    TEST_ASSERT_EQUAL_HEX16(OWN_ADDRESS, b.received[0].source.raw);
    TEST_ASSERT_EQUAL_UINT32(0u, a.received.size());  // A's own echo stays in its backend
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_simulator_reset_and_state_timing_match_the_h8i8o);
    RUN_TEST(test_simulator_repeats_an_unacknowledged_frame_17_ms_apart_with_the_repeat_flag_cleared);
    RUN_TEST(test_simulator_refuses_a_second_frame_before_the_first_is_confirmed);
    RUN_TEST(test_simulator_busy_mode_ends_after_700_ms_or_on_request);
    RUN_TEST(test_simulator_ack_information_with_the_addressed_bit_clear_withdraws_the_chip_acknowledge);
    RUN_TEST(test_backend_starts_on_the_simulated_chip_through_its_reset_glitch);
    RUN_TEST(test_backend_reports_the_temperature_warning_from_a_state_indication);
    RUN_TEST(test_group_write_acknowledged_by_a_partner_is_confirmed_positively);
    RUN_TEST(test_unacknowledged_individual_frame_is_repeated_by_the_chip_and_confirmed_negatively);
    RUN_TEST(test_busy_partner_uses_the_busy_repetition_count);
    RUN_TEST(test_missing_confirmation_resets_the_chip_and_sets_the_address_again);
    RUN_TEST(test_chip_reset_while_our_frame_is_fed_fails_it_and_reconfigures);
    RUN_TEST(test_group_frame_in_the_table_is_acknowledged_in_time_and_delivered);
    RUN_TEST(test_chip_acknowledges_group_frames_outside_the_table_once_its_address_is_set);
    RUN_TEST(test_individual_frames_are_acknowledged_by_the_chip_only_for_the_own_address);
    RUN_TEST(test_local_busy_puts_busy_on_the_line);
    RUN_TEST(test_busy_answer_is_in_time_while_our_own_frame_is_being_fed);
    RUN_TEST(test_late_ack_information_is_ignored_and_the_chip_answers_by_itself);
    RUN_TEST(test_lost_octet_to_the_host_discards_that_frame_and_the_next_is_received);
    RUN_TEST(test_corrupted_octets_to_the_host_are_dropped_as_checksum_or_octet_errors);
    RUN_TEST(test_frame_corrupted_on_the_line_is_nacked_and_reported_as_a_receive_error);
    RUN_TEST(test_frames_to_the_device_are_answered_busy_during_an_erase_and_received_after_it);
    RUN_TEST(test_two_devices_exchange_a_group_write_through_their_chips);
    return UNITY_END();
}
