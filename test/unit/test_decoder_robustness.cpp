// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_decoder_robustness.cpp
 * @brief Robustness tests for decoders (malformed input should not crash/hang)
 */

#include "unity.h"

#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/netip/gateway_discovery_client.hpp"
#include "knx/netip/routing.hpp"

#include <cstdint>
#include <span>
#include <vector>

using knx::datalink::FrameCodec;
using knx::datalink::LDataFrame;
using knx::netip::GatewayDiscoveryClient;
using knx::netip::RoutingCodec;
using knx::netip::GatewayInfo;

void setUp(void) {}
void tearDown(void) {}

namespace {
uint32_t rngState = 0xC0FFEE01u;

uint32_t xorshift32() {
    uint32_t x = rngState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rngState = x;
    return x;
}

std::vector<uint8_t> randomBytes(size_t maxLen) {
    const size_t len = (maxLen == 0) ? 0 : (static_cast<size_t>(xorshift32()) % (maxLen + 1));
    std::vector<uint8_t> out;
    out.resize(len);
    for (size_t i = 0; i < len; ++i) {
        out[i] = static_cast<uint8_t>(xorshift32() & 0xFF);
    }
    return out;
}

const std::vector<std::vector<uint8_t>>& tp1FrameCorpus() {
    static const std::vector<std::vector<uint8_t>> corpus = {
        {},
        {0x00},
        {0xFF},
        {0xBC, 0x00},
        {0xBC, 0x11, 0x00, 0x00, 0x00},
        // Overlong / random-ish payloads
        std::vector<uint8_t>(23, 0x00),
        std::vector<uint8_t>(23, 0xFF),
        {0xBC, 0x11, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    };
    return corpus;
}

const std::vector<std::vector<uint8_t>>& cemiCorpus() {
    static const std::vector<std::vector<uint8_t>> corpus = {
        {},
        {0x11},
        {0x29},
        // Minimal-ish sizes around the common cEMI header length
        std::vector<uint8_t>(1, 0x00),
        std::vector<uint8_t>(2, 0x00),
        std::vector<uint8_t>(10, 0x00),
        std::vector<uint8_t>(10, 0xFF),
        // L_Data.req with too-short payload
        {0x11, 0x00, 0xBC, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00},
    };
    return corpus;
}

const std::vector<std::vector<uint8_t>>& knxnetipCorpus() {
    static const std::vector<std::vector<uint8_t>> corpus = {
        {},
        {0x06},
        {0x06, 0x10},
        // Wrong header length/version
        {0x05, 0x10, 0x02, 0x02, 0x00, 0x06},
        {0x06, 0x11, 0x02, 0x02, 0x00, 0x06},
        // Length mismatches
        {0x06, 0x10, 0x02, 0x02, 0x00, 0x05},
        {0x06, 0x10, 0x02, 0x02, 0x01, 0x00},
        // Minimal 6-byte header with zeroed service/len
        {0x06, 0x10, 0x00, 0x00, 0x00, 0x06},
    };
    return corpus;
}

const std::vector<std::vector<uint8_t>>& knxnetipRoutingCorpus() {
    static const std::vector<std::vector<uint8_t>> corpus = {
        {},
        {0x06},
        {0x06, 0x10},
        // Wrong header length/version
        {0x05, 0x10, 0x05, 0x30, 0x00, 0x06},
        {0x06, 0x11, 0x05, 0x30, 0x00, 0x06},
        // totalLen < header
        {0x06, 0x10, 0x05, 0x30, 0x00, 0x05},
        // totalLen > datagram
        {0x06, 0x10, 0x05, 0x30, 0x01, 0x00},
        // Minimal routing indication with empty payload
        {0x06, 0x10, 0x05, 0x30, 0x00, 0x06},
        // Routing indication with trailing bytes beyond totalLen
        {0x06, 0x10, 0x05, 0x30, 0x00, 0x07, 0xAA, 0xBB, 0xCC},
        // Routing lost message with correct length
        {0x06, 0x10, 0x05, 0x31, 0x00, 0x08, 0x00, 0x01},
        // Routing lost message with wrong length
        {0x06, 0x10, 0x05, 0x31, 0x00, 0x09, 0x00, 0x01, 0x00},
    };
    return corpus;
}
} // namespace

void test_tp1_framecodec_decode_random_inputs_does_not_crash(void) {
    for (const auto& bytes : tp1FrameCorpus()) {
        LDataFrame frame;
        const auto res = FrameCodec::decodeFrame(std::span<const uint8_t>(bytes), frame);
        if (res.isOk()) {
            TEST_ASSERT_TRUE(frame.isValid());
            TEST_ASSERT_TRUE(frame.tpdu.size() >= 2);
        }
    }

    // Keep runtime bounded; focus is: no crash, no hang.
    for (size_t i = 0; i < 5000; ++i) {
        const auto bytes = randomBytes(64);
        LDataFrame frame;
        const auto res = FrameCodec::decodeFrame(std::span<const uint8_t>(bytes), frame);
        if (res.isOk()) {
            TEST_ASSERT_TRUE(frame.isValid());
            TEST_ASSERT_TRUE(frame.tpdu.size() >= 2);
        }
    }
}

void test_cemi_decode_random_inputs_does_not_crash(void) {
    for (const auto& bytes : cemiCorpus()) {
        LDataFrame frame;
        uint8_t messageCode = 0;
        const auto result = knx::netip::decodeCemiLData(std::span<const uint8_t>(bytes), frame, messageCode);
        if (result.isOk()) {
            TEST_ASSERT_TRUE(frame.tpdu.size() >= 2);
        }
    }

    for (size_t i = 0; i < 5000; ++i) {
        const auto bytes = randomBytes(128);
        LDataFrame frame;
        uint8_t messageCode = 0;
        const auto result = knx::netip::decodeCemiLData(std::span<const uint8_t>(bytes), frame, messageCode);
        if (result.isOk()) {
            TEST_ASSERT_TRUE(frame.tpdu.size() >= 2);
        }
    }
}

void test_knxnetip_gateway_parsers_random_inputs_does_not_crash(void) {
    for (const auto& bytes : knxnetipCorpus()) {
        GatewayInfo info1{};
        GatewayInfo info2{};
        (void)GatewayDiscoveryClient::parseSearchResponsePacket(std::span<const uint8_t>(bytes), info1);
        (void)GatewayDiscoveryClient::parseDescriptionResponsePacket(std::span<const uint8_t>(bytes), info2);
    }

    for (size_t i = 0; i < 5000; ++i) {
        const auto bytes = randomBytes(512);
        GatewayInfo info1{};
        GatewayInfo info2{};
        (void)GatewayDiscoveryClient::parseSearchResponsePacket(std::span<const uint8_t>(bytes), info1);
        (void)GatewayDiscoveryClient::parseDescriptionResponsePacket(std::span<const uint8_t>(bytes), info2);
    }
}

void test_knxnetip_gateway_parsers_reject_length_mismatches(void) {
    // Minimal KNXnet/IP header is 6 bytes; validate that inconsistent lengths are rejected.
    // Header: [0]=0x06, [1]=0x10, [2..3]=service type, [4..5]=total length

    // totalLen < header
    {
        const std::vector<uint8_t> pkt = {0x06, 0x10, 0x02, 0x02, 0x00, 0x05};
        GatewayInfo info{};
        TEST_ASSERT_TRUE(GatewayDiscoveryClient::parseSearchResponsePacket(std::span<const uint8_t>(pkt), info).isError());
    }

    // totalLen > received
    {
        const std::vector<uint8_t> pkt = {0x06, 0x10, 0x02, 0x02, 0x01, 0x00};
        GatewayInfo info{};
        TEST_ASSERT_TRUE(GatewayDiscoveryClient::parseSearchResponsePacket(std::span<const uint8_t>(pkt), info).isError());
    }
}

void test_knxnetip_routing_codec_random_inputs_does_not_crash(void) {
    for (const auto& bytes : knxnetipRoutingCorpus()) {
        (void)RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(bytes));

        uint16_t lost = 0;
        (void)RoutingCodec::decodeRoutingLostMessage(std::span<const uint8_t>(bytes), lost);
    }

    for (size_t i = 0; i < 5000; ++i) {
        const auto bytes = randomBytes(512);
        (void)RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(bytes));

        uint16_t lost = 0;
        (void)RoutingCodec::decodeRoutingLostMessage(std::span<const uint8_t>(bytes), lost);
    }
}

void test_knxnetip_routing_codec_rejects_length_mismatches(void) {
    // totalLen < header
    {
        const std::vector<uint8_t> pkt = {0x06, 0x10, 0x05, 0x30, 0x00, 0x05};
        TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(pkt)).isError());
    }

    // totalLen > received
    {
        const std::vector<uint8_t> pkt = {0x06, 0x10, 0x05, 0x30, 0x01, 0x00};
        TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingIndication(std::span<const uint8_t>(pkt)).isError());
    }

    // LOST_MESSAGE must be exactly 8 bytes total
    {
        const std::vector<uint8_t> pkt = {0x06, 0x10, 0x05, 0x31, 0x00, 0x09, 0x00, 0x01, 0x00};
        uint16_t lost = 0;
        TEST_ASSERT_TRUE(RoutingCodec::decodeRoutingLostMessage(std::span<const uint8_t>(pkt), lost).isError());
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_tp1_framecodec_decode_random_inputs_does_not_crash);
    RUN_TEST(test_cemi_decode_random_inputs_does_not_crash);
    RUN_TEST(test_knxnetip_gateway_parsers_random_inputs_does_not_crash);
    RUN_TEST(test_knxnetip_gateway_parsers_reject_length_mismatches);
    RUN_TEST(test_knxnetip_routing_codec_random_inputs_does_not_crash);
    RUN_TEST(test_knxnetip_routing_codec_rejects_length_mismatches);
    return UNITY_END();
}
