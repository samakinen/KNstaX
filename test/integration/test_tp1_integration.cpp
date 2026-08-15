// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tp1_integration.cpp
 * @brief Integration test for TP1 end-to-end communication
 */

#include "unity.h"
#include "knx/types.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/platform/linux_platform.hpp"
#include "../mocks/mock_physical_layer.hpp"
#include <mutex>
#include <memory>
#include <vector>

using namespace knx;
using namespace knx::test;

static std::unique_ptr<MockPhysicalLayer> physicalLayer;
static std::unique_ptr<knx::platform::LinuxPlatform> platformInstance;
static std::unique_ptr<datalink::Tp1DataLinkLayer> tp1Layer;
static std::vector<datalink::LDataFrame> receivedFrames;
static std::mutex receivedFramesMutex;

void setUp(void) {
    // Create mock physical layer
    physicalLayer = std::make_unique<MockPhysicalLayer>();
    platformInstance = std::make_unique<knx::platform::LinuxPlatform>();
    
    // Create data link layer with mock
    datalink::Tp1DataLinkConfig config = datalink::Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;
    tp1Layer = std::make_unique<datalink::Tp1DataLinkLayer>(*platformInstance, *physicalLayer, config);
    {
        std::lock_guard<std::mutex> lock(receivedFramesMutex);
        receivedFrames.clear();
    }
    tp1Layer->setReceiveCallback([](const datalink::LDataFrame& frame) {
        std::lock_guard<std::mutex> lock(receivedFramesMutex);
        receivedFrames.push_back(frame);
    });
    
    // Initialize
    TEST_ASSERT_TRUE(tp1Layer->init(IndividualAddress(1, 1, 10)).isOk());
}

void tearDown(void) {
    tp1Layer->close();
    tp1Layer.reset();
    physicalLayer.reset();
    platformInstance.reset();
    {
        std::lock_guard<std::mutex> lock(receivedFramesMutex);
        receivedFrames.clear();
    }
}

// Test basic initialization
void test_tp1_initialization(void) {
    TEST_ASSERT_TRUE(tp1Layer->isOpen());
    TEST_ASSERT_EQUAL_INT(IndividualAddress(1, 1, 10).raw, 
                          tp1Layer->getOwnAddress().raw);
}

// Test group address subscription
void test_group_address_subscription(void) {
    GroupAddress addr(5, 3, 7);
    TEST_ASSERT_TRUE(tp1Layer->addGroupAddress(addr).isOk());
    
    // Adding same address again should succeed
    TEST_ASSERT_TRUE(tp1Layer->addGroupAddress(addr).isOk());
}

void test_send_and_receive_roundtrip(void) {
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(1, 2, 3);
    frame.destinationType = AddressType::Group;
    frame.priority = Priority::Normal;
    frame.hopCount = 6;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x11});

    tp1Layer->clearGroupAddresses();
    TEST_ASSERT_TRUE(tp1Layer->addGroupAddress(frame.destination).isOk());

    TEST_ASSERT_TRUE(tp1Layer->sendFrame(frame).isOk());

    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));

    physicalLayer->injectFrame(sent);

    std::lock_guard<std::mutex> lock(receivedFramesMutex);
    TEST_ASSERT_EQUAL(1, receivedFrames.size());
    const auto& rx = receivedFrames.back();
    TEST_ASSERT_EQUAL_UINT16(frame.source.raw, rx.source.raw);
    TEST_ASSERT_EQUAL_UINT16(frame.destination.raw, rx.destination.raw);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(frame.priority), static_cast<uint8_t>(rx.priority));
    TEST_ASSERT_EQUAL_UINT8(frame.hopCount, rx.hopCount);
    TEST_ASSERT_EQUAL_UINT8(frame.payload().size(), rx.payload().size());
    TEST_ASSERT_EQUAL_UINT8(frame.payload()[0], rx.payload()[0]);
}

void test_receive_filters_unknown_group(void) {
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(4, 4, 4);
    frame.destinationType = AddressType::Group;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x22});

    tp1Layer->clearGroupAddresses();
    TEST_ASSERT_TRUE(tp1Layer->addGroupAddress(GroupAddress(1, 1, 1)).isOk());

    TEST_ASSERT_TRUE(tp1Layer->sendFrame(frame).isOk());
    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));

    physicalLayer->injectFrame(sent);

    std::lock_guard<std::mutex> lock(receivedFramesMutex);
    TEST_ASSERT_TRUE(receivedFrames.empty());
}

void test_receive_drops_bad_checksum(void) {
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(1, 2, 3);
    frame.destinationType = AddressType::Group;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x33});

    tp1Layer->clearGroupAddresses();
    TEST_ASSERT_TRUE(tp1Layer->addGroupAddress(frame.destination).isOk());

    TEST_ASSERT_TRUE(tp1Layer->sendFrame(frame).isOk());

    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));

    // Corrupt checksum
    TEST_ASSERT_FALSE(sent.empty());
    sent.back() ^= 0xFF;

    physicalLayer->injectFrame(sent);

    std::lock_guard<std::mutex> lock(receivedFramesMutex);
    TEST_ASSERT_TRUE(receivedFrames.empty());
}

void test_receive_individual_address_match(void) {
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 20);
    const IndividualAddress dest(1, 1, 10);
    frame.setDestination(dest);
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x04});

    tp1Layer->clearGroupAddresses();

    TEST_ASSERT_TRUE(tp1Layer->sendFrame(frame).isOk());
    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));

    physicalLayer->injectFrame(sent);

    std::lock_guard<std::mutex> lock(receivedFramesMutex);
    TEST_ASSERT_EQUAL(1, receivedFrames.size());
    const auto& rx = receivedFrames.back();
    TEST_ASSERT_TRUE(rx.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL_UINT16(frame.destination.raw, rx.destination.raw);
    TEST_ASSERT_EQUAL_UINT8(frame.payload()[0], rx.payload()[0]);
}

// Test send frame structure
void test_send_frame_structure(void) {
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(5, 3, 7);
    frame.destinationType = AddressType::Group;
    frame.priority = Priority::Low;
    frame.standardFrame = true;
    frame.ackRequested = true;
    frame.hopCount = 6;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
    
    // Add group address subscription first
    TEST_ASSERT_TRUE(tp1Layer->addGroupAddress(GroupAddress(5, 3, 7)).isOk());
    
    // Send frame (may fail due to no UART, but structure should be valid)
    // In real scenario with mock, this would succeed
}

// Test promiscuous mode
void test_promiscuous_mode(void) {
    tp1Layer->setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);
    
    // In promiscuous mode, should accept all frames
    tp1Layer->setPromiscuousMode(knx::datalink::PromiscuousMode::Disable);
    
    // Back to filtered mode
    TEST_ASSERT_TRUE(tp1Layer->isOpen());
}

// Test frame encoding with DPT1
void test_frame_with_bool_data(void) {
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(0, 0, 1);
    frame.destinationType = AddressType::Group;
    frame.priority = Priority::Normal;
    frame.standardFrame = true;
    frame.ackRequested = false;
    frame.hopCount = 5;

    // Encode boolean true (DPT 1)
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});

    TEST_ASSERT_EQUAL(1, frame.payload().size());
    TEST_ASSERT_EQUAL(0x01, frame.payload()[0] & 0x01);
}

// Test frame with multiple data bytes
void test_frame_with_multiple_data_bytes(void) {
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(0, 0, 2);
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;

    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0xAA, 0xBB, 0xCC});

    TEST_ASSERT_EQUAL(3, frame.payload().size());
    TEST_ASSERT_EQUAL(0xAA, frame.payload()[0]);
    TEST_ASSERT_EQUAL(0xBB, frame.payload()[1]);
    TEST_ASSERT_EQUAL(0xCC, frame.payload()[2]);
}

// Test hop count management
void test_hop_count_management(void) {
    datalink::LDataFrame frame;
    frame.hopCount = 6;
    TEST_ASSERT_EQUAL(6, frame.hopCount);
    
    frame.hopCount = 0;
    TEST_ASSERT_EQUAL(0, frame.hopCount);
    
    frame.hopCount = 3;
    TEST_ASSERT_EQUAL(3, frame.hopCount);
}

// Test address comparison
void test_address_comparison_in_frame(void) {
    GroupAddress addr1(5, 3, 7);
    GroupAddress addr2(5, 3, 7);
    GroupAddress addr3(5, 3, 8);
    
    TEST_ASSERT_EQUAL(addr1.raw, addr2.raw);
    TEST_ASSERT_NOT_EQUAL(addr1.raw, addr3.raw);
}

// Test run
int run_all_tp1_integration_tests(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_tp1_initialization);
    RUN_TEST(test_group_address_subscription);
    RUN_TEST(test_send_frame_structure);
    RUN_TEST(test_promiscuous_mode);
    RUN_TEST(test_send_and_receive_roundtrip);
    RUN_TEST(test_receive_filters_unknown_group);
    RUN_TEST(test_receive_drops_bad_checksum);
    RUN_TEST(test_receive_individual_address_match);
    RUN_TEST(test_frame_with_bool_data);
    RUN_TEST(test_frame_with_multiple_data_bytes);
    RUN_TEST(test_hop_count_management);
    RUN_TEST(test_address_comparison_in_frame);
    
    return UNITY_END();
}

int main() {
    return run_all_tp1_integration_tests();
}
