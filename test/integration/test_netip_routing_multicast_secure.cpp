// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#if KNX_SECURE_ENABLED

#include "knx/netip/routing_endpoint.hpp"
#include "knx/netip/ip_secure/secure_routing_security.hpp"

#include "knx/netip/cemi.hpp"
#include "knx/platform/linux_platform.hpp"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include <unistd.h>

using knx::NetIpPort;

using namespace knx;
using namespace knx::netip;
using namespace knx::netip::ip_secure;
using namespace knx::datalink;

void setUp(void) {}
void tearDown(void) {}

static void assert_uint8_array_equal(std::span<const uint8_t> expected, std::span<const uint8_t> actual)
{
    TEST_ASSERT_EQUAL_UINT32(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); i++) {
        TEST_ASSERT_EQUAL_UINT8(expected[i], actual[i]);
    }
}

static LDataFrame makeFrame()
{
    LDataFrame f;
    f.standardFrame = true;
    f.repeated = false;
    f.priority = Priority::Normal;
    f.ackRequested = true;
    f.confirmation = true;
    f.source = IndividualAddress(0x110A);
    f.destination = GroupAddress(0x2301);
    f.destinationType = AddressType::Group;
    f.hopCount = 6;
    f.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
    return f;
}

static knx::IpAddress pickIface()
{
    if (const char* v = std::getenv("KNX_TEST_MCAST_IFACE")) {
        return knx::IpAddress::fromString(v);
    }
    return knx::IpAddress::fromOctets(127, 0, 0, 1);
}

static knx::IpAddress pickGroup()
{
    if (const char* v = std::getenv("KNX_TEST_MCAST_GROUP")) {
        return knx::IpAddress::fromString(v);
    }
    // Use a PID-derived multicast group to reduce cross-test interference.
    const int pid = static_cast<int>(::getpid());
    const uint8_t groupLastOctet = static_cast<uint8_t>(1 + (pid % 250));
    return knx::IpAddress::fromOctets(239, 255, 1, groupLastOctet);
}

static uint16_t pickPort()
{
    if (const char* v = std::getenv("KNX_TEST_MCAST_PORT")) {
        const unsigned long parsed = std::strtoul(v, nullptr, 10);
        if (parsed > 0 && parsed <= 65535) {
            return static_cast<uint16_t>(parsed);
        }
    }
    // Use a PID-derived port to reduce cross-test interference.
    const int pid = static_cast<int>(::getpid());
    return static_cast<uint16_t>(32000 + (pid % 20000));
}

void test_routing_multicast_send_receive_secure(void)
{
    const knx::IpAddress group = pickGroup();
    const uint16_t port = pickPort();
    const knx::IpAddress iface = pickIface();

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    SecureRoutingSecurity::Key groupKey{};
    for (size_t i = 0; i < groupKey.size(); ++i) {
        groupKey[i] = static_cast<uint8_t>(i);
    }

    const std::array<uint8_t, SecureWrapper::kSerialLen> serial{0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    const std::array<uint8_t, SecureWrapper::kTagLen> tag{0x12, 0x34};

    SecureRoutingSecurity secSender(groupKey, serial, tag, /*initialSeq=*/1);
    SecureRoutingSecurity secReceiver(groupKey, serial, tag, /*initialSeq=*/0);

    RoutingEndpoint a;
    RoutingEndpoint b;

    RoutingEndpoint::Options optA;
    optA.multicastGroup = group;
    optA.port = NetIpPort(port);
    optA.interfaceAddress = iface;
    optA.security = &secSender;

    RoutingEndpoint::Options optB;
    optB.multicastGroup = group;
    optB.port = NetIpPort(port);
    optB.interfaceAddress = iface;
    optB.security = &secReceiver;

    TEST_ASSERT_TRUE(a.open(*net, optA).isOk());
    TEST_ASSERT_TRUE(b.open(*net, optB).isOk());

    const LDataFrame in = makeFrame();
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    auto cemiResult = encodeCemiLData(in, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult.isOk());
    const auto cemiView = std::span<const uint8_t>(cemi.data(), cemiResult.value());

    const auto sendRes = a.sendRoutingIndication(cemiView);
    TEST_ASSERT_TRUE(sendRes.isOk());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);

    std::vector<uint8_t> got;
    bool ok = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::array<uint8_t, kMaxCemiLDataSize> tmp{};
        const auto recvRes = b.receiveRoutingIndication(tmp, 50);
        if (recvRes.isError()) {
            continue;
        }
        if (recvRes.value() == cemiView.size()) {
            ok = true;
            got.assign(tmp.begin(), tmp.begin() + recvRes.value());
            break;
        }
    }

    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT32(cemiView.size(), got.size());
    assert_uint8_array_equal(cemiView, got);

    b.close();
    a.close();
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_routing_multicast_send_receive_secure);
    return UNITY_END();
}

#else

void setUp(void) {}
void tearDown(void) {}

int main()
{
    return 0;
}

#endif
