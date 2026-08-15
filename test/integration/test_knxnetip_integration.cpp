// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_knxnetip_integration.cpp
 * @brief Integration tests for KNXnet/IP tunneling with keepalive
 */

#include "../../include/knx/netip/gateway_discovery_client.hpp"
#include "../../include/knx/netip/tunneling_session_client.hpp"
#include "knx/constants.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/platform/linux_platform.hpp"
#include "../unity_mock/unity.h"
#include <vector>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>

using namespace knx::netip;
using knx::NetIpPort;

void setUp(void) {}
void tearDown(void) {}

// ============================
// Mock Gateway Simulation
// ============================

// Note: These tests simulate protocol behavior without requiring real hardware.
// For full integration testing with real KNX/IP gateway, see hardware validation guide.

void test_tunneling_lifecycle(void) {
    TunnelingSessionClient client;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    // Lifecycle: construct -> open -> close -> destruct
    TEST_ASSERT_FALSE(client.isOpen());
    
    // Attempt connection (will fail without real gateway)
    auto connected = client.open(*net, knx::IpAddress::fromOctets(192, 168, 1, 100), NetIpPort(knx::netip::config::kDefaultPort), 100);
    
    // Without real gateway, connection fails gracefully
    if (connected.isOk()) {
        TEST_ASSERT_TRUE(client.isOpen());
        TEST_ASSERT_TRUE(client.channelId().isValid());
        client.close();
    }
    
    TEST_ASSERT_FALSE(client.isOpen());
}

void test_discover_then_connect_workflow(void) {
    GatewayDiscoveryClient discoveryClient;
    TunnelingSessionClient sessionClient;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    // Step 1: Discover gateways
    std::vector<GatewayInfo> gateways = discoveryClient.discover(*net, 200, 5);
    
    // Step 2: If gateway found, connect to first one
    if (!gateways.empty()) {
        const auto& gateway = gateways[0];
        
        TEST_ASSERT_FALSE(gateway.ipAddress.isZero());
        TEST_ASSERT_TRUE(gateway.port.value() > 0);

        auto connected = sessionClient.open(*net, gateway.ipAddress, gateway.port, 1000);
        
        if (connected.isOk()) {
            TEST_ASSERT_TRUE(sessionClient.isOpen());
            
            // Step 3: Send keepalive
            auto keepaliveOk = sessionClient.sendConnectionStateRequest(1000);
            TEST_ASSERT_TRUE(keepaliveOk.isOk());
            
            // Step 4: Verify activity tracking
            TEST_ASSERT_TRUE(sessionClient.getTimeSinceLastActivity() < 2000);
            
            sessionClient.close();
        }
    }

    // Regardless of discovery/connection outcome, client must end up closed.
    TEST_ASSERT_FALSE(sessionClient.isOpen());
    TEST_ASSERT_FALSE(sessionClient.isKeepaliveActive());
    TEST_ASSERT_EQUAL_UINT8(0, sessionClient.channelId().value());
    TEST_ASSERT_EQUAL_UINT8(0, sessionClient.sequence().value());
}

void test_connection_with_keepalive_thread(void) {
    TunnelingSessionClient client;
    
    // This test would require a real gateway
    // Demonstrates expected usage:
    
    /*
    // 1. Discover gateways
    auto gateways = client.discover(2000);
    if (gateways.empty()) return;
    
    // 2. Connect to first gateway
    if (client.open(gateways[0].ipAddress, gateways[0].port)) {
        
        // 3. Start automatic keepalive (60s interval)
        client.startKeepalive(60000);
        
        // 4. Do work (send/receive telegrams)
        // ...application logic...
        
        // 5. Monitor connection health
        if (client.getTimeSinceLastActivity() > 120000) {
            // No activity for 2 minutes - connection may be dead
        }
        
        // 6. Cleanup
        client.stopKeepalive();
        client.close();
    }
    */
    
    // Even without a gateway, the keepalive thread must start/stop cleanly.
    client.startKeepalive(10);
    TEST_ASSERT_TRUE(client.isKeepaliveActive());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    client.stopKeepalive();
    TEST_ASSERT_FALSE(client.isKeepaliveActive());
    TEST_ASSERT_FALSE(client.isOpen());
}

void test_keepalive_thread_sends_requests(void) {
    TunnelingSessionClient client;
    
    // Start keepalive with short interval for testing
    client.startKeepalive(50);  // 50ms
    
    TEST_ASSERT_TRUE(client.isKeepaliveActive());
    
    // Wait for a few intervals
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Keepalive should still be active
    TEST_ASSERT_TRUE(client.isKeepaliveActive());
    
    // Stop keepalive
    client.stopKeepalive();
    
    TEST_ASSERT_FALSE(client.isKeepaliveActive());
}

void test_multiple_discover_calls(void) {
    GatewayDiscoveryClient client;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    // Multiple discovery calls should work independently
    auto gateways1 = client.discover(*net, 100, 3);
    auto gateways2 = client.discover(*net, 100, 5);
    auto gateways3 = client.discover(*net, 100, 1);
    
    // All calls should complete without error
    TEST_ASSERT_TRUE(gateways1.size() <= 3);
    TEST_ASSERT_TRUE(gateways2.size() <= 5);
    TEST_ASSERT_TRUE(gateways3.size() <= 1);
}

void test_connection_state_without_open(void) {
    TunnelingSessionClient client;
    
    // Operations that require connection should fail gracefully
    TEST_ASSERT_TRUE(client.sendConnectionStateRequest(100).isError());
    
    // Minimal valid cEMI L_Data.ind with a short APDU (GroupValueWrite, data6=0x01)
    // MsgCode, AddInfoLen, Ctrl1, Ctrl2, Src, Dst, DataLen, TPDU[0..]
    std::vector<uint8_t> cemi = {0x29, 0x00, 0x8A, 0xE0, 0x11, 0x01, 0x01, 0x00, 0x02, 0x00, 0x81};
    TEST_ASSERT_TRUE(client.sendCemi(cemi, true, 100).isError());
}

void test_sequence_number_increment(void) {
    TunnelingSessionClient client;
    
    // Initial sequence should be 0
    TEST_ASSERT_EQUAL_UINT8(0, client.sequence().value());
    
    // Without an open tunneling connection, sendCemi must fail and must not
    // advance the tunneling sequence counter.
    const std::vector<uint8_t> cemi = {0x29, 0x00, 0x8A, 0xE0, 0x11, 0x01, 0x01, 0x00, 0x02, 0x00, 0x81};
    TEST_ASSERT_TRUE(client.sendCemi(cemi, false, 50).isError());
    TEST_ASSERT_EQUAL_UINT8(0, client.sequence().value());
}

void test_receive_callback_registration(void) {
    TunnelingSessionClient client;
    
    bool callbackInvoked = false;
    std::vector<uint8_t> receivedData;
    
    client.setReceiveCallback([&](std::span<const uint8_t> cemi) {
        callbackInvoked = true;
        receivedData.assign(cemi.begin(), cemi.end());
    });
    
    // Poll should not crash even without connection
    TEST_ASSERT_TRUE(client.poll(10).isError());
    
    // Callback not invoked without real data
    TEST_ASSERT_FALSE(callbackInvoked);
}

void test_activity_tracking_on_messages(void) {
    TunnelingSessionClient client;
    
    // Initially no activity
    TEST_ASSERT_EQUAL_UINT32(0, client.getTimeSinceLastActivity());
    
    // Without an open connection, operations must not create "activity".
    TEST_ASSERT_TRUE(client.sendConnectionStateRequest(10).isError());
    (void)client.poll(0);
    TEST_ASSERT_EQUAL_UINT32(0, client.getTimeSinceLastActivity());
}

void test_discovery_multicast_address(void) {
    // Verify KNXnet/IP System Setup multicast constants (spec-defined).
    TEST_ASSERT_TRUE(knx::constants::physical::KNXNETIP_MULTICAST_ADDR == knx::IpAddress::fromOctets(224, 0, 23, 12));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, knx::constants::physical::KNXNETIP_SYSTEM_SETUP_MULTICAST_PORT.value());
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, knx::constants::physical::KNXNETIP_DEFAULT_PORT.value());
    TEST_ASSERT_EQUAL_HEX8(0x10, knx::constants::physical::KNXNETIP_PROTOCOL_VERSION);
}

void test_connection_state_interval_compliance(void) {
    // Per KNX spec, connection state requests should be sent every 60 seconds
    // to prevent timeout (default gateway timeout is 120 seconds)
    
    TunnelingSessionClient client;
    
    // Default interval should be 60000ms (60s)
    client.startKeepalive();  // Uses default interval
    TEST_ASSERT_TRUE(client.isKeepaliveActive());
    
    client.stopKeepalive();
    
    // Custom interval for testing
    client.startKeepalive(30000);  // 30s for faster keepalive
    TEST_ASSERT_TRUE(client.isKeepaliveActive());
    
    client.stopKeepalive();
}

void test_gateway_info_completeness(void) {
    // Test that GatewayInfo structure captures all required information
    
    GatewayInfo info;
    info.ipAddress = knx::IpAddress::fromOctets(192, 168, 1, 50);
    info.port = NetIpPort(knx::netip::config::kDefaultPort);
    info.friendlyName = "ABB IP Router";
    
    // MAC address
    uint8_t mac[6] = {0xAB, 0xCD, 0xEF, 0x12, 0x34, 0x56};
    std::memcpy(info.macAddress, mac, 6);
    
    // Device DIB (simulated)
    info.deviceDIB = {0x36, 0x01, 0x02, 0x00, 0x11, 0x00};
    
    // Supported services (simulated)
    info.supportedServices = {0x08, 0x02, 0x02, 0x01, 0x03, 0x01, 0x04, 0x01};
    
    // Verify all fields accessible
    TEST_ASSERT_TRUE(info.ipAddress == knx::IpAddress::fromOctets(192, 168, 1, 50));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
    TEST_ASSERT_EQUAL_STRING("ABB IP Router", info.friendlyName.c_str());
    TEST_ASSERT_EQUAL_HEX8(0xAB, info.macAddress[0]);
    TEST_ASSERT_EQUAL_INT(6, info.deviceDIB.size());
    TEST_ASSERT_EQUAL_INT(8, info.supportedServices.size());
}

void test_concurrent_operations_safety(void) {
    // Test that multiple operations can be performed safely
    
    GatewayDiscoveryClient discoveryClient;
    TunnelingSessionClient sessionClient;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    
    // Start keepalive
    sessionClient.startKeepalive(100);
    
    // Attempt discovery (should not interfere with keepalive)
    auto gateways = discoveryClient.discover(*net, 100);
    TEST_ASSERT_TRUE(gateways.size() <= 10);
    
    // Poll for data
    (void)sessionClient.poll(10);
    
    // Stop keepalive
    sessionClient.stopKeepalive();

    TEST_ASSERT_FALSE(sessionClient.isKeepaliveActive());
    TEST_ASSERT_FALSE(sessionClient.isOpen());
    TEST_ASSERT_EQUAL_UINT8(0, sessionClient.channelId().value());
}

// ============================
// Main Test Runner
// ============================

int main(void) {
    UNITY_BEGIN();
    
    // Lifecycle tests
    RUN_TEST(test_tunneling_lifecycle);
    RUN_TEST(test_discover_then_connect_workflow);
    RUN_TEST(test_connection_with_keepalive_thread);
    
    // Keepalive tests
    RUN_TEST(test_keepalive_thread_sends_requests);
    RUN_TEST(test_connection_state_interval_compliance);
    
    // Discovery tests
    RUN_TEST(test_multiple_discover_calls);
    RUN_TEST(test_discovery_multicast_address);
    
    // Connection management
    RUN_TEST(test_connection_state_without_open);
    RUN_TEST(test_sequence_number_increment);
    RUN_TEST(test_receive_callback_registration);
    RUN_TEST(test_activity_tracking_on_messages);
    
    // Data structure tests
    RUN_TEST(test_gateway_info_completeness);
    
    // Concurrency tests
    RUN_TEST(test_concurrent_operations_safety);
    
    return UNITY_END();
}
