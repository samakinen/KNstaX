// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_transport_connection.cpp
 * @brief Integration tests for connection-oriented transport layer services
 * 
 * Tests T_Connect, T_Disconnect, T_Data_Connected services
 * per KNX System Specifications 03/03/07 Transport Layer
 */

#include "unity.h"
#include "knx/transport/transport_layer.hpp"
#include "knx/network/network_layer.hpp"
#include "../mocks/mock_network_layer.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include <vector>
#include <memory>
#include <span>

using namespace knx;

static std::unique_ptr<mocks::MockNetworkLayer> networkLayer;
static std::unique_ptr<transport::TransportLayer> tpLayer;
static IndividualAddress ownAddress;
static IndividualAddress remoteAddress;
static std::vector<transport::TDataFrame> receivedFrames;

// Helper to simulate receiving a frame
static void simulateRx(const network::NDataFrame& frame) {
    tpLayer->handleNetworkRx(frame);
}

// Helper to build a connect response frame
static network::NDataFrame buildConnectResponse(const IndividualAddress& source) {
    network::NDataFrame frame;
    frame.dlFrame.source = source;
    frame.dlFrame.destination = GroupAddress(ownAddress.raw);
    frame.dlFrame.destinationType = AddressType::Individual;
    frame.dlFrame.tpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    frame.dlFrame.hopCount = 6;
    return frame;
}

static network::NDataFrame buildAck(const IndividualAddress& source, uint8_t seq) {
    network::NDataFrame frame;
    frame.dlFrame.source = source;
    frame.dlFrame.destination = GroupAddress(ownAddress.raw);
    frame.dlFrame.destinationType = AddressType::Individual;
    frame.dlFrame.tpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::ack(seq),
        application::APCIField(0),
        {}
    );
    frame.dlFrame.hopCount = 6;
    return frame;
}

void setUp(void) {
    networkLayer = std::make_unique<mocks::MockNetworkLayer>();
    tpLayer = std::make_unique<transport::TransportLayer>(*networkLayer);
    
    ownAddress = IndividualAddress(1, 1, 1);  // 1.1.1
    remoteAddress = IndividualAddress(0x1102);  // 1.1.2
    
    auto initRes = tpLayer->init(ownAddress);
    TEST_ASSERT_TRUE(initRes.isOk());
    
    receivedFrames.clear();
    
    // Set up callback to capture received data
    tpLayer->setReceiveCallback([](const transport::TDataFrame& frame) {
        receivedFrames.push_back(frame);
    });
}

void tearDown(void) {
    tpLayer.reset();
    networkLayer.reset();
}

// ============================================================================
// Connection Establishment Tests
// ============================================================================

void test_ConnectInitiation(void) {
    // Test initiating a connection
    auto connRes = tpLayer->connect(remoteAddress);
    TEST_ASSERT_TRUE(connRes.isOk());
    ConnectionIndex connIdx = connRes.value();
    TEST_ASSERT(connIdx.value() < 16);
    
    // Should send T_Connect frame
    TEST_ASSERT_EQUAL(1, networkLayer->getSentFrameCount());
    auto sentFrame = networkLayer->getLastSentFrame();
    TEST_ASSERT_TRUE(sentFrame.dlFrame.tpdu.size() >= 2);
    TEST_ASSERT_TRUE(knx::protocol::unpackTpduHeader(
        sentFrame.dlFrame.tpdu[0], sentFrame.dlFrame.tpdu[1]
    ).tpci.isControl(knx::protocol::TPCIControl::Connect));
}

void test_ConnectDuplicatePrevention(void) {
    // First connection should succeed
    auto connRes1 = tpLayer->connect(remoteAddress);
    TEST_ASSERT_TRUE(connRes1.isOk());
    ConnectionIndex connIdx1 = connRes1.value();
    
    // Second connection to same address should fail
    auto connRes2 = tpLayer->connect(remoteAddress);
    TEST_ASSERT_FALSE(connRes2.isOk());
}

void test_ConnectResponseReceived(void) {
    // Initiate connection
    auto connRes = tpLayer->connect(remoteAddress);
    TEST_ASSERT_TRUE(connRes.isOk());
    ConnectionIndex connIdx = connRes.value();
    
    // Simulate receiving T_Connect response
    auto connResp = buildConnectResponse(remoteAddress);
    simulateRx(connResp);
    
    // Connection should be established
    TEST_ASSERT_TRUE(tpLayer->isConnected(connIdx));
}

void test_ConnectProgressionSeam(void) {
    auto connRes = tpLayer->beginConnect(remoteAddress);
    TEST_ASSERT_TRUE(connRes.isOk());
    ConnectionIndex connIdx = connRes.value();

    auto progress = tpLayer->pollConnect();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(transport::TransportLayer::ControlSendProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_EQUAL(1, networkLayer->getSentFrameCount());
    auto sentFrame = networkLayer->getLastSentFrame();
    TEST_ASSERT_TRUE(sentFrame.dlFrame.tpdu.size() >= 2);
    TEST_ASSERT_TRUE(knx::protocol::unpackTpduHeader(
        sentFrame.dlFrame.tpdu[0], sentFrame.dlFrame.tpdu[1]
    ).tpci.isControl(knx::protocol::TPCIControl::Connect));

    simulateRx(buildConnectResponse(remoteAddress));
    TEST_ASSERT_TRUE(tpLayer->isConnected(connIdx));
}

// ============================================================================
// Connection Termination Tests
// ============================================================================

void test_DisconnectInitiation(void) {
    // Establish connection first
    auto connRes = tpLayer->connect(remoteAddress);
    TEST_ASSERT_TRUE(connRes.isOk());
    ConnectionIndex connIdx = connRes.value();
    auto connResp = buildConnectResponse(remoteAddress);
    simulateRx(connResp);
    
    networkLayer->clearSentFrames();
    
    // Disconnect
    auto discRes = tpLayer->disconnect(connIdx);
    TEST_ASSERT_TRUE(discRes.isOk());
    
    // Should send T_Disconnect frame
    TEST_ASSERT_EQUAL(1, networkLayer->getSentFrameCount());
    auto sentFrame = networkLayer->getLastSentFrame();
    TEST_ASSERT_TRUE(sentFrame.dlFrame.tpdu.size() >= 2);
    TEST_ASSERT_TRUE(knx::protocol::unpackTpduHeader(
        sentFrame.dlFrame.tpdu[0], sentFrame.dlFrame.tpdu[1]
    ).tpci.isControl(knx::protocol::TPCIControl::Disconnect));
}

void test_DisconnectInvalidIndex(void) {
    // Try to disconnect non-existent connection
    auto discRes = tpLayer->disconnect(ConnectionIndex(15));
    TEST_ASSERT_FALSE(discRes.isOk());
    
    // Should not send anything
    TEST_ASSERT_EQUAL(0, networkLayer->getSentFrameCount());
}

// ============================================================================
// Data Transfer Tests
// ============================================================================

void test_SendConnectedData(void) {
    // Establish connection
    auto connRes = tpLayer->connect(remoteAddress);
    TEST_ASSERT_TRUE(connRes.isOk());
    ConnectionIndex connIdx = connRes.value();
    auto connResp = buildConnectResponse(remoteAddress);
    simulateRx(connResp);
    
    networkLayer->clearSentFrames();
    
    // Send data
    constexpr std::array<uint8_t, 4> data = {0x01, 0x02, 0x03, 0x04};
    auto sendRes = tpLayer->sendConnectedData(connIdx, data);
    TEST_ASSERT_TRUE(sendRes.isOk());
    
    // Should send numbered data frame
    TEST_ASSERT_EQUAL(1, networkLayer->getSentFrameCount());
    auto sentFrame = networkLayer->getLastSentFrame();
    TEST_ASSERT_TRUE(sentFrame.dlFrame.tpdu.size() >= 2);
    const auto header = knx::protocol::unpackTpduHeader(sentFrame.dlFrame.tpdu[0], sentFrame.dlFrame.tpdu[1]);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, header.tpci.type());
    TEST_ASSERT_EQUAL(data.size(), knx::protocol::tpduPayloadLength(sentFrame.dlFrame.tpdu));
}

void test_SendDataWhenNotConnected(void) {
    // Try to send data without connection
    constexpr std::array<uint8_t, 2> data = {0x01, 0x02};
    auto sendRes = tpLayer->sendConnectedData(ConnectionIndex(0), data);
    TEST_ASSERT_FALSE(sendRes.isOk());
    
    // Should not send anything
    TEST_ASSERT_EQUAL(0, networkLayer->getSentFrameCount());
}

void test_SequenceNumberIncrement(void) {
    // Establish connection
    auto connRes = tpLayer->connect(remoteAddress);
    TEST_ASSERT_TRUE(connRes.isOk());
    ConnectionIndex connIdx = connRes.value();
    auto connResp = buildConnectResponse(remoteAddress);
    simulateRx(connResp);
    
    networkLayer->clearSentFrames();
    
    // Send multiple data frames
    constexpr std::array<uint8_t, 1> data = {0x01};
    TEST_ASSERT_TRUE(tpLayer->sendConnectedData(connIdx, data).isOk());
    auto sent1 = networkLayer->getLastSentFrame();
    auto h1 = knx::protocol::unpackTpduHeader(sent1.dlFrame.tpdu[0], sent1.dlFrame.tpdu[1]);
    uint8_t seq1 = h1.tpci.seqNum();
    simulateRx(buildAck(remoteAddress, seq1));
    
    TEST_ASSERT_TRUE(tpLayer->sendConnectedData(connIdx, data).isOk());
    auto sent2 = networkLayer->getLastSentFrame();
    auto h2 = knx::protocol::unpackTpduHeader(sent2.dlFrame.tpdu[0], sent2.dlFrame.tpdu[1]);
    uint8_t seq2 = h2.tpci.seqNum();
    simulateRx(buildAck(remoteAddress, seq2));
    
    TEST_ASSERT_TRUE(tpLayer->sendConnectedData(connIdx, data).isOk());
    auto sent3 = networkLayer->getLastSentFrame();
    auto h3 = knx::protocol::unpackTpduHeader(sent3.dlFrame.tpdu[0], sent3.dlFrame.tpdu[1]);
    uint8_t seq3 = h3.tpci.seqNum();
    
    // Sequences should increment (modulo 16)
    TEST_ASSERT_EQUAL((seq1 + 1) % 16, seq2);
    TEST_ASSERT_EQUAL((seq2 + 1) % 16, seq3);
}

void test_SendConnectedDataProgressionSeam(void) {
    auto connRes = tpLayer->connect(remoteAddress);
    TEST_ASSERT_TRUE(connRes.isOk());
    ConnectionIndex connIdx = connRes.value();
    simulateRx(buildConnectResponse(remoteAddress));

    networkLayer->clearSentFrames();

    constexpr std::array<uint8_t, 2> data = {0x21, 0x22};
    auto beginRes = tpLayer->beginSendConnectedData(connIdx, data);
    TEST_ASSERT_TRUE(beginRes.isOk());

    auto progress = tpLayer->pollConnectedDataSend();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(transport::TransportLayer::ConnectedSendProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_EQUAL(1, networkLayer->getSentFrameCount());
    auto sentFrame = networkLayer->getLastSentFrame();
    TEST_ASSERT_TRUE(sentFrame.dlFrame.tpdu.size() >= 2);
    const auto header = knx::protocol::unpackTpduHeader(sentFrame.dlFrame.tpdu[0], sentFrame.dlFrame.tpdu[1]);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, header.tpci.type());
    TEST_ASSERT_EQUAL(data.size(), knx::protocol::tpduPayloadLength(sentFrame.dlFrame.tpdu));
}

void test_ProcessRetransmissionsProgressionSeam(void) {
    auto connRes = tpLayer->connect(remoteAddress);
    TEST_ASSERT_TRUE(connRes.isOk());
    ConnectionIndex connIdx = connRes.value();
    simulateRx(buildConnectResponse(remoteAddress));

    constexpr std::array<uint8_t, 2> data = {0x31, 0x32};
    TEST_ASSERT_TRUE(tpLayer->sendConnectedData(connIdx, data).isOk());

    const int initialSent = static_cast<int>(networkLayer->getSentFrameCount());

    auto beginRes = tpLayer->beginProcessRetransmissions(5000u);
    TEST_ASSERT_TRUE(beginRes.isOk());

    auto progress = tpLayer->pollProcessRetransmissions();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(transport::TransportLayer::ControlSendProgressState::Pending),
                      static_cast<int>(progress.value()));

    progress = tpLayer->pollProcessRetransmissions();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(transport::TransportLayer::ControlSendProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_EQUAL(initialSent + 1, static_cast<int>(networkLayer->getSentFrameCount()));
    auto sentFrame = networkLayer->getLastSentFrame();
    TEST_ASSERT_TRUE(sentFrame.dlFrame.tpdu.size() >= 2);
    const auto header = knx::protocol::unpackTpduHeader(sentFrame.dlFrame.tpdu[0], sentFrame.dlFrame.tpdu[1]);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, header.tpci.type());
}

// ============================================================================
// Multi-Connection Tests
// ============================================================================

void test_MultipleSimultaneousConnections(void) {
    // Create connections to different devices
    IndividualAddress remote1(0x1102);
    IndividualAddress remote2(0x1103);
    IndividualAddress remote3(0x1104);
    
    auto connRes1 = tpLayer->connect(remote1);
    auto connRes2 = tpLayer->connect(remote2);
    auto connRes3 = tpLayer->connect(remote3);
    TEST_ASSERT_TRUE(connRes1.isOk());
    TEST_ASSERT_TRUE(connRes2.isOk());
    TEST_ASSERT_TRUE(connRes3.isOk());
    ConnectionIndex conn1 = connRes1.value();
    ConnectionIndex conn2 = connRes2.value();
    ConnectionIndex conn3 = connRes3.value();
    
    // All should be different indices
    TEST_ASSERT_NOT_EQUAL(conn1.value(), conn2.value());
    TEST_ASSERT_NOT_EQUAL(conn1.value(), conn3.value());
    TEST_ASSERT_NOT_EQUAL(conn2.value(), conn3.value());
    
    // Confirm all connections
    simulateRx(buildConnectResponse(remote1));
    simulateRx(buildConnectResponse(remote2));
    simulateRx(buildConnectResponse(remote3));
    
    TEST_ASSERT_TRUE(tpLayer->isConnected(conn1));
    TEST_ASSERT_TRUE(tpLayer->isConnected(conn2));
    TEST_ASSERT_TRUE(tpLayer->isConnected(conn3));
}

void test_ConnectionTableLimit(void) {
    // Transport is initialized with max 4 connections
    for (int i = 0; i < 4; i++) {
        auto connRes = tpLayer->connect(IndividualAddress(static_cast<uint16_t>(0x1100 + i)));
        TEST_ASSERT_TRUE(connRes.isOk());
    }
    
    // 5th connection should fail
    auto connRes = tpLayer->connect(IndividualAddress(0x1104));
    TEST_ASSERT_FALSE(connRes.isOk());
}

// ============================================================================
// Error Handling Tests
// ============================================================================

void test_InvalidConnectionIndex(void) {
    // Check non-existent connection
    TEST_ASSERT_FALSE(tpLayer->isConnected(ConnectionIndex(15)));
}

// ============================================================================
// Frame Type Routing Tests
// ============================================================================

void test_TPCIExtraction(void) {
    // Verify TPCI type extraction for different frame types
    
    // UnnumberedData (0x00)
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::UnnumberedData, transport::TransportLayer::extractTPCI(knx::protocol::TPCIField(0x00)));
    
    // NumberedData (0x40 - 0x7F)
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, transport::TransportLayer::extractTPCI(knx::protocol::TPCIField(0x4C)));  // Seq 3
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, transport::TransportLayer::extractTPCI(knx::protocol::TPCIField(0x7F)));  // Seq 15
    
    // UnnumberedControl (0x80 - 0xBF)
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::UnnumberedControl, transport::TransportLayer::extractTPCI(knx::protocol::TPCIField(0x80)));  // Connect
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::UnnumberedControl, transport::TransportLayer::extractTPCI(knx::protocol::TPCIField(0x81)));  // Disconnect
    
    // NumberedControl (0xC0 - 0xFF)
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedControl, transport::TransportLayer::extractTPCI(knx::protocol::TPCIField(0xC2)));  // ACK
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedControl, transport::TransportLayer::extractTPCI(knx::protocol::TPCIField(0xC3)));  // NAK
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    UNITY_BEGIN();
    
    // Connection establishment
    RUN_TEST(test_ConnectInitiation);
    RUN_TEST(test_ConnectDuplicatePrevention);
    RUN_TEST(test_ConnectResponseReceived);
    RUN_TEST(test_ConnectProgressionSeam);
    
    // Connection termination
    RUN_TEST(test_DisconnectInitiation);
    RUN_TEST(test_DisconnectInvalidIndex);
    
    // Data transfer
    RUN_TEST(test_SendConnectedData);
    RUN_TEST(test_SendConnectedDataProgressionSeam);
    RUN_TEST(test_ProcessRetransmissionsProgressionSeam);
    RUN_TEST(test_SendDataWhenNotConnected);
    RUN_TEST(test_SequenceNumberIncrement);
    
    // Multi-connection
    RUN_TEST(test_MultipleSimultaneousConnections);
    RUN_TEST(test_ConnectionTableLimit);
    
    // Error handling
    RUN_TEST(test_InvalidConnectionIndex);
    
    // Frame routing
    RUN_TEST(test_TPCIExtraction);
    
    return UNITY_END();
}
