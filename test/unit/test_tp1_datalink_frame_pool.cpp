// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tp1_datalink_frame_pool.cpp
 * @brief The TP1 data link layer's receive frame: one buffer, never overrun
 *
 * The layer decodes each received frame into one member frame and lends it to
 * the receive callback (it used to take frames from a 16-frame pool, of which
 * one was ever in use). These tests hold it to that: a burst that arrives from
 * inside the callback is delivered in full, a frame is not left with octets of
 * the one before it, and a pollReceive() made from inside the callback cannot
 * decode over the frame the callback is reading.
 *
 * Runs on the cooperative (bare-metal) platform with the RX task off, which is
 * how a small target drives the layer and needs no host threads.
 */

#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/platform/cooperative_platform.hpp"
#include "unity.h"

#include "../mocks/mock_physical_layer.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::datalink;

namespace {

uint32_t g_nowMs = 0;
uint32_t boardMillis(void*) { return g_nowMs; }
uint64_t boardMicros(void*) { return static_cast<uint64_t>(g_nowMs) * 1000u; }
void boardIdle(void*) { ++g_nowMs; }

platform::CooperativeHooks makeHooks() {
    platform::CooperativeHooks hooks{};
    hooks.millis = &boardMillis;
    hooks.micros = &boardMicros;
    hooks.idle = &boardIdle;
    return hooks;
}

std::vector<uint8_t> encodeTp1(const LDataFrame& frame) {
    std::array<uint8_t, config::MAX_TP1_FRAME_LENGTH> buffer{};
    auto res = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(res.isOk());
    return std::vector<uint8_t>(buffer.begin(), buffer.begin() + res.value());
}

/// A group write from 1.1.1 to 0/0/1 carrying `payload`.
std::vector<uint8_t> groupWrite(std::span<const uint8_t> payload) {
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = false;  // keep deterministic (no ACK generation)
    frame.confirmation = false;
    frame.source = IndividualAddress(0x1101);
    frame.destination = GroupAddress(0x0001);
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, payload);
    return encodeTp1(frame);
}

struct Fixture {
    knx::test::MockPhysicalLayer phy;
    platform::CooperativePlatform platform{makeHooks()};
    Tp1DataLinkLayer dl;

    Fixture()
        : dl(platform, phy, [] {
              Tp1DataLinkConfig config = Tp1DataLinkConfig::defaults();
              config.enableRxTask = false;
              return config;
          }()) {
        TEST_ASSERT_TRUE(dl.init(IndividualAddress(0x1100)).isOk());
        dl.setPromiscuousMode(PromiscuousMode::Enable);
    }
};

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_rx_reentrant_burst_is_delivered_in_full() {
    Fixture f;
    const auto raw = groupWrite({});

    constexpr int burstCount = 64;
    int rxCount = 0;
    bool didBurst = false;

    f.dl.setReceiveCallback([&](const LDataFrame& /*frame*/) {
        ++rxCount;
        if (!didBurst) {
            didBurst = true;
            // The mock calls the layer's physical callback on each inject; the
            // pass already running takes them, one at a time.
            for (int i = 0; i < burstCount; ++i) {
                f.phy.injectFrame(raw);
            }
        }
    });

    f.phy.injectFrame(raw);

    TEST_ASSERT_EQUAL_INT(1 + burstCount, rxCount);
    auto stats = f.dl.getStatistics();
    TEST_ASSERT_EQUAL_UINT32(1 + burstCount, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rxDropped);
    TEST_ASSERT_EQUAL_UINT32(0, stats.decodeFailed);
}

void test_rx_frame_reuse_carries_nothing_over() {
    // A long frame, then a short one, through the same receive frame: the
    // second must arrive with its own length and octets only.
    Fixture f;
    const std::array<uint8_t, 4> longPayload{0xA1, 0xA2, 0xA3, 0xA4};
    const std::array<uint8_t, 1> shortPayload{0x5B};

    std::vector<std::vector<uint8_t>> seen;
    f.dl.setReceiveCallback([&](const LDataFrame& frame) {
        seen.emplace_back(frame.tpdu.begin(), frame.tpdu.end());
    });

    f.phy.injectFrame(groupWrite(longPayload));
    f.phy.injectFrame(groupWrite(shortPayload));

    TEST_ASSERT_EQUAL_UINT(2u, seen.size());
    // TPCI/APCI (2 octets) + payload
    TEST_ASSERT_EQUAL_UINT(2u + longPayload.size(), seen[0].size());
    for (size_t i = 0; i < longPayload.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(longPayload[i], seen[0][2 + i]);
    }
    TEST_ASSERT_EQUAL_UINT(2u + shortPayload.size(), seen[1].size());
    TEST_ASSERT_EQUAL_HEX8(0x5B, seen[1][2]);
}

void test_poll_receive_from_inside_callback_is_busy_and_keeps_the_frame() {
    // Nothing in the stack does this, but the API allows it: a pollReceive()
    // from inside the callback must not decode the next queued frame over the
    // one the callback is still reading. It answers Busy, and the outer pass
    // delivers the queued frame afterwards.
    Fixture f;
    const std::array<uint8_t, 2> first{0x11, 0x12};
    const std::array<uint8_t, 2> second{0x21, 0x22};
    const auto secondRaw = groupWrite(second);

    int rxCount = 0;
    bool nestedWasBusy = false;
    bool frameIntact = false;
    std::vector<uint8_t> secondSeen;

    f.dl.setReceiveCallback([&](const LDataFrame& frame) {
        ++rxCount;
        if (rxCount == 1) {
            f.phy.injectFrame(secondRaw);  // queued behind the frame being read
            auto nested = f.dl.pollReceive();
            nestedWasBusy = nested.isError() && nested.error() == util::ErrorCode::Busy;
            frameIntact = frame.tpdu.size() == 4u && frame.tpdu[2] == 0x11 && frame.tpdu[3] == 0x12;
        } else {
            secondSeen.assign(frame.tpdu.begin(), frame.tpdu.end());
        }
    });

    f.phy.injectFrame(groupWrite(first));

    TEST_ASSERT_TRUE(nestedWasBusy);
    TEST_ASSERT_TRUE(frameIntact);
    TEST_ASSERT_EQUAL_INT(2, rxCount);
    TEST_ASSERT_EQUAL_UINT(4u, secondSeen.size());
    TEST_ASSERT_EQUAL_HEX8(0x21, secondSeen[2]);
    TEST_ASSERT_EQUAL_HEX8(0x22, secondSeen[3]);
    TEST_ASSERT_EQUAL_UINT32(2, f.dl.getStatistics().rxFrames);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_rx_reentrant_burst_is_delivered_in_full);
    RUN_TEST(test_rx_frame_reuse_carries_nothing_over);
    RUN_TEST(test_poll_receive_from_inside_callback_is_busy_and_keeps_the_frame);
    return UNITY_END();
}
