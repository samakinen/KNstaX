// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/gateway_discovery_client.hpp"
#include "knx/platform/linux_platform.hpp"

#include <chrono>
#include <cstring>
#include <span>
#include <vector>

using namespace knx::netip;
using knx::NetIpPort;

void setUp(void) {}
void tearDown(void) {}

void test_gateway_discovery_client_discover_timeout_returns_empty(void)
{
    GatewayDiscoveryClient client;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    const auto start = std::chrono::steady_clock::now();
    auto gateways = client.discover(*net, 100, 3);
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    TEST_ASSERT_TRUE(gateways.empty());
    TEST_ASSERT_TRUE(elapsed <= 500);
}

void test_gateway_discovery_client_async_discover_times_out_cleanly(void)
{
    GatewayDiscoveryClient client;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    TEST_ASSERT_TRUE(client.beginDiscover(*net, 100, 3).isOk());
    TEST_ASSERT_TRUE(client.isDiscovering());

    bool sawPending = false;
    bool sawTimeout = false;
    for (int attempt = 0; attempt < 100 && !sawTimeout; ++attempt) {
        auto progress = client.pollDiscover();
        TEST_ASSERT_TRUE(progress.isOk());
        if (progress.value() == knx::util::OperationProgressState::Pending) {
            sawPending = true;
        }
        if (progress.value() == knx::util::OperationProgressState::Timeout) {
            sawTimeout = true;
        }
        if (!sawTimeout) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    TEST_ASSERT_TRUE(sawPending);
    TEST_ASSERT_TRUE(sawTimeout);
    TEST_ASSERT_FALSE(client.isDiscovering());
    TEST_ASSERT_TRUE(client.discoveredGateways().empty());
}

void test_gateway_discovery_client_get_description_rejects_invalid_host(void)
{
    GatewayDiscoveryClient client;
    GatewayInfo info;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    auto result = client.getDescription(*net,
                                        knx::IpAddress::fromString("invalid.host.test"),
                                        NetIpPort(knx::netip::config::kDefaultPort),
                                        100,
                                        info);

    TEST_ASSERT_TRUE(result.isError());
}

void test_gateway_discovery_client_parses_search_response_packet(void)
{
    GatewayInfo info;
    std::vector<uint8_t> pkt;
    pkt.reserve(6 + 8 + 54 + 6);

    pkt.push_back(0x06);
    pkt.push_back(0x10);
    pkt.push_back(0x02);
    pkt.push_back(0x02);
    pkt.push_back(0x00);
    pkt.push_back(0x00);

    pkt.push_back(0x08);
    pkt.push_back(0x01);
    pkt.push_back(192);
    pkt.push_back(168);
    pkt.push_back(1);
    pkt.push_back(100);
    pkt.push_back(0x0E);
    pkt.push_back(0x57);

    pkt.push_back(54);
    pkt.push_back(0x01);
    pkt.push_back(0x02);
    pkt.push_back(0x00);
    pkt.push_back(0x11);
    pkt.push_back(0x0A);
    pkt.push_back(0x00);
    pkt.push_back(0x00);
    pkt.insert(pkt.end(), {0x10, 0x20, 0x30, 0x40, 0x50, 0x60});
    pkt.insert(pkt.end(), {224, 0, 23, 12});
    pkt.insert(pkt.end(), {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01});

    {
        const char* name = "KNX Test Gateway";
        std::vector<uint8_t> friendlyName(30, 0);
        std::memcpy(friendlyName.data(), name, std::strlen(name));
        pkt.insert(pkt.end(), friendlyName.begin(), friendlyName.end());
    }

    pkt.push_back(6);
    pkt.push_back(0x02);
    pkt.push_back(0x02);
    pkt.push_back(0x01);
    pkt.push_back(0x00);
    pkt.push_back(0x00);

    const uint16_t totalLen = static_cast<uint16_t>(pkt.size());
    pkt[4] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
    pkt[5] = static_cast<uint8_t>(totalLen & 0xFF);

    auto result = GatewayDiscoveryClient::parseSearchResponsePacket(std::span<const uint8_t>(pkt), info);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(info.ipAddress == knx::IpAddress::fromOctets(192, 168, 1, 100));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
    TEST_ASSERT_EQUAL_STRING("KNX Test Gateway", info.friendlyName.c_str());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gateway_discovery_client_discover_timeout_returns_empty);
    RUN_TEST(test_gateway_discovery_client_async_discover_times_out_cleanly);
    RUN_TEST(test_gateway_discovery_client_get_description_rejects_invalid_host);
    RUN_TEST(test_gateway_discovery_client_parses_search_response_packet);
    return UNITY_END();
}