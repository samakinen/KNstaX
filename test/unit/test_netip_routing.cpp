// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/routing.hpp"
#include "knx/netip/cemi.hpp"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"

#include <cstdint>
#include <array>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::netip;
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

void test_routing_indication_roundtrip_cemi_bytes(void)
{
    const LDataFrame in = makeFrame();
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    auto cemiResult = encodeCemiLData(in, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult.isOk());
    const auto cemiView = std::span<const uint8_t>(cemi.data(), cemiResult.value());

    std::vector<uint8_t> pkt(RoutingCodec::KNXNETIP_HEADER_LEN + cemiView.size());
    auto pktResult = RoutingCodec::encodeRoutingIndication(cemiView, pkt);
    TEST_ASSERT_TRUE(pktResult.isOk());
    pkt.resize(pktResult.value());

    // Validate header fields
    TEST_ASSERT_EQUAL_UINT8(RoutingCodec::KNXNETIP_HEADER_LEN, pkt[0]);
    TEST_ASSERT_EQUAL_UINT8(RoutingCodec::KNXNETIP_VERSION, pkt[1]);
    TEST_ASSERT_EQUAL_UINT8(0x05, pkt[2]);
    TEST_ASSERT_EQUAL_UINT8(0x30, pkt[3]);

    const uint16_t totalLen = static_cast<uint16_t>(pkt.size());
    TEST_ASSERT_EQUAL_UINT8((totalLen >> 8) & 0xFF, pkt[4]);
    TEST_ASSERT_EQUAL_UINT8(totalLen & 0xFF, pkt[5]);

    auto decoded = RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(pkt));
    TEST_ASSERT_TRUE(decoded.isOk());

    TEST_ASSERT_EQUAL_UINT32(cemiView.size(), decoded.value().size());
    assert_uint8_array_equal(cemiView, decoded.value());
}

void test_routing_indication_rejects_invalid_header(void)
{
    std::vector<uint8_t> pkt = {
        0x05, 0x10, // wrong header length
        0x05, 0x30,
        0x00, 0x06,
    };

    TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(pkt)).isError());

    pkt[0] = 0x06;
    pkt[1] = 0x11; // wrong version
    TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(pkt)).isError());

    pkt[1] = 0x10;
    pkt[4] = 0x00;
    pkt[5] = 0x05; // total length too small
    TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(pkt)).isError());
}

void test_routing_indication_rejects_wrong_service_type(void)
{
    std::vector<uint8_t> pkt = {
        0x06, 0x10,
        0x04, 0x20, // tunneling request
        0x00, 0x06,
    };

    TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(pkt)).isError());
}

void test_routing_indication_uses_total_length_not_datagram_length(void)
{
    // A routing indication with a 1-byte cEMI payload, but with trailing bytes.
    std::vector<uint8_t> pkt = {
        0x06, 0x10,
        0x05, 0x30,
        0x00, 0x07,
        0xAA,
        0xBB, 0xCC
    };

    auto decoded = RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(pkt));
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL_UINT32(1, decoded.value().size());
    TEST_ASSERT_EQUAL_UINT8(0xAA, decoded.value()[0]);
}

void test_routing_lost_message_decodes_counter(void)
{
    std::vector<uint8_t> pkt = {
        0x06, 0x10,
        0x05, 0x31,
        0x00, 0x08,
        0x00, 0x05,
    };

    uint16_t lost = 0;
    TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingLostMessage(std::span<const uint8_t>(pkt), lost).isOk());
    TEST_ASSERT_EQUAL_UINT16(5, lost);
}

void test_routing_lost_message_encodes_expected_bytes(void)
{
    std::array<uint8_t, RoutingCodec::ROUTING_LOST_MESSAGE_FRAME_LEN> pkt{};
    TEST_ASSERT_TRUE(RoutingCodec::encodeRoutingLostMessage(5, pkt).isOk());

    const std::vector<uint8_t> expected = {
        0x06, 0x10,
        0x05, 0x31,
        0x00, 0x08,
        0x00, 0x05,
    };
    TEST_ASSERT_EQUAL_UINT32(expected.size(), pkt.size());
    assert_uint8_array_equal(expected, pkt);

    uint16_t lost = 0;
    TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingLostMessage(std::span<const uint8_t>(pkt), lost).isOk());
    TEST_ASSERT_EQUAL_UINT16(5, lost);
}

void test_routing_lost_message_rejects_wrong_length(void)
{
    std::vector<uint8_t> pkt = {
        0x06, 0x10,
        0x05, 0x31,
        0x00, 0x09, // wrong total length
        0x00, 0x05,
        0x00,
    };

    uint16_t lost = 0;
    TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingLostMessage(std::span<const uint8_t>(pkt), lost).isError());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_routing_indication_roundtrip_cemi_bytes);
    RUN_TEST(test_routing_indication_rejects_invalid_header);
    RUN_TEST(test_routing_indication_rejects_wrong_service_type);
    RUN_TEST(test_routing_indication_uses_total_length_not_datagram_length);
    RUN_TEST(test_routing_lost_message_decodes_counter);
    RUN_TEST(test_routing_lost_message_encodes_expected_bytes);
    RUN_TEST(test_routing_lost_message_rejects_wrong_length);
    return UNITY_END();
}
