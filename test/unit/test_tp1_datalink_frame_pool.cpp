// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tp1_datalink_frame_pool.cpp
 * @brief Regression tests for TP1 data link layer frame pool safety
 */

#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/platform/linux_platform.hpp"
#include "unity.h"

#include "../mocks/mock_physical_layer.hpp"

#include <atomic>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::datalink;

static std::vector<uint8_t> encodeTp1(const LDataFrame& frame) {
    uint8_t buffer[23];
    auto res = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(res.isOk());
    return std::vector<uint8_t>(buffer, buffer + res.value());
}

void setUp(void) {}
void tearDown(void) {}

void test_rx_reentrant_burst_does_not_exhaust_pool_or_crash() {
    knx::test::MockPhysicalLayer phy;
    knx::platform::LinuxPlatform platform;
    Tp1DataLinkConfig config = Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;
    Tp1DataLinkLayer dl(platform, phy, config);

    TEST_ASSERT_TRUE(dl.init(IndividualAddress(0x1100)).isOk());
    dl.setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);

    LDataFrame incoming;
    incoming.standardFrame = true;
    incoming.repeated = false;
    incoming.priority = Priority::Low;
    incoming.ackRequested = false; // keep deterministic (no ACK generation)
    incoming.confirmation = false;
    incoming.source = IndividualAddress(0x1101);
    incoming.destination = GroupAddress(0x0001);
    incoming.destinationType = AddressType::Group;
    incoming.hopCount = 6;
    incoming.setTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        knx::application::APCIField(0),
        std::span<const uint8_t>{});

    auto raw = encodeTp1(incoming);

    constexpr int burstCount = 64;
    std::atomic<int> rxCount{0};
    std::atomic<bool> didBurst{false};

    dl.setReceiveCallback([&](const LDataFrame& /*frame*/) {
        rxCount.fetch_add(1);
        if (!didBurst.exchange(true)) {
            for (int i = 0; i < burstCount; ++i) {
                phy.injectFrame(raw);
            }
        }
    });

    phy.injectFrame(raw);

    TEST_ASSERT_EQUAL_INT(1 + burstCount, rxCount.load());

    auto stats = dl.getStatistics();
    TEST_ASSERT_EQUAL_UINT32(1 + burstCount, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rxDropped);
    TEST_ASSERT_EQUAL_UINT32(0, stats.decodeFailed);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_rx_reentrant_burst_does_not_exhaust_pool_or_crash);
    return UNITY_END();
}
