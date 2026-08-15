// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_datalink_edge_cases.cpp
 * @brief Unit tests for TP1 data link layer edge cases and error handling
 */

#include "unity.h"
#include "knx/types.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/platform/linux_platform.hpp"
#include "../mocks/mock_physical_layer.hpp"
#include <memory>
#include <mutex>
#include <vector>

using namespace knx;
using namespace knx::test;

static std::unique_ptr<MockPhysicalLayer> physicalLayer;
static std::unique_ptr<knx::platform::LinuxPlatform> platformInstance;
static std::unique_ptr<datalink::Tp1DataLinkLayer> dlLayer;
static std::vector<datalink::LDataFrame> receivedFrames;
static int rxCallbackCount = 0;
static std::mutex rxMutex;

void setUp(void) {
    platformInstance = std::make_unique<knx::platform::LinuxPlatform>();
    physicalLayer = std::make_unique<MockPhysicalLayer>();
    datalink::Tp1DataLinkConfig config = datalink::Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;
    dlLayer = std::make_unique<datalink::Tp1DataLinkLayer>(*platformInstance, *physicalLayer, config);
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        receivedFrames.clear();
        rxCallbackCount = 0;
    }
    
    dlLayer->setReceiveCallback([](const datalink::LDataFrame& frame) {
        std::lock_guard<std::mutex> lock(rxMutex);
        receivedFrames.push_back(frame);
        rxCallbackCount++;
    });
    
    TEST_ASSERT_TRUE(dlLayer->init(IndividualAddress(1, 1, 10)).isOk());
}

void tearDown(void) {
    dlLayer->close();
    dlLayer.reset();
    physicalLayer.reset();
    platformInstance.reset();
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        receivedFrames.clear();
        rxCallbackCount = 0;
    }
}

// Test 1: Statistics counters initialization
void test_statistics_initialization(void) {
    auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(0, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.txFrames);
    TEST_ASSERT_EQUAL_UINT32(0, stats.rxDropped);
    TEST_ASSERT_EQUAL_UINT32(0, stats.decodeFailed);
    TEST_ASSERT_EQUAL_UINT32(0, stats.filterDropped);
}

// Test 2: TX counter increments on successful send
void test_statistics_tx_counter(void) {
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(1, 2, 3);
    frame.destinationType = AddressType::Group;
    frame.priority = Priority::Normal;
    frame.hopCount = 6;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x55});
    
    auto statsBefore = dlLayer->getStatistics();
    TEST_ASSERT_TRUE(dlLayer->sendFrame(frame).isOk());
    auto statsAfter = dlLayer->getStatistics();
    
    TEST_ASSERT_EQUAL_UINT32(statsBefore.txFrames + 1, statsAfter.txFrames);
}

// Test 3: Filter dropped counter when address doesn't match
void test_statistics_filter_dropped(void) {
    dlLayer->clearGroupAddresses();
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(GroupAddress(1, 1, 1)).isOk());
    
    // Send frame to different group address
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 20);
    frame.destination = GroupAddress(9, 9, 9);  // Not subscribed
    frame.destinationType = AddressType::Group;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0xAA});
    
    std::vector<uint8_t> raw;
    TEST_ASSERT_TRUE(dlLayer->sendFrame(frame).isOk());
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(raw));
    
    auto statsBefore = dlLayer->getStatistics();
    physicalLayer->injectFrame(raw);
    auto statsAfter = dlLayer->getStatistics();
    
    TEST_ASSERT_EQUAL_UINT32(statsBefore.filterDropped + 1, statsAfter.filterDropped);
    TEST_ASSERT_EQUAL_UINT32(0, statsAfter.rxFrames); // Not counted as received
}

// Test 4: RX counter increments on successful receive
void test_statistics_rx_counter(void) {
    dlLayer->clearGroupAddresses();
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(GroupAddress(5, 5, 5)).isOk());
    
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 20);
    frame.destination = GroupAddress(5, 5, 5);
    frame.destinationType = AddressType::Group;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0xBB});
    
    std::vector<uint8_t> raw;
    TEST_ASSERT_TRUE(dlLayer->sendFrame(frame).isOk());
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(raw));
    
    auto statsBefore = dlLayer->getStatistics();
    physicalLayer->injectFrame(raw);
    auto statsAfter = dlLayer->getStatistics();
    
    TEST_ASSERT_EQUAL_UINT32(statsBefore.rxFrames + 1, statsAfter.rxFrames);
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        TEST_ASSERT_EQUAL_INT(1, rxCallbackCount);
    }
}

// Test 5: Configuration struct with custom parameters
void test_custom_configuration(void) {
    datalink::Tp1DataLinkConfig config;
    config.rxTaskStackSize = 8192;
    config.rxTaskPriority = 10;
    config.txMutexTimeout = 2000;
    
    auto customLayer = std::make_unique<datalink::Tp1DataLinkLayer>(*platformInstance, *physicalLayer, config);
    TEST_ASSERT_TRUE(customLayer->init(IndividualAddress(1, 1, 11)).isOk());
    TEST_ASSERT_TRUE(customLayer->isOpen());
    customLayer->close();
}

// Test 6: Default configuration struct
void test_default_configuration(void) {
    auto defaultConfig = datalink::Tp1DataLinkConfig::defaults();
    TEST_ASSERT_EQUAL_UINT32(knx::config::RX_TASK_STACK_SIZE, defaultConfig.rxTaskStackSize);
    TEST_ASSERT_EQUAL_UINT32(5, defaultConfig.rxTaskPriority);
    TEST_ASSERT_EQUAL_UINT32(1000, defaultConfig.txMutexTimeout);
}

// Test 6b: Async TX progression works through the base physical interface
void test_async_physical_progression_via_base_interface(void) {
    auto asyncPhysical = std::make_unique<MockPhysicalLayer>();
    asyncPhysical->setAsyncSendEnabled(true);
    asyncPhysical->queueAsyncOutcome(physical::TxOutcomeState::Success);

    datalink::Tp1DataLinkConfig config = datalink::Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;

    auto asyncLayer = std::make_unique<datalink::Tp1DataLinkLayer>(
        *platformInstance,
        *asyncPhysical,
        platformInstance.get(),
        config);

    TEST_ASSERT_TRUE(asyncLayer->init(IndividualAddress(1, 1, 12)).isOk());

    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 12);
    frame.destination = GroupAddress(1, 2, 3);
    frame.destinationType = AddressType::Group;
    frame.priority = Priority::Normal;
    frame.hopCount = 6;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData,
                  application::APCIService::GroupValueWrite,
                  {0x66});

    auto beginResult = asyncLayer->beginTransmit(frame);
    TEST_ASSERT_TRUE(beginResult.isOk());

    auto firstProgress = asyncLayer->pollTransmit();
    TEST_ASSERT_TRUE(firstProgress.isOk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(datalink::Tp1DataLinkLayer::TxProgressState::Pending),
                          static_cast<int>(firstProgress.value()));

    auto secondProgress = asyncLayer->pollTransmit();
    TEST_ASSERT_TRUE(secondProgress.isOk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(datalink::Tp1DataLinkLayer::TxProgressState::Success),
                          static_cast<int>(secondProgress.value()));

    std::vector<uint8_t> sentRaw;
    TEST_ASSERT_TRUE(asyncPhysical->getSentFrame(sentRaw));
    TEST_ASSERT_FALSE(sentRaw.empty());

    auto stats = asyncLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(1u, stats.txFrames);

    asyncLayer->close();
}

// Test 7: Multiple group address management
void test_multiple_group_addresses(void) {
    dlLayer->clearGroupAddresses();
    
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(GroupAddress(1, 1, 1)).isOk());
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(GroupAddress(2, 2, 2)).isOk());
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(GroupAddress(3, 3, 3)).isOk());
    
    // Remove one
    TEST_ASSERT_TRUE(dlLayer->removeGroupAddress(GroupAddress(2, 2, 2)).isOk());
    
    // Verify filtering works for remaining addresses
    datalink::LDataFrame frame1;
    frame1.source = IndividualAddress(1, 1, 20);
    frame1.destination = GroupAddress(1, 1, 1);
    frame1.destinationType = AddressType::Group;
    frame1.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
    
    datalink::LDataFrame frame2;
    frame2.source = IndividualAddress(1, 1, 20);
    frame2.destination = GroupAddress(3, 3, 3);
    frame2.destinationType = AddressType::Group;
    frame2.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x02});
    
    std::vector<uint8_t> raw1, raw2;
    TEST_ASSERT_TRUE(dlLayer->sendFrame(frame1).isOk());
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(raw1));
    TEST_ASSERT_TRUE(dlLayer->sendFrame(frame2).isOk());
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(raw2));
    
    physicalLayer->injectFrame(raw1);
    physicalLayer->injectFrame(raw2);

    {
        std::lock_guard<std::mutex> lock(rxMutex);
        TEST_ASSERT_EQUAL_INT(2, rxCallbackCount);
    }
}

// Test 8: Priority levels
void test_priority_levels(void) {
    datalink::LDataFrame lowPri;
    lowPri.source = IndividualAddress(1, 1, 10);
    lowPri.destination = GroupAddress(1, 1, 1);
    lowPri.destinationType = AddressType::Group;
    lowPri.priority = Priority::Low;
    lowPri.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
    
    datalink::LDataFrame normalPri;
    normalPri = lowPri;
    normalPri.priority = Priority::Normal;
    
    datalink::LDataFrame urgentPri;
    urgentPri = lowPri;
    urgentPri.priority = Priority::Urgent;
    
    datalink::LDataFrame systemPri;
    systemPri = lowPri;
    systemPri.priority = Priority::System;
    
    TEST_ASSERT_TRUE(dlLayer->sendFrame(lowPri).isOk());
    TEST_ASSERT_TRUE(dlLayer->sendFrame(normalPri).isOk());
    TEST_ASSERT_TRUE(dlLayer->sendFrame(urgentPri).isOk());
    TEST_ASSERT_TRUE(dlLayer->sendFrame(systemPri).isOk());
    
    auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(4, stats.txFrames);
}

// Test 9: Hop count variations
void test_hop_count_variations(void) {
    for (uint8_t hopCount = 0; hopCount <= 7; hopCount++) {
        datalink::LDataFrame frame;
        frame.source = IndividualAddress(1, 1, 10);
        frame.destination = GroupAddress(1, 2, 3);
        frame.destinationType = AddressType::Group;
        frame.hopCount = hopCount;
        frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {hopCount});
        
        TEST_ASSERT_TRUE(dlLayer->sendFrame(frame).isOk());
    }
    
    auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(8, stats.txFrames);
}

// Test 10: Frame pool should not exhaust during steady RX
void test_rx_does_not_exhaust_frame_pool(void) {
    dlLayer->setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);

    // Build and inject many frames. If the DL forgets to release pooled frames,
    // rxDropped will start increasing once the pool is exhausted.
    constexpr int kFramesToInject = 100;
    for (int i = 0; i < kFramesToInject; ++i) {
        datalink::LDataFrame frame;
        frame.source = IndividualAddress(1, 1, 20);
        frame.destination = GroupAddress(1, 2, 3);
        frame.destinationType = AddressType::Group;
        frame.ackRequested = false; // avoid generating ACK traffic
        frame.hopCount = 6;
        frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {static_cast<uint8_t>(i & 0xFF)});

        std::vector<uint8_t> raw;
        TEST_ASSERT_TRUE(dlLayer->sendFrame(frame).isOk());
        TEST_ASSERT_TRUE(physicalLayer->getSentFrame(raw));

        physicalLayer->injectFrame(raw);
    }

    auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(0, stats.rxDropped);
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(kFramesToInject), stats.rxFrames);
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        TEST_ASSERT_EQUAL_INT(kFramesToInject, rxCallbackCount);
    }
}

// Test 11: Individual address targeting
void test_individual_address_targeting(void) {
    dlLayer->clearGroupAddresses();
    
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 20);
    frame.destination = GroupAddress(dlLayer->getOwnAddress().raw); // Use raw for individual
    frame.destinationType = AddressType::Individual;  // Individual addressing
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0xFF});
    
    std::vector<uint8_t> raw;
    TEST_ASSERT_TRUE(dlLayer->sendFrame(frame).isOk());
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(raw));
    
    physicalLayer->injectFrame(raw);

    // Should receive even without group address subscription
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        TEST_ASSERT_EQUAL_INT(1, rxCallbackCount);
    }
}

// Test 12: Empty data payload (minimum KNX frame)
void test_empty_data_payload(void) {
    dlLayer->clearGroupAddresses();
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(GroupAddress(7, 7, 7)).isOk());
    
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 20);
    frame.destination = GroupAddress(7, 7, 7);
    frame.destinationType = AddressType::Group;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueRead, {});
    
    std::vector<uint8_t> raw;
    TEST_ASSERT_TRUE(dlLayer->sendFrame(frame).isOk());
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(raw));
    
    physicalLayer->injectFrame(raw);

    {
        std::lock_guard<std::mutex> lock(rxMutex);
        TEST_ASSERT_EQUAL_INT(1, rxCallbackCount);
        TEST_ASSERT_TRUE(receivedFrames.back().payload().size() <= 1);
    }
}

// Test 13: Maximum data payload
// TP1 standard frame length field encodes TPDU byte count (0..15).
// With the KNX-correct 2-byte TPDU header (TPCI/APCI), max user payload is 15 - 2 = 13 bytes.
void test_maximum_data_payload(void) {
    dlLayer->clearGroupAddresses();
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(GroupAddress(8, 8, 8)).isOk());
    
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 20);
    frame.destination = GroupAddress(8, 8, 8);
    frame.destinationType = AddressType::Group;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite,
                  {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                   0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC});
    
    std::vector<uint8_t> raw;
    TEST_ASSERT_TRUE(dlLayer->sendFrame(frame).isOk());
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(raw));
    
    physicalLayer->injectFrame(raw);

    {
        std::lock_guard<std::mutex> lock(rxMutex);
        TEST_ASSERT_EQUAL_INT(1, rxCallbackCount);
        TEST_ASSERT_EQUAL_INT(13, receivedFrames.back().payload().size());
    }
}

// L2 duplication prevention (03_02_02 §2.3): a repetition of the directly
// preceding correctly received frame must be indicated to the user only once.
// Without this, a device answers an L2-repeated IndividualAddress_Read
// broadcast once per repetition and ETS reports "more than one device is in
// programming mode".
void test_repeated_frame_delivered_only_once(void) {
    dlLayer->clearGroupAddresses();
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(GroupAddress(6, 6, 6)).isOk());

    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 20);
    frame.destination = GroupAddress(6, 6, 6);
    frame.destinationType = AddressType::Group;
    frame.repeated = false;
    frame.ackRequested = false;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0xCD});

    uint8_t original[23];
    auto encOriginal = datalink::FrameCodec::encodeFrame(frame, std::span<uint8_t>(original));
    TEST_ASSERT_TRUE(encOriginal.isOk());

    frame.repeated = true;  // identical content, wire r-bit cleared
    uint8_t repetition[23];
    auto encRepetition = datalink::FrameCodec::encodeFrame(frame, std::span<uint8_t>(repetition));
    TEST_ASSERT_TRUE(encRepetition.isOk());

    physicalLayer->injectFrame(std::vector<uint8_t>(original, original + encOriginal.value()));
    physicalLayer->injectFrame(std::vector<uint8_t>(repetition, repetition + encRepetition.value()));

    auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(1, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(1, stats.duplicatesDropped);
    {
        std::lock_guard<std::mutex> lock(rxMutex);
        TEST_ASSERT_EQUAL_INT(1, rxCallbackCount);
    }

    // A fresh (non-repeated) transmission of the same content is NOT a
    // duplicate and must be delivered again.
    physicalLayer->injectFrame(std::vector<uint8_t>(original, original + encOriginal.value()));
    stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(2, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(1, stats.duplicatesDropped);

    // A repetition whose original was missed still gets delivered once:
    // inject an unrelated frame first (replaces the dedup slot), then only
    // the repetition of a different telegram.
    datalink::LDataFrame other = frame;
    other.repeated = false;
    other.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x3E});
    uint8_t otherRaw[23];
    auto encOther = datalink::FrameCodec::encodeFrame(other, std::span<uint8_t>(otherRaw));
    TEST_ASSERT_TRUE(encOther.isOk());
    physicalLayer->injectFrame(std::vector<uint8_t>(otherRaw, otherRaw + encOther.value()));

    physicalLayer->injectFrame(std::vector<uint8_t>(repetition, repetition + encRepetition.value()));
    stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(4, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(1, stats.duplicatesDropped);
}

int run_all_datalink_edge_case_tests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_statistics_initialization);
    RUN_TEST(test_statistics_tx_counter);
    RUN_TEST(test_statistics_filter_dropped);
    RUN_TEST(test_statistics_rx_counter);
    RUN_TEST(test_custom_configuration);
    RUN_TEST(test_default_configuration);
    RUN_TEST(test_async_physical_progression_via_base_interface);
    RUN_TEST(test_multiple_group_addresses);
    RUN_TEST(test_priority_levels);
    RUN_TEST(test_hop_count_variations);
    RUN_TEST(test_rx_does_not_exhaust_frame_pool);
    RUN_TEST(test_individual_address_targeting);
    RUN_TEST(test_empty_data_payload);
    RUN_TEST(test_maximum_data_payload);
    RUN_TEST(test_repeated_frame_delivered_only_once);
    return UNITY_END();
}

int main() {
    return run_all_datalink_edge_case_tests();
}
