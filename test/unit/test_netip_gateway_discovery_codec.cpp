// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/gateway_discovery_codec.hpp"
#include "knx/netip/netip_config.hpp"

#include <array>
#include <cstring>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::netip;

void setUp(void) {}
void tearDown(void) {}

static std::vector<uint8_t> makeGatewayResponse(NetIpServiceType serviceType)
{
    std::vector<uint8_t> packet;
    packet.reserve(6 + 8 + 54 + 8);

    packet.push_back(0x06);
    packet.push_back(0x10);
    packet.push_back(static_cast<uint8_t>((serviceType.value() >> 8) & 0xFF));
    packet.push_back(static_cast<uint8_t>(serviceType.value() & 0xFF));
    packet.push_back(0x00);
    packet.push_back(0x00);

    packet.insert(packet.end(), {0x08, 0x01, 192, 168, 10, 20, 0x0E, 0x57});

    packet.push_back(54);
    packet.push_back(GatewayDiscoveryCodec::kDeviceInfoDibType);
    packet.push_back(0x02);
    packet.push_back(0x00);
    packet.push_back(0x11);
    packet.push_back(0x0A);
    packet.push_back(0x00);
    packet.push_back(0x01);
    packet.insert(packet.end(), {0, 1, 2, 3, 4, 5});
    packet.insert(packet.end(), {224, 0, 23, 12});
    packet.insert(packet.end(), {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});
    const char name[] = "Codec Gateway";
    for (size_t i = 0; i < 30; ++i) {
        packet.push_back(i < (sizeof(name) - 1) ? static_cast<uint8_t>(name[i]) : 0x00);
    }

    packet.insert(packet.end(), {0x08, GatewayDiscoveryCodec::kSupportedServiceFamiliesDibType, 0x02, 0x01, 0x04, 0x01, 0x00, 0x00});

    const uint16_t totalLen = static_cast<uint16_t>(packet.size());
    packet[4] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
    packet[5] = static_cast<uint8_t>(totalLen & 0xFF);
    return packet;
}

void test_parse_search_response_extracts_gateway_info(void)
{
    GatewayInfo info;
    const auto packet = makeGatewayResponse(control_packet::kServiceSearchResponse);

    auto result = GatewayDiscoveryCodec::parseSearchResponse(packet, info);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(info.ipAddress == IpAddress::fromOctets(192, 168, 10, 20));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
    TEST_ASSERT_EQUAL_STRING("Codec Gateway", info.friendlyName.c_str());
    TEST_ASSERT_EQUAL_HEX8(0xAA, info.macAddress[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, info.macAddress[5]);
    TEST_ASSERT_EQUAL_UINT32(54, static_cast<uint32_t>(info.deviceDIB.size()));
    TEST_ASSERT_EQUAL_UINT32(8, static_cast<uint32_t>(info.supportedServices.size()));
}

void test_parse_description_response_extracts_gateway_info(void)
{
    GatewayInfo info;
    const auto packet = makeGatewayResponse(control_packet::kServiceDescriptionResponse);

    auto result = GatewayDiscoveryCodec::parseDescriptionResponse(packet, info);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(info.ipAddress == IpAddress::fromOctets(192, 168, 10, 20));
    TEST_ASSERT_EQUAL_UINT16(knx::netip::config::kDefaultPort, info.port.value());
    TEST_ASSERT_EQUAL_STRING("Codec Gateway", info.friendlyName.c_str());
}

void test_parse_gateway_response_rejects_non_udp_control_endpoint(void)
{
    GatewayInfo info;
    auto packet = makeGatewayResponse(control_packet::kServiceSearchResponse);
    packet[7] = static_cast<uint8_t>(control_packet::HpaiProtocol::Tcp);

    auto result = GatewayDiscoveryCodec::parseSearchResponse(packet, info);

    TEST_ASSERT_TRUE(result.isError());
}

void test_parse_gateway_response_stops_after_truncated_dib(void)
{
    GatewayInfo info;
    auto packet = makeGatewayResponse(control_packet::kServiceDescriptionResponse);
    packet.resize(6 + 8 + 2);
    packet[14] = 10;
    packet[15] = GatewayDiscoveryCodec::kDeviceInfoDibType;
    const uint16_t totalLen = static_cast<uint16_t>(packet.size());
    packet[4] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
    packet[5] = static_cast<uint8_t>(totalLen & 0xFF);

    auto result = GatewayDiscoveryCodec::parseDescriptionResponse(packet, info);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(info.ipAddress == IpAddress::fromOctets(192, 168, 10, 20));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(info.deviceDIB.size()));
}

void test_gateway_info_initializes_to_safe_defaults(void)
{
    GatewayInfo info;
    TEST_ASSERT_TRUE(info.ipAddress.isZero());
    TEST_ASSERT_FALSE(info.port.isValid());
    TEST_ASSERT_EQUAL_UINT8(0x00, info.macAddress[0]);
    TEST_ASSERT_TRUE(info.friendlyName.empty());
    TEST_ASSERT_TRUE(info.securedServiceFamilies.empty());
}

// --- DIB type 0x06 (Secured Service Families) tests ---

static std::vector<uint8_t> makeGatewayResponseWithSecuredDib(void)
{
    // Build on top of makeGatewayResponse() by appending a DIB 0x06 block.
    // DIB 0x06 layout: len(1) + type(1) + [family(1) + version(1)] * N
    // We declare two entries: family 0x02 (tunnelling) requires secure (version=1),
    //                         family 0x04 (routing) does NOT require secure (version=0).
    auto packet = makeGatewayResponse(control_packet::kServiceSearchResponse);

    const uint8_t dib06[] = {
        0x06,                                              // length (6 bytes total)
        GatewayDiscoveryCodec::kSecuredServiceFamiliesDibType, // 0x06
        0x02, 0x01,                                        // tunnelling: version=1 (secure required)
        0x04, 0x00,                                        // routing:    version=0 (not required)
    };
    packet.insert(packet.end(), std::begin(dib06), std::end(dib06));

    const uint16_t totalLen = static_cast<uint16_t>(packet.size());
    packet[4] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
    packet[5] = static_cast<uint8_t>(totalLen & 0xFF);
    return packet;
}

void test_parse_search_response_captures_secured_service_families_dib(void)
{
    GatewayInfo info;
    const auto packet = makeGatewayResponseWithSecuredDib();

    auto result = GatewayDiscoveryCodec::parseSearchResponse(packet, info);

    TEST_ASSERT_TRUE(result.isOk());
    // DIB 0x06 must be stored: 6 bytes (len + type + 2 entries × 2 bytes)
    TEST_ASSERT_EQUAL_UINT32(6, static_cast<uint32_t>(info.securedServiceFamilies.size()));
    TEST_ASSERT_EQUAL_HEX8(0x06, info.securedServiceFamilies[0]); // length
    TEST_ASSERT_EQUAL_HEX8(GatewayDiscoveryCodec::kSecuredServiceFamiliesDibType,
                           info.securedServiceFamilies[1]);
}

void test_parse_description_response_captures_secured_service_families_dib(void)
{
    // Build a description response that contains DIB 0x06.
    auto packet = makeGatewayResponse(control_packet::kServiceDescriptionResponse);
    const uint8_t dib06[] = {
        0x04,
        GatewayDiscoveryCodec::kSecuredServiceFamiliesDibType,
        0x02, 0x01, // tunnelling secure
    };
    packet.insert(packet.end(), std::begin(dib06), std::end(dib06));
    const uint16_t totalLen = static_cast<uint16_t>(packet.size());
    packet[4] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
    packet[5] = static_cast<uint8_t>(totalLen & 0xFF);

    GatewayInfo info;
    auto result = GatewayDiscoveryCodec::parseDescriptionResponse(packet, info);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(4, static_cast<uint32_t>(info.securedServiceFamilies.size()));
}

void test_parse_response_without_dib06_leaves_secured_families_empty(void)
{
    GatewayInfo info;
    const auto packet = makeGatewayResponse(control_packet::kServiceSearchResponse);

    auto result = GatewayDiscoveryCodec::parseSearchResponse(packet, info);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(info.securedServiceFamilies.empty());
}

void test_parse_response_preserves_both_dib02_and_dib06(void)
{
    GatewayInfo info;
    const auto packet = makeGatewayResponseWithSecuredDib();

    auto result = GatewayDiscoveryCodec::parseSearchResponse(packet, info);

    TEST_ASSERT_TRUE(result.isOk());
    // DIB 0x02 (supported services) must still be populated
    TEST_ASSERT_EQUAL_UINT32(8, static_cast<uint32_t>(info.supportedServices.size()));
    // DIB 0x06 must also be populated
    TEST_ASSERT_EQUAL_UINT32(6, static_cast<uint32_t>(info.securedServiceFamilies.size()));
}

// --- GatewayInfo::requiresSecure() tests ---

void test_requiresSecure_returns_true_when_version_nonzero(void)
{
    GatewayInfo info;
    const auto packet = makeGatewayResponseWithSecuredDib();
    TEST_ASSERT_TRUE(GatewayDiscoveryCodec::parseSearchResponse(packet, info).isOk());

    // Family 0x02 (tunnelling) has version=1 → secure required
    TEST_ASSERT_TRUE(info.requiresSecure(0x02));
}

void test_requiresSecure_returns_false_when_version_zero(void)
{
    GatewayInfo info;
    const auto packet = makeGatewayResponseWithSecuredDib();
    TEST_ASSERT_TRUE(GatewayDiscoveryCodec::parseSearchResponse(packet, info).isOk());

    // Family 0x04 (routing) has version=0 → NOT secure required
    TEST_ASSERT_FALSE(info.requiresSecure(0x04));
}

void test_requiresSecure_returns_false_for_absent_family(void)
{
    GatewayInfo info;
    const auto packet = makeGatewayResponseWithSecuredDib();
    TEST_ASSERT_TRUE(GatewayDiscoveryCodec::parseSearchResponse(packet, info).isOk());

    // Family 0xFF is not listed → must return false, not crash
    TEST_ASSERT_FALSE(info.requiresSecure(0xFF));
}

void test_requiresSecure_returns_false_when_no_dib06_present(void)
{
    GatewayInfo info;
    // No DIB 0x06 in this response
    const auto packet = makeGatewayResponse(control_packet::kServiceSearchResponse);
    TEST_ASSERT_TRUE(GatewayDiscoveryCodec::parseSearchResponse(packet, info).isOk());

    TEST_ASSERT_FALSE(info.requiresSecure(0x02));
    TEST_ASSERT_FALSE(info.requiresSecure(0x04));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_parse_search_response_extracts_gateway_info);
    RUN_TEST(test_parse_description_response_extracts_gateway_info);
    RUN_TEST(test_parse_gateway_response_rejects_non_udp_control_endpoint);
    RUN_TEST(test_parse_gateway_response_stops_after_truncated_dib);
    RUN_TEST(test_gateway_info_initializes_to_safe_defaults);
    RUN_TEST(test_parse_search_response_captures_secured_service_families_dib);
    RUN_TEST(test_parse_description_response_captures_secured_service_families_dib);
    RUN_TEST(test_parse_response_without_dib06_leaves_secured_families_empty);
    RUN_TEST(test_parse_response_preserves_both_dib02_and_dib06);
    RUN_TEST(test_requiresSecure_returns_true_when_version_nonzero);
    RUN_TEST(test_requiresSecure_returns_false_when_version_zero);
    RUN_TEST(test_requiresSecure_returns_false_for_absent_family);
    RUN_TEST(test_requiresSecure_returns_false_when_no_dib06_present);
    return UNITY_END();
}