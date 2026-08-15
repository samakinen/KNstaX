// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <array>
#include <span>

#include "unity.h"

#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/network/two_port_coupler.hpp"
#include "knx/physical/ip_routing_physical.hpp"
#include "knx/netip/routing_endpoint.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/testing/mock_tp1_physical.hpp"

#include "knx/platform/linux_platform.hpp"

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

using namespace knx;
using namespace knx::datalink;
using namespace knx::network;
using namespace knx::physical;

void setUp(void) {}
void tearDown(void) {}

static uint16_t pickPort() {
    if (const char* v = std::getenv("KNX_TEST_MCAST_PORT")) {
        const unsigned long parsed = std::strtoul(v, nullptr, 10);
        if (parsed > 0 && parsed <= 65535) {
            return static_cast<uint16_t>(parsed);
        }
    }
    const uint32_t pid = static_cast<uint32_t>(::getpid());
    return static_cast<uint16_t>(32000 + (pid % 20000));
}

static knx::IpAddress pickGroup() {
    if (const char* v = std::getenv("KNX_TEST_MCAST_GROUP")) {
        return knx::IpAddress::fromString(v);
    }
    const uint32_t pid = static_cast<uint32_t>(::getpid());
    const uint8_t octet = static_cast<uint8_t>(1 + (pid % 250));
    return knx::IpAddress::fromOctets(239, 255, 1, octet);
}

static knx::IpAddress pickIface() {
    if (const char* v = std::getenv("KNX_TEST_MCAST_IFACE")) {
        return knx::IpAddress::fromString(v);
    }
    return knx::IpAddress::fromOctets(127, 0, 0, 1);
}

static LDataFrame makeGroupRead(const IndividualAddress& src, const GroupAddress& dst, uint8_t hop) {
    LDataFrame out;
    out.standardFrame = true;
    out.repeated = false;
    out.priority = Priority::Low;
    out.ackRequested = false;
    out.confirmation = false;
    out.source = src;
    out.destination = dst;
    out.destinationType = AddressType::Group;
    out.hopCount = hop;
    out.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueRead, {});
    return out;
}

void test_tp1_to_ip_routing_forward_and_filter(void) {
    const auto mcastGroup = pickGroup();
    const auto port = pickPort();
    const auto iface = pickIface();

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    // Port A: TP1 line side (stub physical)
    testing::MockTp1Physical tp1Phys;
    Tp1DataLinkLayer dlTp1(platform, tp1Phys);
    TEST_ASSERT_TRUE(dlTp1.init(IndividualAddress(0x1100)).isOk());
    dlTp1.setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);

    // Port B: IP routing side (multicast)
    IpRoutingPhysical ipPhys;
    ipPhys.setNetworkInterface(net);
    ipPhys.setMulticast(mcastGroup, NetIpPort(port), iface);
    Tp1DataLinkLayer dlIp(platform, ipPhys);
    TEST_ASSERT_TRUE(dlIp.init(IndividualAddress(0x1200)).isOk());
    dlIp.setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);

    // Receiver on multicast to observe forwarded frames
    IpRoutingPhysical rxPhys;
    rxPhys.setNetworkInterface(net);
    rxPhys.setMulticast(mcastGroup, NetIpPort(port), iface);
    Tp1DataLinkLayer dlRx(platform, rxPhys);
    TEST_ASSERT_TRUE(dlRx.init(IndividualAddress(0x1201)).isOk());
    dlRx.setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);

    std::atomic<bool> got{false};
    LDataFrame received;
    dlRx.setReceiveCallback([&](const LDataFrame& f) {
        received = f;
        got.store(true);
    });

    TwoPortCoupler coupler(dlTp1, dlIp);
    coupler.setRoutingEnabled(knx::Toggle::Enable);
    coupler.setFilteringEnabled(knx::Toggle::Enable);
    TEST_ASSERT_TRUE(coupler.init().isOk());

    // Forwarding case
    const GroupAddress groupAddr(0x0100);
    LDataFrame in = makeGroupRead(IndividualAddress(0x110A), groupAddr, 6);
    uint8_t raw[64];
    auto enc = FrameCodec::encodeFrame(in, std::span<uint8_t>(raw));
    TEST_ASSERT_TRUE(enc.isOk());
    const size_t rawLen = enc.value();

    tp1Phys.injectRxFrame(std::vector<uint8_t>(raw, raw + rawLen));

    for (int i = 0; i < 80 && !got.load(); ++i) {
        ::usleep(5 * 1000);
    }
    TEST_ASSERT_TRUE(got.load());
    TEST_ASSERT_EQUAL_HEX16(in.destination.raw, received.destination.raw);
    TEST_ASSERT_EQUAL_HEX16(in.source.raw, received.source.raw);
    TEST_ASSERT_EQUAL_UINT8(5, received.hopCount); // decremented by coupler

    // Filter-table block case
    got.store(false);
    coupler.filterTable().clear();
    TEST_ASSERT_TRUE(coupler.filterTable().addEntry(groupAddr, knx::GroupAddress(0xFFFF), FilterAction::Block, knx::EntryState::Enabled).isOk());

    tp1Phys.injectRxFrame(std::vector<uint8_t>(raw, raw + rawLen));
    for (int i = 0; i < 40 && !got.load(); ++i) {
        ::usleep(5 * 1000);
    }
    TEST_ASSERT_FALSE(got.load());

    coupler.close();
    dlTp1.close();
    dlIp.close();
    dlRx.close();
}

void test_ip_routing_to_tp1_forward_and_filter(void) {
    const auto mcastGroup = pickGroup();
    const auto port = pickPort();
    const auto iface = pickIface();

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    // Port A: TP1 line side (stub physical)
    testing::MockTp1Physical tp1Phys;
    Tp1DataLinkLayer dlTp1(platform, tp1Phys);
    TEST_ASSERT_TRUE(dlTp1.init(IndividualAddress(0x1100)).isOk());
    dlTp1.setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);

    // Port B: IP routing side (multicast)
    IpRoutingPhysical ipPhys;
    ipPhys.setNetworkInterface(net);
    ipPhys.setMulticast(mcastGroup, NetIpPort(port), iface);
    Tp1DataLinkLayer dlIp(platform, ipPhys);
    TEST_ASSERT_TRUE(dlIp.init(IndividualAddress(0x1200)).isOk());
    dlIp.setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);

    TwoPortCoupler coupler(dlTp1, dlIp);
    coupler.setRoutingEnabled(knx::Toggle::Enable);
    coupler.setFilteringEnabled(knx::Toggle::Enable);
    TEST_ASSERT_TRUE(coupler.init().isOk());

    // Inject a routing indication into multicast (acts as "IP-side" incoming telegram)
    const GroupAddress groupAddr(0x0100);
    const LDataFrame in = makeGroupRead(IndividualAddress(0x2211), groupAddr, 6);
    std::array<uint8_t, knx::netip::kMaxCemiLDataSize> cemi{};
    auto cemiResult = knx::netip::encodeCemiLData(in, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult.isOk());

    knx::netip::RoutingEndpoint sender;
    knx::netip::RoutingEndpoint::Options senderOptions;
    senderOptions.multicastGroup = mcastGroup;
    senderOptions.port = NetIpPort(port);
    senderOptions.interfaceAddress = iface;
    TEST_ASSERT_TRUE(sender.open(*net, senderOptions).isOk());
    const auto initialSend = sender.sendRoutingIndication(std::span<const uint8_t>(cemi.data(), cemiResult.value()));
    TEST_ASSERT_TRUE(initialSend.isOk());

    // Observe forwarded TP1 bytes via stub TX queue.
    std::vector<uint8_t> forwardedRaw;
    bool forwarded = false;
    for (int i = 0; i < 80 && !forwarded; ++i) {
        forwarded = tp1Phys.popTxFrame(forwardedRaw);
        if (!forwarded) {
            ::usleep(5 * 1000);
        }
    }
    TEST_ASSERT_TRUE(forwarded);

    LDataFrame decoded;
    auto decRes = FrameCodec::decodeFrame(std::span<const uint8_t>(forwardedRaw), decoded);
    TEST_ASSERT_TRUE(decRes.isOk());
    TEST_ASSERT_TRUE(decoded.destinationType == AddressType::Group);
    TEST_ASSERT_EQUAL_HEX16(in.destination.raw, decoded.destination.raw);
    TEST_ASSERT_EQUAL_HEX16(in.source.raw, decoded.source.raw);
    TEST_ASSERT_EQUAL_UINT8(5, decoded.hopCount); // decremented by coupler

    // Filter-table block case: block group addr and re-send; should not forward again.
    coupler.filterTable().clear();
    TEST_ASSERT_TRUE(coupler.filterTable().addEntry(groupAddr, knx::GroupAddress(0xFFFF), FilterAction::Block, knx::EntryState::Enabled).isOk());

    const auto blockedSend = sender.sendRoutingIndication(cemi);
    TEST_ASSERT_TRUE(blockedSend.isOk());

    forwardedRaw.clear();
    forwarded = false;
    for (int i = 0; i < 40 && !forwarded; ++i) {
        forwarded = tp1Phys.popTxFrame(forwardedRaw);
        if (!forwarded) {
            ::usleep(5 * 1000);
        }
    }
    TEST_ASSERT_FALSE(forwarded);

    sender.close();
    coupler.close();
    dlTp1.close();
    dlIp.close();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_tp1_to_ip_routing_forward_and_filter);
    RUN_TEST(test_ip_routing_to_tp1_forward_and_filter);
    return UNITY_END();
}
