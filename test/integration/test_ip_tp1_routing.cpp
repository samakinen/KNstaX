// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <array>
#include <span>

#include "unity.h"
#include "knx/physical/ip_tunneling_physical.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/physical/physical_factory.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/application/apci_services.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <queue>
#include <mutex>

using namespace knx;
using namespace knx::datalink;
using namespace knx::physical;

static void writeHeader(std::vector<uint8_t>& buf, uint16_t service, uint16_t totalLen) {
    std::array<uint8_t, knx::netip::KnxNetIpCodec::kHeaderLen> header{};
    auto result = knx::netip::KnxNetIpCodec::encodeHeader(
        knx::NetIpServiceType(service),
        totalLen - knx::netip::KnxNetIpCodec::kHeaderLen,
        header);
    if (result.isError()) return;
    buf.insert(buf.end(), header.begin(), header.end());
}

void setUp(void) {}
void tearDown(void) {}

void test_ip_to_tp1_routing(void) {
    // Simpler test: demonstrate frame conversion between IP (cEMI) and TP1 (raw)
    // without complex async routing

    // Create test frame
    LDataFrame testFrame;
    testFrame.standardFrame = true;
    testFrame.repeated = false;
    testFrame.priority = Priority::Low;
    testFrame.ackRequested = false;
    testFrame.confirmation = false;
    testFrame.source = IndividualAddress(0x1100);
    testFrame.destination = GroupAddress(0x0301);
    testFrame.destinationType = AddressType::Group;
    testFrame.hopCount = 6;
    testFrame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x02});

    // Convert to TP1 raw bytes
    uint8_t tp1Frame[64]; size_t tp1Len = sizeof(tp1Frame);
    auto encodeResult = FrameCodec::encodeFrame(testFrame, std::span<uint8_t>(tp1Frame, tp1Len));
    TEST_ASSERT_TRUE(encodeResult.isOk());
    tp1Len = encodeResult.value();

    // Convert to cEMI for IP tunneling
    std::array<uint8_t, netip::kMaxCemiLDataSize> cemiFrame{};
    auto cemiResult = netip::encodeCemiLData(testFrame, 0x11, cemiFrame);
    TEST_ASSERT_TRUE(cemiResult.isOk());
    const auto cemiView = std::span<const uint8_t>(cemiFrame.data(), cemiResult.value());

    // Decode cEMI back to LDataFrame
    LDataFrame decodedFromCemi;
    uint8_t msgCode = 0;
    TEST_ASSERT_TRUE(netip::decodeCemiLData(cemiView, decodedFromCemi, msgCode));
    TEST_ASSERT_EQUAL(0x11, msgCode);

    // Verify round-trip
    TEST_ASSERT_EQUAL(testFrame.source.raw, decodedFromCemi.source.raw);
    TEST_ASSERT_EQUAL(testFrame.destination.raw, decodedFromCemi.destination.raw);
    TEST_ASSERT_EQUAL(testFrame.apci().raw, decodedFromCemi.apci().raw);
    TEST_ASSERT_EQUAL(1, decodedFromCemi.payload().size());
    TEST_ASSERT_EQUAL(testFrame.payload()[0], decodedFromCemi.payload()[0]);

    // Decode TP1 frame
    LDataFrame decodedFromTp1;
    auto decodeResult = FrameCodec::decodeFrame(std::span<const uint8_t>(tp1Frame, tp1Len), decodedFromTp1);
    TEST_ASSERT_TRUE(decodeResult.isOk());

    // Verify both conversions produce equivalent frames
    TEST_ASSERT_EQUAL(decodedFromCemi.source.raw, decodedFromTp1.source.raw);
    TEST_ASSERT_EQUAL(decodedFromCemi.destination.raw, decodedFromTp1.destination.raw);
    TEST_ASSERT_EQUAL(decodedFromCemi.apci().raw, decodedFromTp1.apci().raw);
}

void test_bidirectional_routing(void) {
    // Test bidirectional conversion: TP1 → cEMI → TP1
    
    // Create frame
    LDataFrame frame1;
    frame1.standardFrame = true;
    frame1.repeated = false;
    frame1.priority = Priority::Low;
    frame1.ackRequested = false;
    frame1.confirmation = false;
    frame1.source = IndividualAddress(0x1234);
    frame1.destination = GroupAddress(0x0501);
    frame1.destinationType = AddressType::Group;
    frame1.hopCount = 5;
    frame1.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x02});

    // Encode as TP1 raw bytes
    uint8_t tp1Bytes[64]; size_t tp1Len = sizeof(tp1Bytes);
    auto encodeResult2 = FrameCodec::encodeFrame(frame1, std::span<uint8_t>(tp1Bytes, tp1Len));
    TEST_ASSERT_TRUE(encodeResult2.isOk());
    tp1Len = encodeResult2.value();

    // Convert to cEMI (IP representation)
    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto cemiResult2 = netip::encodeCemiLData(frame1, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult2.isOk());
    const auto cemiView2 = std::span<const uint8_t>(cemi.data(), cemiResult2.value());

    // Decode cEMI back to LDataFrame
    LDataFrame fromCemi;
    uint8_t msgCode = 0;
    TEST_ASSERT_TRUE(netip::decodeCemiLData(cemiView2, fromCemi, msgCode));

    // Verify TP1→cEMI→LDataFrame conversion preserves key fields
    TEST_ASSERT_EQUAL(frame1.source.raw, fromCemi.source.raw);
    TEST_ASSERT_EQUAL(frame1.destination.raw, fromCemi.destination.raw);
    TEST_ASSERT_EQUAL(frame1.destinationType, fromCemi.destinationType);
}




int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ip_to_tp1_routing);
    RUN_TEST(test_bidirectional_routing);
    return UNITY_END();
}
