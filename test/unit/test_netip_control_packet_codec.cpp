// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/netip_config.hpp"

#include <array>
#include <span>

using namespace knx;
using namespace knx::netip;

void setUp(void) {}
void tearDown(void) {}

void test_encode_connection_request_writes_expected_layout(void)
{
    std::array<uint8_t, 64> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};

    auto result = control_packet::Codec::encodeConnectionRequest(
        writer,
        control_packet::HpaiEndpoint{control_packet::HpaiProtocol::Udp, IpAddress::fromOctets(192, 168, 1, 10), knx::netip::config::kDefaultPort},
        control_packet::HpaiEndpoint{control_packet::HpaiProtocol::Udp, IpAddress::fromOctets(192, 168, 1, 10), knx::netip::config::kDefaultPort});

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(26, writer.size());
    TEST_ASSERT_EQUAL_UINT8(0x02, bytes[2]);
    TEST_ASSERT_EQUAL_UINT8(0x05, bytes[3]);
    TEST_ASSERT_EQUAL_UINT8(0x08, bytes[6]);
    TEST_ASSERT_EQUAL_UINT8(0x01, bytes[7]);
    TEST_ASSERT_EQUAL_UINT8(192, bytes[8]);
    TEST_ASSERT_EQUAL_UINT8(168, bytes[9]);
    TEST_ASSERT_EQUAL_UINT8(1, bytes[10]);
    TEST_ASSERT_EQUAL_UINT8(10, bytes[11]);
    TEST_ASSERT_EQUAL_UINT8(0x04, bytes[22]);
    TEST_ASSERT_EQUAL_UINT8(0x04, bytes[23]);
    TEST_ASSERT_EQUAL_UINT8(0x02, bytes[24]);
    TEST_ASSERT_EQUAL_UINT8(0x00, bytes[25]);
}

void test_decode_channel_status_response_returns_channel_and_status(void)
{
    const std::array<uint8_t, 8> frame{0x06, 0x10, 0x02, 0x08, 0x00, 0x08, 0x21, 0x00};

    auto result = control_packet::Codec::decodeChannelStatusResponse(
        frame,
        control_packet::kServiceConnectionStateResponse);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT8(0x21, result.value().channelId);
    TEST_ASSERT_EQUAL_UINT8(0x00, result.value().status);
}

void test_encode_and_decode_tunneling_request_round_trip(void)
{
    std::array<uint8_t, 64> bytes{};
    const std::array<uint8_t, 3> cemi{0x29, 0x00, 0xBC};
    PacketWriter writer{std::span<uint8_t>(bytes)};

    auto encodeResult = control_packet::Codec::encodeTunnelingRequest(writer, 0x31, 0x07, cemi);
    TEST_ASSERT_TRUE(encodeResult.isOk());

    auto decodeResult = control_packet::Codec::decodeTunnelingRequest(writer.span());
    TEST_ASSERT_TRUE(decodeResult.isOk());
    TEST_ASSERT_EQUAL_UINT8(0x31, decodeResult.value().channelId);
    TEST_ASSERT_EQUAL_UINT8(0x07, decodeResult.value().sequence);
    TEST_ASSERT_EQUAL_UINT32(cemi.size(), decodeResult.value().cemi.size());
    TEST_ASSERT_EQUAL_UINT8(0x29, decodeResult.value().cemi[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, decodeResult.value().cemi[1]);
    TEST_ASSERT_EQUAL_UINT8(0xBC, decodeResult.value().cemi[2]);
}

void test_encode_and_decode_tunneling_ack_round_trip(void)
{
    std::array<uint8_t, 32> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};

    auto encodeResult = control_packet::Codec::encodeTunnelingAck(writer, 0x19, 0x44, 0x00);
    TEST_ASSERT_TRUE(encodeResult.isOk());

    auto decodeResult = control_packet::Codec::decodeTunnelingAck(writer.span());
    TEST_ASSERT_TRUE(decodeResult.isOk());
    TEST_ASSERT_EQUAL_UINT8(0x19, decodeResult.value().channelId);
    TEST_ASSERT_EQUAL_UINT8(0x44, decodeResult.value().sequence);
    TEST_ASSERT_EQUAL_UINT8(0x00, decodeResult.value().status);
}

void test_decode_tunneling_request_rejects_invalid_tunneling_header_length(void)
{
    const std::array<uint8_t, 10> frame{0x06, 0x10, 0x04, 0x20, 0x00, 0x0A, 0x03, 0x01, 0x02, 0x00};

    auto result = control_packet::Codec::decodeTunnelingRequest(frame);
    TEST_ASSERT_TRUE(result.isError());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_connection_request_writes_expected_layout);
    RUN_TEST(test_decode_channel_status_response_returns_channel_and_status);
    RUN_TEST(test_encode_and_decode_tunneling_request_round_trip);
    RUN_TEST(test_encode_and_decode_tunneling_ack_round_trip);
    RUN_TEST(test_decode_tunneling_request_rejects_invalid_tunneling_header_length);
    return UNITY_END();
}