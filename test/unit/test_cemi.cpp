// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"
#include "knx/netip/cemi.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/transport/transport_layer.hpp"

#include <array>
#include <span>

using namespace knx;
using namespace knx::netip;
using namespace knx::datalink;

void setUp(void) {}
void tearDown(void) {}

static void assert_uint8_array_equal(std::span<const uint8_t> expected, std::span<const uint8_t> actual) {
    TEST_ASSERT_EQUAL_UINT32(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); i++) {
        TEST_ASSERT_EQUAL_UINT8(expected[i], actual[i]);
    }
}

static LDataFrame makeFrame() {
    LDataFrame f;
    f.standardFrame = true;
    f.repeated = false;
    f.priority = Priority::Normal;
    f.ackRequested = true;
    f.confirmation = true;
    f.source = IndividualAddress(0x110A); // 1.1.10
    f.destination = GroupAddress(0x2301); // 2/3/1 raw
    f.destinationType = AddressType::Group;
    f.hopCount = 6;
    f.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
    return f;
}

void test_cemi_roundtrip(void) {
    LDataFrame in = makeFrame();
    std::array<uint8_t, kMaxCemiLDataSize> buf{};

    // Use a generic message code (e.g., L_Data.ind = 0x29 or L_Data.req = 0x11)
    const uint8_t msgCode = 0x29;
    auto encoded = encodeCemiLData(in, msgCode, buf);
    TEST_ASSERT_TRUE(encoded.isOk());
    const auto cemi = std::span<const uint8_t>(buf.data(), encoded.value());

    // cEMI L_Data layout (AddInfoLen=0):
    // [0]=MsgCode, [1]=AddInfoLen, [2]=Ctrl1, [3]=Ctrl2, [4..5]=Src, [6..7]=Dst, [8]=DataLen, [9..]=TPDU
    TEST_ASSERT_EQUAL_UINT8(msgCode, cemi[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, cemi[1]);
    // Ctrl1: standard (0x80) + not repeated (0x20) + broadcast (0x10)
    //        + normal priority (0x04) + ack req (0x02) + confirmation (0x01)
    TEST_ASSERT_EQUAL_UINT8(0xB7, cemi[2]);
    // Ctrl2: bit7=group address, bits6..4=hop count, low nibble reserved/0
    TEST_ASSERT_EQUAL_UINT8(0xE0, cemi[3]);
    TEST_ASSERT_EQUAL_UINT8(0x11, cemi[4]);
    TEST_ASSERT_EQUAL_UINT8(0x0A, cemi[5]);
    TEST_ASSERT_EQUAL_UINT8(0x23, cemi[6]);
    TEST_ASSERT_EQUAL_UINT8(0x01, cemi[7]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(in.tpdu.size() - 1), cemi[8]);

    // Ctrl1 0xB7: standard + not repeated + broadcast + normal prio + ack + confirm.
    const uint8_t expected[] = {
        0x29, 0x00, 0xB7, 0xE0,
        0x11, 0x0A,
        0x23, 0x01,
        0x02,
        0x00, 0x80, 0x01
    };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), cemi.size());
    assert_uint8_array_equal(expected, cemi);

    LDataFrame out;
    uint8_t decodedCode = 0;
    TEST_ASSERT_TRUE(decodeCemiLData(cemi, out, decodedCode).isOk());

    // Verify essential fields
    TEST_ASSERT_EQUAL_UINT8(msgCode, decodedCode);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(in.priority), static_cast<uint8_t>(out.priority));
    TEST_ASSERT_EQUAL_UINT8(in.ackRequested, out.ackRequested);
    TEST_ASSERT_EQUAL_UINT8(in.confirmation, out.confirmation);
    TEST_ASSERT_EQUAL_UINT16(in.source.raw, out.source.raw);
    TEST_ASSERT_EQUAL_UINT16(in.destination.raw, out.destination.raw);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(in.destinationType), static_cast<uint8_t>(out.destinationType));
    TEST_ASSERT_EQUAL_UINT8(in.hopCount, out.hopCount);
    TEST_ASSERT_EQUAL_UINT8(in.tpci().raw, out.tpci().raw);
    TEST_ASSERT_EQUAL_UINT16(in.apci().raw, out.apci().raw);
    TEST_ASSERT_EQUAL_UINT8(in.payload().size(), out.payload().size());
    TEST_ASSERT_EQUAL_UINT8(in.payload()[0], out.payload()[0]);
}

void test_cemi_encodes_short_apdu_data6(void) {
    LDataFrame in;
    in.standardFrame = true;
    in.repeated = false;
    in.priority = Priority::Normal;
    in.ackRequested = true;
    in.confirmation = false;
    in.source = IndividualAddress(0x110A);
    in.destination = GroupAddress(0x2301);
    in.destinationType = AddressType::Group;
    in.hopCount = 6;
    in.setTpdu(knx::protocol::TPCI::UnnumberedData,
               application::APCIField::create(application::APCIService::GroupValueWrite, 0x01),
               {});

    std::array<uint8_t, kMaxCemiLDataSize> buf{};
    auto encoded = encodeCemiLData(in, 0x29, buf);
    TEST_ASSERT_TRUE(encoded.isOk());
    const auto cemi = std::span<const uint8_t>(buf.data(), encoded.value());

    // Ctrl1 0xB6: standard + not repeated + broadcast + normal prio + ack req.
    const uint8_t expected[] = {
        0x29, 0x00, 0xB6, 0xE0,
        0x11, 0x0A,
        0x23, 0x01,
        0x01,
        0x00, 0x81
    };
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), cemi.size());
    assert_uint8_array_equal(expected, cemi);
}

void test_cemi_rejects_nonzero_ctrl2_low_nibble_for_standard(void) {
    LDataFrame in = makeFrame();
    std::array<uint8_t, kMaxCemiLDataSize> buf{};

    const uint8_t msgCode = 0x29;
    auto encoded = encodeCemiLData(in, msgCode, buf);
    TEST_ASSERT_TRUE(encoded.isOk());

    // Corrupt reserved low nibble of Ctrl2
    buf[3] |= 0x01;

    LDataFrame out;
    uint8_t decodedCode = 0;
    TEST_ASSERT_TRUE(decodeCemiLData(std::span<const uint8_t>(buf.data(), encoded.value()), out, decodedCode).isError());
}

void test_cemi_rejects_trailing_bytes_beyond_datalen(void) {
    LDataFrame in = makeFrame();
    std::array<uint8_t, kMaxCemiLDataSize + 1> buf{};

    const uint8_t msgCode = 0x29;
    auto encoded = encodeCemiLData(in, msgCode, std::span<uint8_t>(buf.data(), kMaxCemiLDataSize));
    TEST_ASSERT_TRUE(encoded.isOk());

    // Append garbage beyond the TPDU declared by DataLen
    buf[encoded.value()] = 0x00;

    LDataFrame out;
    uint8_t decodedCode = 0;
    TEST_ASSERT_TRUE(decodeCemiLData(std::span<const uint8_t>(buf.data(), encoded.value() + 1), out, decodedCode).isError());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_cemi_roundtrip);
    RUN_TEST(test_cemi_encodes_short_apdu_data6);
    RUN_TEST(test_cemi_rejects_nonzero_ctrl2_low_nibble_for_standard);
    RUN_TEST(test_cemi_rejects_trailing_bytes_beyond_datalen);
    return UNITY_END();
}
