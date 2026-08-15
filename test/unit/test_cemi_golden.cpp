// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_cemi_golden.cpp
 * @brief Golden cEMI L_Data encoding test
 */

#include "unity.h"

#include "knx/netip/cemi.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/protocol/tpdu_codec.hpp"

#include <array>

using namespace knx;
using namespace knx::datalink;
using namespace knx::application;
using namespace knx::netip;

void setUp(void) {}
void tearDown(void) {}

void test_cemi_l_data_golden_group_value_write_short(void)
{
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Normal;
    frame.ackRequested = true;
    frame.confirmation = false;
    frame.source = IndividualAddress(1, 1, 10); // 0x110A
    frame.destination = GroupAddress(2, 3, 1);  // 0x1301
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;

    const auto apci = APCIField::create(APCIService::GroupValueWrite, 0x01);
    frame.tpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        apci,
        {}
    );

    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    auto encoded = encodeCemiLData(frame, 0x29, cemi);
    TEST_ASSERT_TRUE(encoded.isOk());
    const auto cemiView = std::span<const uint8_t>(cemi.data(), encoded.value());

    const uint8_t expected[] = {
        0x29, // L_Data.ind
        0x00, // AddInfoLen
        0x8A, // Ctrl1: standard + normal priority + ack req
        0xE0, // Ctrl2: group + hopcount=6
        0x11, 0x0A, // SRC: 1.1.10
        0x13, 0x01, // DST: 2/3/1 (group bit not set in cEMI)
        0x01,       // NPDU len = tpduLen-1
        0x00, 0x81  // TPDU
    };

    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), cemiView.size());
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_UINT8(expected[i], cemiView[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cemi_l_data_golden_group_value_write_short);
    return UNITY_END();
}
