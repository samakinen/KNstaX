// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tp1_frame_golden.cpp
 * @brief Golden TP1 frame encoding test
 *
 * Expected bytes are anchored to KNX 03_02_02 (Communication Medium TP1):
 * control field layout §2.2.2 / §2.3.2 table: `FT 0 r 1 P1 P0 x x` with
 * r = 1 for a NOT repeated frame and priority codes 00=system, 01=normal,
 * 10=urgent, 11=low. A first-transmission normal-priority L_Data_Standard
 * frame therefore carries CTRL 1011 01xx.
 */

#include "unity.h"

#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/transport/transport_layer.hpp"

#include <span>

using namespace knx;
using namespace knx::datalink;
using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

void test_tp1_frame_golden_group_value_write_short(void)
{
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;   // first transmission → wire r-bit = 1
    frame.priority = Priority::Normal;
    frame.ackRequested = false;
    frame.confirmation = false;
    frame.source = IndividualAddress(1, 1, 10); // 0x110A
    frame.destination = GroupAddress(2, 3, 1);  // 0x1301
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;

    const auto apci = APCIField::create(APCIService::GroupValueWrite, 0x01);
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, apci, {});

    uint8_t buffer[23] = {0};
    auto enc = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(enc.isOk());

    const uint8_t expected[] = {
        0xB4,       // CTRL: 1011 0100 = standard, not repeated, normal priority
        0x11, 0x0A, // SRC: 1.1.10
        0x13, 0x01, // DST: group 2/3/1 (0x1301)
        0xE1,       // LEN: group + hopcount=6 + (tpduLen - 1) = 1
        0x00, 0x81, // TPDU: TPCI=0x00, APCI=0x081 (short write, data=1)
        0x22        // CHECKSUM (XOR of all preceding bytes, inverted)
    };

    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), enc.value());
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_UINT8(expected[i], buffer[i]);
    }
}

void test_tp1_frame_golden_system_priority_not_repeated(void)
{
    // Matches the on-bus confirmed encoding of transport control frames
    // (e.g. T_ACK responses during ETS commissioning): CTRL 0xB0 =
    // L_Data_Standard, not repeated, system priority (03_02_02 §2.3.2 table).
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::System;
    frame.ackRequested = false;
    frame.confirmation = false;
    frame.source = IndividualAddress(1, 1, 3);       // 0x1103
    frame.destination.raw = IndividualAddress(1, 0, 0).raw; // 0x1000
    frame.destinationType = AddressType::Individual;
    frame.hopCount = 6;
    frame.tpdu = {0xC2};  // T_ACK seq 0

    uint8_t buffer[23] = {0};
    auto enc = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(enc.isOk());
    TEST_ASSERT_EQUAL_UINT8(0xB0, buffer[0]);

    // Round-trip: decode must restore the logical flags.
    LDataFrame decoded;
    auto dec = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer, enc.value()), decoded);
    TEST_ASSERT_TRUE(dec.isOk());
    TEST_ASSERT_FALSE(decoded.repeated);
    TEST_ASSERT_TRUE(decoded.priority == Priority::System);
}

void test_tp1_frame_golden_repeated_low_priority(void)
{
    // Repeated low-priority frame: CTRL 1001 1100 = 0x9C (r-bit cleared).
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = true;    // repetition → wire r-bit = 0
    frame.priority = Priority::Low;
    frame.ackRequested = false;
    frame.confirmation = false;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(2, 3, 1);
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;

    const auto apci = APCIField::create(APCIService::GroupValueWrite, 0x01);
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, apci, {});

    uint8_t buffer[23] = {0};
    auto enc = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(enc.isOk());
    TEST_ASSERT_EQUAL_UINT8(0x9C, buffer[0]);

    LDataFrame decoded;
    auto dec = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer, enc.value()), decoded);
    TEST_ASSERT_TRUE(dec.isOk());
    TEST_ASSERT_TRUE(decoded.repeated);
    TEST_ASSERT_TRUE(decoded.priority == Priority::Low);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tp1_frame_golden_group_value_write_short);
    RUN_TEST(test_tp1_frame_golden_system_priority_not_repeated);
    RUN_TEST(test_tp1_frame_golden_repeated_low_priority);
    return UNITY_END();
}
