// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_knxnetip_discovery.cpp
 * @brief Unit tests for KNXnet/IP discovery functionality
 */

#include "../../include/knx/netip/gateway_discovery_client.hpp"
#include "../../include/knx/netip/tunneling_session_client.hpp"
#include "knx/constants.hpp"
#include "knx/platform/linux_platform.hpp"
#include "../unity_mock/unity.h"
#include <vector>
#include <string>
#include <cstring>
#include <chrono>

using namespace knx::netip;
using knx::NetIpPort;
#include "knx/netip/netip_config.hpp"

void setUp(void) {}
void tearDown(void) {}

// ============================
// Gateway Discovery Tests
// ============================

void test_discover_creates_socket(void) {
    GatewayDiscoveryClient client;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    // Discover should complete without crashing even if no gateways found
    std::vector<GatewayInfo> gateways = client.discover(*net, 100, 5);
    
    (void)gateways;
}

void test_discover_with_timeout(void) {
    GatewayDiscoveryClient client;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    auto start = std::chrono::steady_clock::now();
    std::vector<GatewayInfo> gateways = client.discover(*net, 200, 10);
    auto end = std::chrono::steady_clock::now();
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // Should complete within reasonable time of timeout
    TEST_ASSERT_TRUE(elapsed <= 500);  // Allow some overhead
}

void test_discover_max_gateways_limit(void) {
    GatewayDiscoveryClient client;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    // Request maximum 3 gateways
    std::vector<GatewayInfo> gateways = client.discover(*net, 100, 3);
    
    // Should not exceed limit
    TEST_ASSERT_TRUE(gateways.size() <= 3);
}

void test_discover_unlimited_gateways(void) {
    GatewayDiscoveryClient client;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    // Request unlimited gateways (0 = no limit)
    std::vector<GatewayInfo> gateways = client.discover(*net, 100, 0);
    
    (void)gateways;
}

void test_parseSearchResponsePacket_parses_dibs(void) {
    GatewayInfo info;

    // Build a minimal SEARCH_RESPONSE packet with:
    // - HPAI control endpoint
    // - Device Info DIB (len=54)
    // - Supported Service Families DIB (len=6)
    std::vector<uint8_t> pkt;
    pkt.reserve(6 + 8 + 54 + 6);

    // Header
    pkt.push_back(0x06);
    pkt.push_back(0x10);
    pkt.push_back(0x02);
    pkt.push_back(0x02); // SEARCH_RESPONSE
    pkt.push_back(0x00);
    pkt.push_back(0x00);

    // HPAI: 192.168.1.100:3671
    pkt.push_back(0x08);
    pkt.push_back(0x01);
    pkt.push_back(192);
    pkt.push_back(168);
    pkt.push_back(1);
    pkt.push_back(100);
    pkt.push_back(0x0E);
    pkt.push_back(0x57);

    // Device Info DIB (len=54, type=0x01)
    pkt.push_back(54);
    pkt.push_back(0x01);
    pkt.push_back(0x02); // medium
    pkt.push_back(0x00); // status
    pkt.push_back(0x11);
    pkt.push_back(0x0A); // IA 1.1.10
    pkt.push_back(0x00);
    pkt.push_back(0x00); // proj-inst
    // serial (6)
    pkt.insert(pkt.end(), {0x10, 0x20, 0x30, 0x40, 0x50, 0x60});
    // multicast (4)
    pkt.insert(pkt.end(), {224, 0, 23, 12});
    // MAC (6)
    pkt.insert(pkt.end(), {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01});
    // Friendly name (30)
    {
        const char* name = "KNX Test Gateway";
        std::vector<uint8_t> fn(30, 0);
        std::memcpy(fn.data(), name, std::strlen(name));
        pkt.insert(pkt.end(), fn.begin(), fn.end());
    }

    // Supported Service Families DIB (len=6, type=0x02) with one entry (family=0x02, ver=0x01)
    pkt.push_back(6);
    pkt.push_back(0x02);
    pkt.push_back(0x02);
    pkt.push_back(0x01);
    pkt.push_back(0x00);
    pkt.push_back(0x00);

    // Fill total length
    uint16_t totalLen = static_cast<uint16_t>(pkt.size());
    pkt[4] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
    pkt[5] = static_cast<uint8_t>(totalLen & 0xFF);

    auto result = GatewayDiscoveryClient::parseSearchResponsePacket(std::span<const uint8_t>(pkt), info);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(info.ipAddress == knx::IpAddress::fromOctets(192, 168, 1, 100));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
    TEST_ASSERT_EQUAL_HEX8(0xDE, info.macAddress[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, info.macAddress[5]);
    TEST_ASSERT_EQUAL_STRING("KNX Test Gateway", info.friendlyName.c_str());
    TEST_ASSERT_EQUAL_UINT32(54, static_cast<uint32_t>(info.deviceDIB.size()));
    TEST_ASSERT_EQUAL_UINT32(6, static_cast<uint32_t>(info.supportedServices.size()));
    TEST_ASSERT_EQUAL_HEX8(0x02, info.supportedServices[1]); // DIB type
}

void test_gatewayinfo_structure(void) {
    GatewayInfo info;
    
    // Initialize fields
    info.ipAddress = knx::IpAddress::fromOctets(192, 168, 1, 100);
    info.port = NetIpPort(knx::netip::config::kDefaultPort);
    info.friendlyName = "Test Gateway";
    uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    std::memcpy(info.macAddress, mac, 6);
    
    // Verify structure
    TEST_ASSERT_TRUE(info.ipAddress == knx::IpAddress::fromOctets(192, 168, 1, 100));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
    TEST_ASSERT_EQUAL_STRING("Test Gateway", info.friendlyName.c_str());
    TEST_ASSERT_EQUAL_HEX8(0x01, info.macAddress[0]);
    TEST_ASSERT_EQUAL_HEX8(0x06, info.macAddress[5]);
}

// ============================
// Description Tests
// ============================

void test_getDescription_invalid_host(void) {
    GatewayDiscoveryClient client;
    GatewayInfo info;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    // Invalid host should fail gracefully
    auto result = client.getDescription(*net, knx::IpAddress::fromString("invalid.host.test"), NetIpPort(knx::netip::config::kDefaultPort), 100, info);

    TEST_ASSERT_TRUE(result.isError());
}

void test_getDescription_timeout(void) {
    GatewayDiscoveryClient client;
    GatewayInfo info;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    auto start = std::chrono::steady_clock::now();
    auto result = client.getDescription(*net, knx::IpAddress::fromOctets(192, 168, 255, 255), NetIpPort(knx::netip::config::kDefaultPort), 100, info);
    auto end = std::chrono::steady_clock::now();
    
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    // Should timeout gracefully (allow more time for DNS/socket ops)
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_TRUE(elapsed <= 1000);  // Allow 1s overhead
}

void test_parseDescriptionResponsePacket_parses_dibs(void) {
    GatewayInfo info;

    // Build a minimal DESCRIPTION_RESPONSE packet with:
    // - HPAI control endpoint
    // - Device Info DIB (54 bytes)
    // - Supported Service Families DIB
    std::vector<uint8_t> pkt;
    pkt.reserve(6 + 8 + 54 + 6);

    // Header placeholder
    pkt.push_back(0x06);
    pkt.push_back(0x10);
    pkt.push_back(0x02);
    pkt.push_back(0x04); // DESCRIPTION_RESPONSE
    pkt.push_back(0x00);
    pkt.push_back(0x00);

    // HPAI: 192.168.1.100:3671
    pkt.push_back(0x08);
    pkt.push_back(0x01);
    pkt.push_back(192);
    pkt.push_back(168);
    pkt.push_back(1);
    pkt.push_back(100);
    pkt.push_back(0x0E);
    pkt.push_back(0x57);

    // Device Info DIB (len=54, type=0x01)
    pkt.push_back(54);
    pkt.push_back(0x01);
    pkt.push_back(0x02); // medium
    pkt.push_back(0x00); // status
    pkt.push_back(0x11);
    pkt.push_back(0x0A); // IA 1.1.10
    pkt.push_back(0x00);
    pkt.push_back(0x00); // proj-inst
    // serial (6)
    pkt.insert(pkt.end(), {0x10, 0x20, 0x30, 0x40, 0x50, 0x60});
    // multicast (4)
    pkt.insert(pkt.end(), {224, 0, 23, 12});
    // MAC (6)
    pkt.insert(pkt.end(), {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01});
    // Friendly name (30)
    {
        const char* name = "KNX Test Gateway";
        std::vector<uint8_t> fn(30, 0);
        std::memcpy(fn.data(), name, std::strlen(name));
        pkt.insert(pkt.end(), fn.begin(), fn.end());
    }

    // Supported Service Families DIB (len=6, type=0x02) with one entry (family=0x02, ver=0x01)
    pkt.push_back(6);
    pkt.push_back(0x02);
    pkt.push_back(0x02);
    pkt.push_back(0x01);
    pkt.push_back(0x00);
    pkt.push_back(0x00);

    // Fill total length
    uint16_t totalLen = static_cast<uint16_t>(pkt.size());
    pkt[4] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
    pkt[5] = static_cast<uint8_t>(totalLen & 0xFF);

    auto result = GatewayDiscoveryClient::parseDescriptionResponsePacket(std::span<const uint8_t>(pkt), info);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(info.ipAddress == knx::IpAddress::fromOctets(192, 168, 1, 100));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
    TEST_ASSERT_EQUAL_HEX8(0xDE, info.macAddress[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01, info.macAddress[5]);
    TEST_ASSERT_EQUAL_STRING("KNX Test Gateway", info.friendlyName.c_str());
    TEST_ASSERT_EQUAL_UINT32(54, static_cast<uint32_t>(info.deviceDIB.size()));
    TEST_ASSERT_EQUAL_UINT32(6, static_cast<uint32_t>(info.supportedServices.size()));
    TEST_ASSERT_EQUAL_HEX8(0x02, info.supportedServices[1]); // DIB type
 }

void test_parseDescriptionResponsePacket_rejects_wrong_service_type(void) {
    GatewayInfo info;
    std::vector<uint8_t> pkt(6 + 8, 0);
    pkt[0] = 0x06;
    pkt[1] = 0x10;
    pkt[2] = 0x02;
    pkt[3] = 0x02; // SEARCH_RESPONSE (wrong)
    pkt[4] = 0x00;
    pkt[5] = static_cast<uint8_t>(pkt.size());
    // HPAI
    pkt[6] = 0x08;
    pkt[7] = 0x01;

    auto result = GatewayDiscoveryClient::parseDescriptionResponsePacket(std::span<const uint8_t>(pkt), info);
    TEST_ASSERT_TRUE(result.isError());
}

void test_parseDescriptionResponsePacket_rejects_length_mismatch(void) {
    GatewayInfo info;
    std::vector<uint8_t> pkt(6 + 8, 0);
    pkt[0] = 0x06;
    pkt[1] = 0x10;
    pkt[2] = 0x02;
    pkt[3] = 0x04; // DESCRIPTION_RESPONSE
    // total length claims more than we actually provide
    pkt[4] = 0x01;
    pkt[5] = 0x00;
    // HPAI
    pkt[6] = 0x08;
    pkt[7] = 0x01;

    auto result = GatewayDiscoveryClient::parseDescriptionResponsePacket(std::span<const uint8_t>(pkt), info);
    TEST_ASSERT_TRUE(result.isError());
}

void test_parseDescriptionResponsePacket_rejects_truncated_hpai(void) {
    GatewayInfo info;
    // Header + partial HPAI
    std::vector<uint8_t> pkt(6 + 3, 0);
    pkt[0] = 0x06;
    pkt[1] = 0x10;
    pkt[2] = 0x02;
    pkt[3] = 0x04; // DESCRIPTION_RESPONSE
    pkt[4] = 0x00;
    pkt[5] = static_cast<uint8_t>(pkt.size());
    pkt[6] = 0x08; // claims full HPAI
    pkt[7] = 0x01;

    auto result = GatewayDiscoveryClient::parseDescriptionResponsePacket(std::span<const uint8_t>(pkt), info);
    TEST_ASSERT_TRUE(result.isError());
}

void test_parseDescriptionResponsePacket_ignores_truncated_dib_but_parses_hpai(void) {
    GatewayInfo info;
    std::vector<uint8_t> pkt;
    pkt.reserve(6 + 8 + 2);
    // Header
    pkt.push_back(0x06);
    pkt.push_back(0x10);
    pkt.push_back(0x02);
    pkt.push_back(0x04); // DESCRIPTION_RESPONSE
    pkt.push_back(0x00);
    pkt.push_back(0x00);
    // HPAI 10.0.0.1:3671
    pkt.push_back(0x08);
    pkt.push_back(0x01);
    pkt.push_back(10);
    pkt.push_back(0);
    pkt.push_back(0);
    pkt.push_back(1);
    pkt.push_back(0x0E);
    pkt.push_back(0x57);
    // Start a DIB but truncate it (len=10, only provide 2 bytes)
    pkt.push_back(10);
    pkt.push_back(0x01);

    uint16_t totalLen = static_cast<uint16_t>(pkt.size());
    pkt[4] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
    pkt[5] = static_cast<uint8_t>(totalLen & 0xFF);

    auto result = GatewayDiscoveryClient::parseDescriptionResponsePacket(std::span<const uint8_t>(pkt), info);
    // Parser should accept the packet and just stop DIB parsing.
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(info.ipAddress == knx::IpAddress::fromOctets(10, 0, 0, 1));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
        TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
        TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(info.deviceDIB.size()));
}

void test_parseSearchResponsePacket_rejects_wrong_version(void) {
    GatewayInfo info;
    std::vector<uint8_t> pkt(6 + 8, 0);
    pkt[0] = 0x06;
    pkt[1] = 0x11; // wrong version
    pkt[2] = 0x02;
    pkt[3] = 0x02; // SEARCH_RESPONSE
    pkt[4] = 0x00;
    pkt[5] = static_cast<uint8_t>(pkt.size());
    pkt[6] = 0x08;
    pkt[7] = 0x01;

    auto result = GatewayDiscoveryClient::parseSearchResponsePacket(std::span<const uint8_t>(pkt), info);
    TEST_ASSERT_TRUE(result.isError());
}

// ============================
// Connection State Tests
// ============================

void test_sendConnectionStateRequest_not_connected(void) {
    TunnelingSessionClient client;
    
    // Should fail if not connected
    auto result = client.sendConnectionStateRequest(100);
    
    TEST_ASSERT_TRUE(result.isError());
}

void test_getTimeSinceLastActivity_initial(void) {
    TunnelingSessionClient client;
    
    // Initially should be 0 (no activity yet)
    uint32_t elapsed = client.getTimeSinceLastActivity();
    
    TEST_ASSERT_EQUAL_UINT32(0, elapsed);
}

void test_keepalive_initial_state(void) {
    TunnelingSessionClient client;
    
    // Keepalive should be inactive initially
    TEST_ASSERT_FALSE(client.isKeepaliveActive());
}

void test_keepalive_start_stop(void) {
    TunnelingSessionClient client;
    
    // Start keepalive
    client.startKeepalive(100);  // 100ms for testing
    TEST_ASSERT_TRUE(client.isKeepaliveActive());
    
    // Stop keepalive
    client.stopKeepalive();
    TEST_ASSERT_FALSE(client.isKeepaliveActive());
}

void test_keepalive_prevents_double_start(void) {
    TunnelingSessionClient client;
    
    client.startKeepalive(100);
    TEST_ASSERT_TRUE(client.isKeepaliveActive());
    
    // Second start should be ignored
    client.startKeepalive(200);
    TEST_ASSERT_TRUE(client.isKeepaliveActive());
    
    client.stopKeepalive();
}

void test_keepalive_cleanup_on_close(void) {
    TunnelingSessionClient client;
    
    client.startKeepalive(100);
    TEST_ASSERT_TRUE(client.isKeepaliveActive());
    
    // Close should stop keepalive
    client.close();
    TEST_ASSERT_FALSE(client.isKeepaliveActive());
}

void test_keepalive_cleanup_on_destructor(void) {
    const auto start = std::chrono::steady_clock::now();
    {
        TunnelingSessionClient client;
        client.startKeepalive(10);
        TEST_ASSERT_TRUE(client.isKeepaliveActive());
        // Destructor called here; must join keepalive thread promptly.
    }
    const auto end = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    TEST_ASSERT_TRUE(elapsed < 500);
}

// ============================
// Integration Test Stub
// ============================

void test_discovery_integration_notes(void) {
    // Spec constants used by discovery.
    TEST_ASSERT_TRUE(knx::constants::physical::KNXNETIP_MULTICAST_ADDR == knx::IpAddress::fromOctets(224, 0, 23, 12));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, knx::constants::physical::KNXNETIP_SYSTEM_SETUP_MULTICAST_PORT.value());
        TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, knx::constants::physical::KNXNETIP_SYSTEM_SETUP_MULTICAST_PORT.value());
}

// ============================
// Main Test Runner
// ============================

int main(void) {
    UNITY_BEGIN();
    
    // Discovery tests
    RUN_TEST(test_discover_creates_socket);
    RUN_TEST(test_discover_with_timeout);
    RUN_TEST(test_discover_max_gateways_limit);
    RUN_TEST(test_discover_unlimited_gateways);
    RUN_TEST(test_gatewayinfo_structure);
    
    // Description tests
    RUN_TEST(test_getDescription_invalid_host);
    RUN_TEST(test_getDescription_timeout);
    RUN_TEST(test_parseDescriptionResponsePacket_parses_dibs);
    RUN_TEST(test_parseDescriptionResponsePacket_rejects_wrong_service_type);
    RUN_TEST(test_parseDescriptionResponsePacket_rejects_length_mismatch);
    RUN_TEST(test_parseDescriptionResponsePacket_rejects_truncated_hpai);
    RUN_TEST(test_parseDescriptionResponsePacket_ignores_truncated_dib_but_parses_hpai);
    RUN_TEST(test_parseSearchResponsePacket_rejects_wrong_version);
    RUN_TEST(test_parseSearchResponsePacket_parses_dibs);
    
    // Connection state tests
    RUN_TEST(test_sendConnectionStateRequest_not_connected);
    RUN_TEST(test_getTimeSinceLastActivity_initial);
    
    // Keepalive tests
    RUN_TEST(test_keepalive_initial_state);
    RUN_TEST(test_keepalive_start_stop);
    RUN_TEST(test_keepalive_prevents_double_start);
    RUN_TEST(test_keepalive_cleanup_on_close);
    RUN_TEST(test_keepalive_cleanup_on_destructor);
    
    // Integration notes
    RUN_TEST(test_discovery_integration_notes);
    
    return UNITY_END();
}
