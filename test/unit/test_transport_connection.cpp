// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_transport_connection.cpp
 * @brief Unit tests for transport-layer connection-oriented semantics
 */

#include "unity.h"

#include "knx/transport/transport_layer.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/constants.hpp"

#include "../mocks/mock_network_layer.hpp"

#include <span>

using namespace knx;
using namespace knx::transport;

static std::unique_ptr<knx::mocks::MockNetworkLayer> nw;
static std::unique_ptr<TransportLayer> tl;

static std::vector<TDataFrame> rx;

static network::NDataFrame makeNFrame(uint16_t src, uint16_t dst, std::span<const uint8_t> tpdu) {
    network::NDataFrame f;
    f.dlFrame.source = IndividualAddress(src);
    f.dlFrame.destination = GroupAddress(dst);
    f.dlFrame.destinationType = AddressType::Individual;
    f.dlFrame.tpdu.assign(tpdu);
    f.dlFrame.hopCount = 6;
    return f;
}

static network::NDataFrame buildNak(uint16_t src, uint16_t dst, uint8_t seq) {
    return makeNFrame(
        src,
        dst,
        knx::protocol::buildTpdu(
            knx::protocol::TPCIField::nak(seq),
            application::APCIField(0),
            {}
        )
    );
}

static network::NDataFrame buildAck(uint16_t src, uint16_t dst, uint8_t seq) {
    return makeNFrame(
        src,
        dst,
        knx::protocol::buildTpdu(
            knx::protocol::TPCIField::ack(seq),
            application::APCIField(0),
            {}
        )
    );
}

void setUp(void) {
    nw = std::make_unique<knx::mocks::MockNetworkLayer>();
    tl = std::make_unique<TransportLayer>(*nw);
    rx.clear();

    auto initRes = tl->init(IndividualAddress(1, 1, 10));
    TEST_ASSERT_TRUE(initRes.isOk());
    tl->setReceiveCallback([](const TDataFrame& f) {
        rx.push_back(f);
    });

    nw->clearSentFrames();
}

void tearDown(void) {
    tl->close();
    tl.reset();
    nw.reset();
    rx.clear();
}

void test_ConnectResponseDoesNotEchoConnect(void) {
    // Establish connection (local initiator)
    const IndividualAddress remote(0x1114);
    auto connRes = tl->connect(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex idx = connRes.value();
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());

    // Simulate periodic timebase update
    tl->processRetransmissions(100);

    // Remote sends CONNECT (symmetric on-wire) as response.
    const auto tpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, tpdu));

    // Must be connected, and must not send an extra CONNECT back.
    TEST_ASSERT_TRUE(tl->isConnected(idx));
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
}

void test_SingleByteInboundConnectCreatesConnectionAndSendsResponse(void) {
    const IndividualAddress remote(0x1114);
    const uint8_t connectTpdu[] = {static_cast<uint8_t>(knx::protocol::TPCIControl::Connect)};

    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, std::span<const uint8_t>(connectTpdu, 1u)));

    // Server role accepts silently (KNX 03.03.04 §5.1 E00→A1: no T_Connect
    // echo on the bus). The connection must still exist and accept data.
    TEST_ASSERT_EQUAL_UINT32(0, nw->getSentFrameCount());

    const auto dataTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, 0),
        application::APCIField(0),
        {0x5A}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, dataTpdu));

    // The numbered data is T_ACKed (proving the connection was created) and
    // delivered upward.
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    const auto sent = nw->getLastSentFrame();
    TEST_ASSERT_TRUE(sent.dlFrame.tpdu.size() >= 1u);
    const auto sentHdr = knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0],
                                                          sent.dlFrame.tpdu.size() > 1u ? sent.dlFrame.tpdu[1] : 0u);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedControl, sentHdr.tpci.type());
    TEST_ASSERT_TRUE(sentHdr.tpci.isAck());

    TEST_ASSERT_EQUAL_UINT32(1, rx.size());
    TEST_ASSERT_EQUAL_UINT16(remote.raw, rx.front().source.raw);
    TEST_ASSERT_EQUAL_UINT8(0u, rx.front().sequenceNumber);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(rx.front().payload().size()));
    TEST_ASSERT_EQUAL_UINT8(0x5A, rx.front().payload()[0]);
}

void test_DisconnectResponseDoesNotEchoDisconnect(void) {
    const IndividualAddress remote(0x1114);
    auto connRes = tl->connect(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex idx = connRes.value();
    tl->processRetransmissions(100);

    // Become connected
    const auto connTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, connTpdu));
    TEST_ASSERT_TRUE(tl->isConnected(idx));

    nw->clearSentFrames();

    // Local requests disconnect -> sends DISCONNECT
    TEST_ASSERT_TRUE(tl->disconnect(idx).isOk());
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());

    // Remote sends DISCONNECT as response; must not echo another DISCONNECT.
    const auto discTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Disconnect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, discTpdu));
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
}

void test_SeqMismatchSendsNakAndDoesNotDeliver(void) {
    const IndividualAddress remote(0x1114);
    auto connRes = tl->connect(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex idx = connRes.value();
    tl->processRetransmissions(100);

    // Become connected
    const auto connTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, connTpdu));
    TEST_ASSERT_TRUE(tl->isConnected(idx));

    nw->clearSentFrames();
    rx.clear();

    // Send numbered data with wrong seq (expected 0)
    const uint8_t wrongSeq = 5;
    const auto dataTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, wrongSeq),
        application::APCIField(0),
        {0xAA}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, dataTpdu));

    // Should send a T_NAK carrying the received sequence number (03_03_04
    // A4) and not deliver. This test used to expect the sequence number the
    // device was waiting for (0), which the partner cannot match to anything.
    // Control TPDUs (T_ACK/T_NAK) are exactly 1 byte on the wire.
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    const auto sent = nw->getLastSentFrame();
    TEST_ASSERT_TRUE(sent.dlFrame.tpdu.size() >= 1);
    const auto hdr = knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0],
                                                     sent.dlFrame.tpdu.size() > 1u ? sent.dlFrame.tpdu[1] : 0u);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedControl, hdr.tpci.type());
    TEST_ASSERT_EQUAL_UINT8(wrongSeq, hdr.tpci.seqNum());
    TEST_ASSERT_TRUE(hdr.tpci.isNak());
    TEST_ASSERT_EQUAL_UINT32(0, rx.size());
}

void test_DuplicateSuppressionAcksOnly(void) {
    const IndividualAddress remote(0x1114);
    auto connRes = tl->connect(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex idx = connRes.value();
    tl->processRetransmissions(100);

    // Become connected
    const auto connTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, connTpdu));
    TEST_ASSERT_TRUE(tl->isConnected(idx));

    nw->clearSentFrames();
    rx.clear();

    // Send seq 0 -> should ACK and deliver
    const uint8_t seq = 0;
    const auto dataTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, seq),
        application::APCIField(0),
        {0x01}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, dataTpdu));
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    TEST_ASSERT_EQUAL_UINT32(1, rx.size());

    // Send same seq again -> should ACK but not deliver again
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, dataTpdu));
    TEST_ASSERT_EQUAL_UINT32(2, nw->getSentFrameCount());
    TEST_ASSERT_EQUAL_UINT32(1, rx.size());
}

void test_ReceiveQueueDeliversNumberedDataWithoutCallback(void) {
    const IndividualAddress remote(0x1114);
    auto connRes = tl->connect(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex idx = connRes.value();
    tl->processRetransmissions(100);

    const auto connTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, connTpdu));
    TEST_ASSERT_TRUE(tl->isConnected(idx));

    tl->setReceiveCallback(nullptr);
    rx.clear();
    nw->clearSentFrames();

    const auto dataTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, 0),
        application::APCIField(0),
        {0xCA, 0xFE}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, dataTpdu));

    TEST_ASSERT_EQUAL_UINT32(0, rx.size());
    TEST_ASSERT_EQUAL_UINT32(1, tl->queuedReceiveCount());

    TDataFrame queued;
    TEST_ASSERT_TRUE(tl->popReceivedFrame(queued));
    TEST_ASSERT_EQUAL_UINT16(remote.raw, queued.source.raw);
    TEST_ASSERT_EQUAL_UINT8(0, queued.sequenceNumber);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(queued.payload().size()));
    TEST_ASSERT_EQUAL_UINT8(0xCA, queued.payload()[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFE, queued.payload()[1]);
    TEST_ASSERT_EQUAL_UINT32(0, tl->queuedReceiveCount());
}

void test_ReceiveQueueDropsNewestWhenFull(void) {
    tl->setReceiveCallback(nullptr);
    rx.clear();

    for (uint8_t value = 0u; value < 9u; ++value) {
        const auto tpdu = knx::protocol::buildTpdu(
            knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
            application::APCIField(0),
            {static_cast<uint8_t>(0xA0u + value)}
        );
        tl->handleNetworkRx(makeNFrame(static_cast<uint16_t>(0x1114u + value), 0x110A, tpdu));
    }

    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(rx.size()));
    TEST_ASSERT_EQUAL_UINT32(8u, static_cast<uint32_t>(tl->queuedReceiveCount()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(tl->droppedReceiveFrameCount()));

    TDataFrame queued;
    TEST_ASSERT_TRUE(tl->popReceivedFrame(queued));
    TEST_ASSERT_EQUAL_UINT16(0x1114u, queued.source.raw);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(queued.payload().size()));
    TEST_ASSERT_EQUAL_UINT8(0xA0u, queued.payload()[0]);
}

void test_RetransmitAfterTimeoutResendsSameSeq(void) {
    const IndividualAddress remote(0x1114);
    auto connRes = tl->connect(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex idx = connRes.value();

    tl->processRetransmissions(1000);
    const auto connTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, connTpdu));
    TEST_ASSERT_TRUE(tl->isConnected(idx));

    nw->clearSentFrames();

    // Send connected data: initial seq should be 0.
    TEST_ASSERT_TRUE(tl->sendConnectedData(idx, {0x55}).isOk());
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    auto first = nw->getLastSentFrame();
    auto firstHdr = knx::protocol::unpackTpduHeader(first.dlFrame.tpdu[0], first.dlFrame.tpdu[1]);
    const uint8_t firstSeq = firstHdr.tpci.seqNum();

    // Advance time past retransmit timeout; should resend same seq.
    tl->processRetransmissions(1000 + 3000);
    TEST_ASSERT_TRUE(nw->getSentFrameCount() >= 2);
    auto second = nw->getLastSentFrame();
    auto secondHdr = knx::protocol::unpackTpduHeader(second.dlFrame.tpdu[0], second.dlFrame.tpdu[1]);
    const uint8_t secondSeq = secondHdr.tpci.seqNum();
    TEST_ASSERT_EQUAL_UINT8(firstSeq, secondSeq);
}

void test_MatchingNakSchedulesImmediateRetransmitWithSameSeq(void) {
    const IndividualAddress remote(0x1114);
    auto connRes = tl->connect(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex idx = connRes.value();

    tl->processRetransmissions(1000);
    const auto connTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, connTpdu));
    TEST_ASSERT_TRUE(tl->isConnected(idx));

    nw->clearSentFrames();

    TEST_ASSERT_TRUE(tl->sendConnectedData(idx, {0x66}).isOk());
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    const auto first = nw->getLastSentFrame();
    const auto firstHdr = knx::protocol::unpackTpduHeader(first.dlFrame.tpdu[0], first.dlFrame.tpdu[1]);
    const uint8_t seq = firstHdr.tpci.seqNum();

    // Move transport timebase forward so NAK can schedule an immediate retry on the next tick.
    tl->processRetransmissions(5000);
    tl->handleNetworkRx(buildNak(remote.raw, 0x110A, seq));
    tl->processRetransmissions(5000);

    TEST_ASSERT_TRUE(nw->getSentFrameCount() >= 2);
    const auto resent = nw->getLastSentFrame();
    const auto resentHdr = knx::protocol::unpackTpduHeader(resent.dlFrame.tpdu[0], resent.dlFrame.tpdu[1]);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, resentHdr.tpci.type());
    TEST_ASSERT_EQUAL_UINT8(seq, resentHdr.tpci.seqNum());
}

void test_AckClearsRetransmissionSoLaterTimeoutDoesNotResend(void) {
    const IndividualAddress remote(0x1114);
    auto connRes = tl->connect(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex idx = connRes.value();

    tl->processRetransmissions(1000);
    const auto connTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, connTpdu));
    TEST_ASSERT_TRUE(tl->isConnected(idx));

    nw->clearSentFrames();

    TEST_ASSERT_TRUE(tl->sendConnectedData(idx, {0x77}).isOk());
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    const auto first = nw->getLastSentFrame();
    const auto firstHdr = knx::protocol::unpackTpduHeader(first.dlFrame.tpdu[0], first.dlFrame.tpdu[1]);
    const uint8_t seq = firstHdr.tpci.seqNum();

    tl->handleNetworkRx(buildAck(remote.raw, 0x110A, seq));
    // Past the acknowledge timeout but inside the 6 s connection timeout: an
    // idle connection now times out with T_Disconnect (E16), which is not
    // what this test is about.
    tl->processRetransmissions(1000 + knx::constants::timing::ACK_TIMEOUT_MS + 1000);

    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
}

void test_ConnectionlessSendProgressionSeam(void) {
    nw->clearSentFrames();

    TDataFrame frame;
    frame.service = TDataService::Group;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(1, 2, 3);
    frame.destinationType = AddressType::Group;
    frame.tpdu.clear();
    (void)knx::protocol::buildTpduInPlace(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        application::APCIField::create(application::APCIService::GroupValueWrite, 0x01),
        std::span<const uint8_t>{},
        frame.tpdu);

    auto beginRes = tl->beginTransmit(frame);
    TEST_ASSERT_TRUE(beginRes.isOk());

    auto progress = tl->pollTransmit();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(TransportLayer::SendProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    const auto sent = nw->getLastSentFrame();
    TEST_ASSERT_TRUE(sent.dlFrame.tpdu.size() >= 2);
    const auto hdr = knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0], sent.dlFrame.tpdu[1]);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::UnnumberedData, hdr.tpci.type());
}

namespace {

network::NDataFrame buildInboundConnect(uint16_t src, bool repeated) {
    auto frame = makeNFrame(
        src,
        0x110A,
        knx::protocol::buildTpdu(
            knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
            application::APCIField(0),
            {}
        )
    );
    frame.dlFrame.repeated = repeated;
    return frame;
}

network::NDataFrame buildInboundData(uint16_t src, uint8_t seq, uint8_t value) {
    return makeNFrame(
        src,
        0x110A,
        knx::protocol::buildTpdu(
            knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, seq),
            application::APCIField(0),
            {value}
        )
    );
}

}  // namespace

// A management client whose own state machine timed out re-opens the connection
// with a fresh T_Connect and restarts at sequence 0. Ignoring it (03/03/04
// §5.4.2 E00/OPEN_IDLE → A0) leaves our SeqNoRcv stale, so every frame of the
// new session is NAKed and ETS aborts the download.
void test_NewConnectWhileConnectedResynchronisesSequenceNumbers(void) {
    const IndividualAddress remote(0x1114);
    tl->handleNetworkRx(buildInboundConnect(remote.raw, false));

    tl->handleNetworkRx(buildInboundData(remote.raw, 0, 0x01));
    TEST_ASSERT_EQUAL_UINT32(1, rx.size());

    nw->clearSentFrames();
    rx.clear();

    // Peer reconnects and starts over at seq 0.
    tl->handleNetworkRx(buildInboundConnect(remote.raw, false));
    tl->handleNetworkRx(buildInboundData(remote.raw, 0, 0x02));

    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    const auto sent = nw->getLastSentFrame();
    const auto hdr = knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0],
                                                     sent.dlFrame.tpdu.size() > 1u ? sent.dlFrame.tpdu[1] : 0u);
    TEST_ASSERT_TRUE(hdr.tpci.isAck());
    TEST_ASSERT_EQUAL_UINT8(0, hdr.tpci.seqNum());
    TEST_ASSERT_EQUAL_UINT32(1, rx.size());
    TEST_ASSERT_EQUAL_UINT8(0x02, rx.front().payload()[0]);
}

// The data link deliberately forwards repetitions addressed to us, so the same
// T_Connect can arrive twice. A repetition must not reset a live session.
void test_RepeatedConnectWhileConnectedKeepsSequenceNumbers(void) {
    const IndividualAddress remote(0x1114);
    tl->handleNetworkRx(buildInboundConnect(remote.raw, false));
    tl->handleNetworkRx(buildInboundData(remote.raw, 0, 0x01));
    TEST_ASSERT_EQUAL_UINT32(1, rx.size());

    nw->clearSentFrames();
    rx.clear();

    tl->handleNetworkRx(buildInboundConnect(remote.raw, true));

    // Still mid-session: seq 1 is next, and a repeat of seq 0 is a duplicate
    // (ACKed, not delivered) rather than the start of a new session.
    tl->handleNetworkRx(buildInboundData(remote.raw, 0, 0x02));
    TEST_ASSERT_EQUAL_UINT32(0, rx.size());

    tl->handleNetworkRx(buildInboundData(remote.raw, 1, 0x03));
    TEST_ASSERT_EQUAL_UINT32(1, rx.size());
    TEST_ASSERT_EQUAL_UINT8(0x03, rx.front().payload()[0]);
}

// A response to a connectionless request must stay connectionless even while a
// connection to the same peer is open (03/03/04 §5).
void test_IndividualSendStaysConnectionlessWhileConnected(void) {
    const IndividualAddress remote(0x1114);
    tl->handleNetworkRx(buildInboundConnect(remote.raw, false));
    nw->clearSentFrames();

    TDataFrame frame;
    frame.service = TDataService::Individual;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(remote.raw);
    frame.destinationType = AddressType::Individual;
    (void)knx::protocol::buildTpduInPlace(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        application::APCIField::create(application::APCIService::DeviceDescriptorResponse, 0),
        std::span<const uint8_t>{},
        frame.tpdu);

    TEST_ASSERT_TRUE(tl->sendFrame(frame).isOk());
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    auto sent = nw->getLastSentFrame();
    auto hdr = knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0], sent.dlFrame.tpdu[1]);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::UnnumberedData, hdr.tpci.type());

    // The same frame marked as connection-oriented does go on the connection.
    frame.service = TDataService::Connected;
    TEST_ASSERT_TRUE(tl->sendFrame(frame).isOk());
    sent = nw->getLastSentFrame();
    hdr = knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0], sent.dlFrame.tpdu[1]);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, hdr.tpci.type());
    TEST_ASSERT_EQUAL_UINT8(0, hdr.tpci.seqNum());
}

// ---------------------------------------------------------------------------
// SeqNoSend ownership
// ---------------------------------------------------------------------------

/// Bring a connection up in the client role and clear the T_Connect from the log.
static ConnectionIndex establishConnection(const IndividualAddress& remote) {
    auto connRes = tl->connect(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex idx = connRes.value();

    const auto connTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, connTpdu));
    TEST_ASSERT_TRUE(tl->isConnected(idx));

    nw->clearSentFrames();
    return idx;
}

/// Send an application answer on the connection, the way ApplicationLayer does.
static uint8_t sendConnectedAnswer(const IndividualAddress& remote, uint8_t payload) {
    TDataFrame frame;
    frame.service = TDataService::Connected;
    frame.destination = GroupAddress(remote.raw);
    frame.destinationType = AddressType::Individual;
    const uint8_t data[1] = {payload};
    (void)knx::protocol::buildTpduInPlace(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        application::APCIField::create(application::APCIService::DeviceDescriptorResponse, 0),
        data,
        frame.tpdu);

    TEST_ASSERT_TRUE(tl->sendFrame(frame).isOk());
    const auto sent = nw->getLastSentFrame();
    const auto hdr = knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0], sent.dlFrame.tpdu[1]);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, hdr.tpci.type());
    return hdr.tpci.seqNum();
}

/**
 * 03/03/04 advances SeqNoSend as part of sending the frame. Tying it to the
 * T_ACK instead meant one lost acknowledgement froze the counter for the rest
 * of the connection, and the peer discarded every later answer as a duplicate.
 */
void test_SendSequenceAdvancesOnTransmitWithoutAck(void) {
    const IndividualAddress remote(0x1114);
    (void)establishConnection(remote);

    TEST_ASSERT_EQUAL_UINT8(0, sendConnectedAnswer(remote, 0x11));
    // No T_ACK in between: the next frame must still carry the next number.
    TEST_ASSERT_EQUAL_UINT8(1, sendConnectedAnswer(remote, 0x22));
    TEST_ASSERT_EQUAL_UINT8(2, sendConnectedAnswer(remote, 0x33));
}

/// ...and the T_ACK must not advance it a second time on top of that.
void test_AckDoesNotAdvanceSendSequenceAgain(void) {
    const IndividualAddress remote(0x1114);
    (void)establishConnection(remote);

    TEST_ASSERT_EQUAL_UINT8(0, sendConnectedAnswer(remote, 0x11));
    tl->handleNetworkRx(buildAck(remote.raw, 0x110A, 0));
    TEST_ASSERT_EQUAL_UINT8(1, sendConnectedAnswer(remote, 0x22));

    tl->handleNetworkRx(buildAck(remote.raw, 0x110A, 1));
    TEST_ASSERT_EQUAL_UINT8(2, sendConnectedAnswer(remote, 0x33));
}

/**
 * An installed clock makes the owner loop's background-work pass service the
 * connection timers. Without it nothing in the stack calls
 * processRetransmissions(), so an unacknowledged frame was never resent.
 */
void test_InstalledClockRetransmitsFromBackgroundWork(void) {
    uint32_t fakeNowMs = 1000;
    tl->setClock([&fakeNowMs]() { return fakeNowMs; });

    const IndividualAddress remote(0x1114);
    const ConnectionIndex idx = establishConnection(remote);

    TEST_ASSERT_TRUE(tl->sendConnectedData(idx, {0x55}).isOk());
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    const auto first = nw->getLastSentFrame();
    const uint8_t seq =
        knx::protocol::unpackTpduHeader(first.dlFrame.tpdu[0], first.dlFrame.tpdu[1]).tpci.seqNum();

    // Nothing due yet.
    tl->processBackgroundWork();
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());

    // Past the T_ACK timeout the background pass alone must resend it.
    fakeNowMs += knx::constants::timing::ACK_TIMEOUT_MS;
    tl->processBackgroundWork();
    TEST_ASSERT_EQUAL_UINT32(2, nw->getSentFrameCount());

    const auto resent = nw->getLastSentFrame();
    const auto resentHdr =
        knx::protocol::unpackTpduHeader(resent.dlFrame.tpdu[0], resent.dlFrame.tpdu[1]);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, resentHdr.tpci.type());
    TEST_ASSERT_EQUAL_UINT8(seq, resentHdr.tpci.seqNum());
}

/**
 * A T_Data_Connected with no connection behind it is dropped, not refused.
 *
 * Regression in both directions. The device used to drop it silently, which on
 * a lost T_Connect left ETS waiting out its own timeout; answering T_Disconnect
 * instead looked like the obvious fix and 03/03/04 prescribes it. On KNXnet/IP
 * routing it made things worse: ETS opens a connection by sending T_Connect and
 * the first request two milliseconds apart, multicast loses one of that pair
 * routinely, and ETS recovers by timing out after 3 s and reconnecting. A
 * T_Disconnect ends that retry loop — ETS reports "connection refused" and
 * abandons the download rather than trying again.
 */
void test_NumberedDataWithoutConnectionIsDroppedNotRefused(void) {
    const IndividualAddress remote(0x1114);
    TEST_ASSERT_FALSE(tl->isConnected(ConnectionIndex(0)));

    const auto dataTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, 0),
        application::APCIField(0),
        {0x5A}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, dataTpdu));

    // Nothing on the wire at all: no T_Disconnect, no T_ACK, no NAK. The peer's
    // own timeout is what recovers this, and it recovers by reconnecting.
    TEST_ASSERT_EQUAL_UINT32(0, nw->getSentFrameCount());
    // And nothing delivered upward: dropping is not the same as accepting.
    TEST_ASSERT_EQUAL_UINT32(0, rx.size());
}

/// The reconnect that follows gets a working connection, which is the whole
/// point of leaving the peer's retry alone.
void test_ConnectionWorksAfterDroppedNumberedData(void) {
    const IndividualAddress remote(0x1114);

    const auto dataTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, 0),
        application::APCIField(0),
        {0x5A}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, dataTpdu));
    nw->clearSentFrames();

    const auto connTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
        application::APCIField(0),
        {}
    );
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, connTpdu));
    tl->handleNetworkRx(makeNFrame(remote.raw, 0x110A, dataTpdu));

    const auto sent = nw->getLastSentFrame();
    const auto hdr = knx::protocol::unpackTpduHeader(
        sent.dlFrame.tpdu[0], sent.dlFrame.tpdu.size() > 1u ? sent.dlFrame.tpdu[1] : 0u);
    TEST_ASSERT_TRUE(hdr.tpci.isAck());
    TEST_ASSERT_EQUAL_UINT32(1, rx.size());
    TEST_ASSERT_EQUAL_UINT8(0x5A, rx.front().payload()[0]);
}

// ---------------------------------------------------------------------------
// 03_03_04 §5 error handling: E06 (A4), E09/E16/E18 (A6), E11 (A0)
// ---------------------------------------------------------------------------

static knx::protocol::TPCIField lastSentTpci() {
    const auto sent = nw->getLastSentFrame();
    TEST_ASSERT_TRUE(sent.dlFrame.tpdu.size() >= 1u);
    return knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0],
                                           sent.dlFrame.tpdu.size() > 1u ? sent.dlFrame.tpdu[1] : 0u)
        .tpci;
}

static bool lastSentIsDisconnectTo(const IndividualAddress& remote) {
    const auto sent = nw->getLastSentFrame();
    return sent.dlFrame.destination.raw == remote.raw
        && lastSentTpci().isControl(knx::protocol::TPCIControl::Disconnect);
}

static network::NDataFrame buildData(uint16_t src, uint8_t seq, uint8_t payload) {
    return makeNFrame(src, 0x110A,
                      knx::protocol::buildTpdu(
                          knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, seq),
                          application::APCIField(0), {payload}));
}

/// E16: the 6 s connection timeout closes with T_Disconnect (A6), not silently.
void test_ConnectionTimeoutSendsDisconnect(void) {
    tl->processRetransmissions(1000);
    const IndividualAddress remote(0x1114);
    const ConnectionIndex idx = establishConnection(remote);

    tl->processRetransmissions(1000 + knx::constants::timing::CONNECTION_TIMEOUT_MS - 1);
    TEST_ASSERT_TRUE(tl->isConnected(idx));
    TEST_ASSERT_EQUAL_UINT32(0, nw->getSentFrameCount());

    tl->processRetransmissions(1000 + knx::constants::timing::CONNECTION_TIMEOUT_MS);
    TEST_ASSERT_FALSE(tl->isConnected(idx));
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    TEST_ASSERT_TRUE(lastSentIsDisconnectTo(remote));
}

/// E17 three times at a fixed 3 s, then E18 closes with T_Disconnect (A6).
void test_RepetitionsExhaustedSendsDisconnect(void) {
    uint32_t now = 1000;
    tl->processRetransmissions(now);
    const IndividualAddress remote(0x1114);
    const ConnectionIndex idx = establishConnection(remote);

    TEST_ASSERT_TRUE(tl->sendConnectedData(idx, {0x55}).isOk());
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());

    for (uint32_t rep = 1; rep <= knx::constants::timing::CONNECTION_MAX_REPETITIONS; ++rep) {
        now += knx::constants::timing::ACK_TIMEOUT_MS;
        tl->processRetransmissions(now);
        TEST_ASSERT_EQUAL_UINT32(1 + rep, nw->getSentFrameCount());
        TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedData, lastSentTpci().type());
        TEST_ASSERT_TRUE(tl->isConnected(idx));
    }

    now += knx::constants::timing::ACK_TIMEOUT_MS;
    tl->processRetransmissions(now);
    TEST_ASSERT_FALSE(tl->isConnected(idx));
    TEST_ASSERT_TRUE(lastSentIsDisconnectTo(remote));
}

/// Every way a connection ends is reported to the closed-connection callback.
void test_ClosedCallbackSeesTimeoutAndPartnerDisconnect(void) {
    std::vector<uint16_t> closed;
    tl->setConnectionClosedCallback([&closed](const IndividualAddress& partner) {
        closed.push_back(partner.raw);
    });

    tl->processRetransmissions(1000);
    const IndividualAddress remote(0x1114);
    (void)establishConnection(remote);
    tl->processRetransmissions(1000 + knx::constants::timing::CONNECTION_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT32(1, closed.size());
    TEST_ASSERT_EQUAL_HEX16(remote.raw, closed[0]);

    const IndividualAddress other(0x1115);
    const ConnectionIndex idx = establishConnection(other);
    const auto disconnectTpdu = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Disconnect),
        application::APCIField(0),
        {});
    tl->handleNetworkRx(makeNFrame(other.raw, 0x110A, disconnectTpdu));
    TEST_ASSERT_FALSE(tl->isConnected(idx));
    TEST_ASSERT_EQUAL_UINT32(2, closed.size());
    TEST_ASSERT_EQUAL_HEX16(other.raw, closed[1]);

    tl->setConnectionClosedCallback(nullptr);
}

/// E09 in OPEN_WAIT: a T_ACK for a frame we are not waiting for → A6.
void test_WrongSequenceAckWhileWaitingDisconnects(void) {
    tl->processRetransmissions(1000);
    const IndividualAddress remote(0x1114);
    const ConnectionIndex idx = establishConnection(remote);
    TEST_ASSERT_TRUE(tl->sendConnectedData(idx, {0x55}).isOk());  // seq 0 outstanding
    nw->clearSentFrames();

    tl->handleNetworkRx(buildAck(remote.raw, 0x110A, 7));
    TEST_ASSERT_FALSE(tl->isConnected(idx));
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    TEST_ASSERT_TRUE(lastSentIsDisconnectTo(remote));
}

/// A data-link repetition of a stale T_ACK is not E09: our L2 ACK was lost.
void test_RepeatedStaleAckWhileWaitingIsIgnored(void) {
    tl->processRetransmissions(1000);
    const IndividualAddress remote(0x1114);
    const ConnectionIndex idx = establishConnection(remote);
    TEST_ASSERT_TRUE(tl->sendConnectedData(idx, {0x55}).isOk());
    tl->handleNetworkRx(buildAck(remote.raw, 0x110A, 0));
    TEST_ASSERT_TRUE(tl->sendConnectedData(idx, {0x56}).isOk());  // seq 1 outstanding
    nw->clearSentFrames();

    auto repeated = buildAck(remote.raw, 0x110A, 0);
    repeated.dlFrame.repeated = true;
    tl->handleNetworkRx(repeated);
    TEST_ASSERT_TRUE(tl->isConnected(idx));
    TEST_ASSERT_EQUAL_UINT32(0, nw->getSentFrameCount());

    // The real T_ACK still completes the exchange.
    tl->handleNetworkRx(buildAck(remote.raw, 0x110A, 1));
    tl->processRetransmissions(1000 + knx::constants::timing::ACK_TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT32(0, nw->getSentFrameCount());
}

/// E11: a T_NAK with a sequence number other than the outstanding one is
/// ignored (A0, as Style 2); the frame keeps its normal acknowledge timeout.
void test_WrongSequenceNakIsIgnored(void) {
    tl->processRetransmissions(1000);
    const IndividualAddress remote(0x1114);
    const ConnectionIndex idx = establishConnection(remote);
    TEST_ASSERT_TRUE(tl->sendConnectedData(idx, {0x55}).isOk());
    nw->clearSentFrames();

    tl->handleNetworkRx(buildNak(remote.raw, 0x110A, 9));
    tl->processRetransmissions(1001);
    TEST_ASSERT_TRUE(tl->isConnected(idx));
    TEST_ASSERT_EQUAL_UINT32(0, nw->getSentFrameCount());
}

/// A frame T_NAK'd for being out of order and then repeated in order must be
/// delivered. The repeat used to match "last sequence number seen" and was
/// T_ACKed as a duplicate without ever reaching the application.
void test_RepeatOfNakdFrameIsDeliveredNotTreatedAsDuplicate(void) {
    tl->processRetransmissions(1000);
    const IndividualAddress remote(0x1114);
    (void)establishConnection(remote);

    tl->handleNetworkRx(buildData(remote.raw, 0, 0x10));  // accepted
    tl->handleNetworkRx(buildData(remote.raw, 2, 0x12));  // out of order → T_NAK(2)
    TEST_ASSERT_TRUE(lastSentTpci().isNak());
    TEST_ASSERT_EQUAL_UINT8(2, lastSentTpci().seqNum());
    tl->handleNetworkRx(buildData(remote.raw, 1, 0x11));  // in order
    tl->handleNetworkRx(buildData(remote.raw, 2, 0x12));  // the NAK'd frame, now in order

    TEST_ASSERT_EQUAL_UINT32(3, rx.size());
    TEST_ASSERT_EQUAL_UINT8(0x12, rx.back().payload()[0]);

    // And a genuine repeat of the last accepted frame is E05: T_ACK, no delivery.
    tl->handleNetworkRx(buildData(remote.raw, 2, 0x12));
    TEST_ASSERT_TRUE(lastSentTpci().isAck());
    TEST_ASSERT_EQUAL_UINT8(2, lastSentTpci().seqNum());
    TEST_ASSERT_EQUAL_UINT32(3, rx.size());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ConnectionTimeoutSendsDisconnect);
    RUN_TEST(test_ClosedCallbackSeesTimeoutAndPartnerDisconnect);
    RUN_TEST(test_RepetitionsExhaustedSendsDisconnect);
    RUN_TEST(test_WrongSequenceAckWhileWaitingDisconnects);
    RUN_TEST(test_RepeatedStaleAckWhileWaitingIsIgnored);
    RUN_TEST(test_WrongSequenceNakIsIgnored);
    RUN_TEST(test_RepeatOfNakdFrameIsDeliveredNotTreatedAsDuplicate);
    RUN_TEST(test_ConnectResponseDoesNotEchoConnect);
    RUN_TEST(test_SingleByteInboundConnectCreatesConnectionAndSendsResponse);
    RUN_TEST(test_DisconnectResponseDoesNotEchoDisconnect);
    RUN_TEST(test_SeqMismatchSendsNakAndDoesNotDeliver);
    RUN_TEST(test_DuplicateSuppressionAcksOnly);
    RUN_TEST(test_ReceiveQueueDeliversNumberedDataWithoutCallback);
    RUN_TEST(test_ReceiveQueueDropsNewestWhenFull);
    RUN_TEST(test_RetransmitAfterTimeoutResendsSameSeq);
    RUN_TEST(test_MatchingNakSchedulesImmediateRetransmitWithSameSeq);
    RUN_TEST(test_AckClearsRetransmissionSoLaterTimeoutDoesNotResend);
    RUN_TEST(test_ConnectionlessSendProgressionSeam);
    RUN_TEST(test_NewConnectWhileConnectedResynchronisesSequenceNumbers);
    RUN_TEST(test_RepeatedConnectWhileConnectedKeepsSequenceNumbers);
    RUN_TEST(test_IndividualSendStaysConnectionlessWhileConnected);
    RUN_TEST(test_SendSequenceAdvancesOnTransmitWithoutAck);
    RUN_TEST(test_AckDoesNotAdvanceSendSequenceAgain);
    RUN_TEST(test_InstalledClockRetransmitsFromBackgroundWork);
    RUN_TEST(test_NumberedDataWithoutConnectionIsDroppedNotRefused);
    RUN_TEST(test_ConnectionWorksAfterDroppedNumberedData);
    return UNITY_END();
}
