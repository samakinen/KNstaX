// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"
#include "knx/physical/ip_physical_layer.hpp"
#include "knx/platform/linux_platform.hpp"
#include <vector>
#include <cstring>
#include <span>

using namespace knx::physical;

void setUp(void) {}
void tearDown(void) {}

void test_ip_physical_bind_and_multicast(void) {
    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpPhysical ip;
    ip.setNetworkInterface(net);
    // Bind to loopback on a non-default port
    const knx::IpAddress loopback = knx::IpAddress::fromOctets(127, 0, 0, 1);
    ip.setBindAddressPort(loopback, 47808); // BACnet default port used as arbitrary
    ip.setMulticastInterface(loopback); // use loopback interface
    TEST_ASSERT_TRUE(ip.init());
    TEST_ASSERT_TRUE(ip.isOpen());

    const char* msg = "cfg-knx";
    size_t len = std::strlen(msg);

    // Send to the configured bind port to loopback
    TEST_ASSERT_TRUE(ip.beginTransmit(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(msg), len), loopback, 47808).isOk());
    auto sendProgress = ip.pollTransmit();
    TEST_ASSERT_TRUE(sendProgress.isOk());
    TEST_ASSERT_EQUAL(IpPhysical::ProgressState::Success, sendProgress.value());

    // Receive the datagram
    TEST_ASSERT_TRUE(ip.beginReceive(200).isOk());
    auto receiveProgress = ip.pollReceive();
    while (receiveProgress.isOk() && receiveProgress.value() == IpPhysical::ProgressState::Pending) {
        receiveProgress = ip.pollReceive();
    }
    TEST_ASSERT_TRUE(receiveProgress.isOk());
    TEST_ASSERT_EQUAL(IpPhysical::ProgressState::Complete, receiveProgress.value());
    auto recvRes = ip.receivedFrameView();
    TEST_ASSERT_TRUE(recvRes.isOk());
    const auto buf = recvRes.value();
    TEST_ASSERT_TRUE(buf.size() == len);
    TEST_ASSERT_TRUE(std::memcmp(msg, buf.data(), len) == 0);

    ip.close();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ip_physical_bind_and_multicast);
    return UNITY_END();
}
