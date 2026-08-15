// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_transport_application.cpp
 * @brief End-to-end test across data link -> network -> transport -> application
 */

#include "unity.h"
#include "knx/types.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/network/network_layer.hpp"
#include "knx/transport/transport_layer.hpp"
#include "knx/application/group_object.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/platform/linux_platform.hpp"
#include "../mocks/mock_physical_layer.hpp"
#include <memory>
#include <vector>

using namespace knx;
using namespace knx::test;

static std::unique_ptr<MockPhysicalLayer> physicalLayer;
static std::unique_ptr<knx::platform::LinuxPlatform> platformInstance;
static std::unique_ptr<datalink::Tp1DataLinkLayer> dlLayer;
static std::unique_ptr<network::NetworkLayer> nwLayer;
static std::unique_ptr<transport::TransportLayer> tpLayer;
static std::unique_ptr<application::GroupObject> groupObj;
static std::vector<transport::TDataFrame> receivedTransport;

static void drainSentFrames() {
    std::vector<uint8_t> tmp;
    while (physicalLayer->tryGetSentFrame(tmp)) {
        // drain
    }
}

static uint8_t ctrlByte(std::span<const uint8_t> frame) {
    return frame.empty() ? 0 : frame[0];
}

void setUp(void) {
    platformInstance = std::make_unique<knx::platform::LinuxPlatform>();
    physicalLayer = std::make_unique<MockPhysicalLayer>();
    dlLayer = std::make_unique<datalink::Tp1DataLinkLayer>(*platformInstance, *physicalLayer);
    nwLayer = std::make_unique<network::NetworkLayer>(*dlLayer);
    tpLayer = std::make_unique<transport::TransportLayer>(*nwLayer);
    receivedTransport.clear();

    // Application group object
    application::GroupObjectConfig cfg{};
    cfg.address = GroupAddress(5, 3, 7);
    cfg.dpt = application::dptids::Bool;
    cfg.flags.read = true;
    cfg.flags.write = true;
    cfg.flags.transmit = false;
    cfg.flags.update = true;
    groupObj = std::make_unique<application::GroupObject>(cfg);

    // Transport callback pushes frames and updates the group object
    tpLayer->setReceiveCallback([](const transport::TDataFrame& frame) {
        receivedTransport.push_back(frame);
        // Feed into application object
        TEST_ASSERT_TRUE(groupObj->receiveValue(frame.payload()).isOk());
    });

    TEST_ASSERT_TRUE(dlLayer->init(IndividualAddress(1, 1, 10)).isOk());
    TEST_ASSERT_TRUE(nwLayer->init(IndividualAddress(1, 1, 10)).isOk());
    TEST_ASSERT_TRUE(tpLayer->init(IndividualAddress(1, 1, 10)).isOk());

    dlLayer->clearGroupAddresses();
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(cfg.address).isOk());
}

void tearDown(void) {
    tpLayer->close();
    nwLayer->close();
    dlLayer->close();
    tpLayer.reset();
    nwLayer.reset();
    dlLayer.reset();
    physicalLayer.reset();
    platformInstance.reset();
    groupObj.reset();
    receivedTransport.clear();
}

// Verify inbound group write travels through data link -> network -> transport -> application
void test_transport_application_group_write(void) {
    drainSentFrames();

    datalink::LDataFrame tx;
    tx.source = IndividualAddress(1, 1, 20);
    tx.destination = GroupAddress(5, 3, 7);
    tx.destinationType = AddressType::Group;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    tx.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01}); // DPT1 true
    tx.ackRequested = true;

    // Encode via data link to produce raw bytes
    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(dlLayer->sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(physicalLayer->getSentFrame(raw));
    }

    // Inject as incoming telegram
    physicalLayer->injectFrame(raw);

    // Group-addressed telegrams are delivered locally but must not emit TP1 ACKs.
    std::vector<uint8_t> ack;
    TEST_ASSERT_FALSE(physicalLayer->getSentFrame(ack));

    // Transport callback should have fired
    TEST_ASSERT_EQUAL(1, static_cast<int>(receivedTransport.size()));
    const auto& tFrame = receivedTransport.back();
    TEST_ASSERT_TRUE(tFrame.destinationType == AddressType::Group);
    TEST_ASSERT_EQUAL_UINT16(tx.destination.raw, tFrame.destination.raw);
    TEST_ASSERT_EQUAL_UINT8(tx.payload()[0], tFrame.payload()[0]);

    // Application object updated
    TEST_ASSERT_TRUE(groupObj->isValid());
    TEST_ASSERT_TRUE(groupObj->asBool());
}

int run_all_transport_application_tests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_transport_application_group_write);
    return UNITY_END();
}

int main() {
    return run_all_transport_application_tests();
}
