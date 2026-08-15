// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/header_codec.hpp"

#include <array>
#include <limits>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::netip;

void setUp(void) {}
void tearDown(void) {}

void test_encode_header_writes_expected_bytes(void)
{
    std::array<uint8_t, KnxNetIpCodec::kHeaderLen> header{};
    auto result = KnxNetIpCodec::encodeHeader(NetIpServiceType(0x0420), 4, header);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(10, result.value());
    TEST_ASSERT_EQUAL_UINT8(0x06, header[0]);
    TEST_ASSERT_EQUAL_UINT8(0x10, header[1]);
    TEST_ASSERT_EQUAL_UINT8(0x04, header[2]);
    TEST_ASSERT_EQUAL_UINT8(0x20, header[3]);
    TEST_ASSERT_EQUAL_UINT8(0x00, header[4]);
    TEST_ASSERT_EQUAL_UINT8(0x0A, header[5]);
}

void test_decode_header_reads_service_and_total_length(void)
{
    const std::array<uint8_t, 8> frame = {0x06, 0x10, 0x02, 0x06, 0x00, 0x08, 0x01, 0x00};
    KnxNetIpHeader header{};

    auto result = KnxNetIpCodec::decodeHeader(frame, header);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT16(0x0206, header.serviceType.value());
    TEST_ASSERT_EQUAL_UINT16(8, header.totalLength);
}

void test_decode_header_rejects_invalid_header_fields(void)
{
    std::vector<uint8_t> frame = {0x05, 0x10, 0x02, 0x06, 0x00, 0x08, 0x01, 0x00};
    KnxNetIpHeader header{};
    TEST_ASSERT_TRUE(KnxNetIpCodec::decodeHeader(std::span<const uint8_t>(frame), header).isError());

    frame[0] = 0x06;
    frame[1] = 0x11;
    TEST_ASSERT_TRUE(KnxNetIpCodec::decodeHeader(std::span<const uint8_t>(frame), header).isError());

    frame[1] = 0x10;
    frame[4] = 0x00;
    frame[5] = 0x05;
    TEST_ASSERT_TRUE(KnxNetIpCodec::decodeHeader(std::span<const uint8_t>(frame), header).isError());
}

void test_payload_span_uses_declared_length_not_datagram_length(void)
{
    const std::vector<uint8_t> frame = {
        0x06, 0x10,
        0x05, 0x30,
        0x00, 0x07,
        0xAA,
        0xBB, 0xCC,
    };

    auto result = KnxNetIpCodec::payloadSpan(std::span<const uint8_t>(frame), NetIpServiceType(0x0530));
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(1, result.value().size());
    TEST_ASSERT_EQUAL_UINT8(0xAA, result.value()[0]);
}

void test_payload_span_rejects_wrong_service_type(void)
{
    const std::vector<uint8_t> frame = {0x06, 0x10, 0x04, 0x20, 0x00, 0x06};
    TEST_ASSERT_TRUE(KnxNetIpCodec::payloadSpan(std::span<const uint8_t>(frame), NetIpServiceType(0x0206)).isError());
}

void test_encode_header_rejects_oversized_payload(void)
{
    std::array<uint8_t, KnxNetIpCodec::kHeaderLen> header{};
    auto result = KnxNetIpCodec::encodeHeader(
        NetIpServiceType(0x0420),
        (std::numeric_limits<uint16_t>::max() - KnxNetIpCodec::kHeaderLen) + 1u,
        header);
    TEST_ASSERT_TRUE(result.isError());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_encode_header_writes_expected_bytes);
    RUN_TEST(test_decode_header_reads_service_and_total_length);
    RUN_TEST(test_decode_header_rejects_invalid_header_fields);
    RUN_TEST(test_payload_span_uses_declared_length_not_datagram_length);
    RUN_TEST(test_payload_span_rejects_wrong_service_type);
    RUN_TEST(test_encode_header_rejects_oversized_payload);
    return UNITY_END();
}