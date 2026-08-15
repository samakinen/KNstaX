// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/physical/ip_routing_physical.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/platform/linux_platform.hpp"

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

using namespace knx;
using namespace knx::datalink;
using namespace knx::physical;

void setUp(void) {}
void tearDown(void) {}

static Tp1DataLinkConfig polledRxConfig() {
    auto config = Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;
    return config;
}

static uint16_t pickPort() {
    if (const char* v = std::getenv("KNX_TEST_MCAST_PORT")) {
        const unsigned long parsed = std::strtoul(v, nullptr, 10);
        if (parsed > 0 && parsed <= 65535) {
            return static_cast<uint16_t>(parsed);
        }
    }
    const uint32_t pid = static_cast<uint32_t>(::getpid());
    return static_cast<uint16_t>(30000 + (pid % 20000));
}

static knx::IpAddress pickGroup() {
    if (const char* v = std::getenv("KNX_TEST_MCAST_GROUP")) {
        return knx::IpAddress::fromString(v);
    }
    const uint32_t pid = static_cast<uint32_t>(::getpid());
    const uint8_t octet = static_cast<uint8_t>(1 + (pid % 250));
    return knx::IpAddress::fromOctets(239, 255, 0, octet);
}

static knx::IpAddress pickIface() {
    if (const char* v = std::getenv("KNX_TEST_MCAST_IFACE")) {
        return knx::IpAddress::fromString(v);
    }
    return knx::IpAddress::fromOctets(127, 0, 0, 1);
}

void test_ip_routing_physical_send_receive(void) {
    const auto group = pickGroup();
    const auto port = pickPort();
    const auto iface = pickIface();

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpRoutingPhysical physA;
    physA.setNetworkInterface(net);
    physA.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkLayer dlA(platform, physA);
    TEST_ASSERT_TRUE(dlA.init(IndividualAddress(0x1101)).isOk());
    TEST_ASSERT_TRUE(dlA.addGroupAddress(GroupAddress(0x0100)).isOk());

    IpRoutingPhysical physB;
    physB.setNetworkInterface(net);
    physB.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkLayer dlB(platform, physB);
    TEST_ASSERT_TRUE(dlB.init(IndividualAddress(0x1102)).isOk());
    TEST_ASSERT_TRUE(dlB.addGroupAddress(GroupAddress(0x0100)).isOk());

    std::atomic<bool> got{false};
    LDataFrame received;

    dlB.setReceiveCallback([&](const LDataFrame& in) {
        received = in;
        got.store(true);
    });

    LDataFrame out;
    out.standardFrame = true;
    out.repeated = false;
    out.priority = Priority::Low;
    out.ackRequested = false;
    out.confirmation = false;
    out.source = IndividualAddress(0x1101);
    out.destination = GroupAddress(0x0100);
    out.destinationType = AddressType::Group;
    out.hopCount = 6;
    out.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueRead, {});

    TEST_ASSERT_TRUE(dlA.sendFrame(out).isOk());

    for (int i = 0; i < 50 && !got.load(); ++i) {
        ::usleep(10 * 1000);
    }

    TEST_ASSERT_TRUE(got.load());
    TEST_ASSERT_TRUE(received.destinationType == AddressType::Group);
    TEST_ASSERT_EQUAL_HEX16(out.destination.raw, received.destination.raw);
    TEST_ASSERT_EQUAL_HEX16(out.source.raw, received.source.raw);
    TEST_ASSERT_EQUAL_UINT8(out.hopCount, received.hopCount);
    TEST_ASSERT_EQUAL_UINT8(out.tpdu.size(), received.tpdu.size());
    for (size_t i = 0; i < out.tpdu.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(out.tpdu[i], received.tpdu[i]);
    }

    dlA.close();
    dlB.close();
}

void test_ip_routing_physical_send_receive_with_explicit_progression(void) {
    const auto group = pickGroup();
    const auto port = pickPort();
    const auto iface = pickIface();

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpRoutingPhysical physA;
    physA.setNetworkInterface(net);
    physA.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkLayer dlA(platform, physA, nullptr, polledRxConfig());
    TEST_ASSERT_TRUE(dlA.init(IndividualAddress(0x1101)).isOk());
    TEST_ASSERT_TRUE(dlA.addGroupAddress(GroupAddress(0x0100)).isOk());

    IpRoutingPhysical physB;
    physB.setNetworkInterface(net);
    physB.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkLayer dlB(platform, physB, nullptr, polledRxConfig());
    TEST_ASSERT_TRUE(dlB.init(IndividualAddress(0x1102)).isOk());
    TEST_ASSERT_TRUE(dlB.addGroupAddress(GroupAddress(0x0100)).isOk());

    std::atomic<bool> got{false};
    LDataFrame received;

    dlB.setReceiveCallback([&](const LDataFrame& in) {
        received = in;
        got.store(true);
    });

    LDataFrame out;
    out.standardFrame = true;
    out.repeated = false;
    out.priority = Priority::Low;
    out.ackRequested = false;
    out.confirmation = false;
    out.source = IndividualAddress(0x1101);
    out.destination = GroupAddress(0x0100);
    out.destinationType = AddressType::Group;
    out.hopCount = 6;
    out.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueRead, {});

    TEST_ASSERT_TRUE(dlA.sendFrame(out).isOk());

    for (int i = 0; i < 50 && !got.load(); ++i) {
        (void)dlA.processRxAvailable(0);
        (void)dlB.processRxAvailable(0);
        ::usleep(10 * 1000);
    }

    TEST_ASSERT_TRUE(got.load());
    TEST_ASSERT_TRUE(received.destinationType == AddressType::Group);
    TEST_ASSERT_EQUAL_HEX16(out.destination.raw, received.destination.raw);
    TEST_ASSERT_EQUAL_HEX16(out.source.raw, received.source.raw);
    TEST_ASSERT_EQUAL_UINT8(out.hopCount, received.hopCount);
    TEST_ASSERT_EQUAL_UINT8(out.tpdu.size(), received.tpdu.size());
    for (size_t i = 0; i < out.tpdu.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(out.tpdu[i], received.tpdu[i]);
    }

    dlA.close();
    dlB.close();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ip_routing_physical_send_receive);
    RUN_TEST(test_ip_routing_physical_send_receive_with_explicit_progression);
    return UNITY_END();
}
