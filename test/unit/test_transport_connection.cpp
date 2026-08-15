// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_transport_connection.cpp
 * @brief Unit tests for transport-layer connection-oriented semantics
 */

#include "unity.h"

#include "knx/transport/transport_layer.hpp"
#include "knx/protocol/tpdu_codec.hpp"

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

    // Should send NAK indicating expected seq (0) and not deliver.
    // Control TPDUs (T_ACK/T_NAK) are exactly 1 byte on the wire.
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    const auto sent = nw->getLastSentFrame();
    TEST_ASSERT_TRUE(sent.dlFrame.tpdu.size() >= 1);
    const auto hdr = knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0],
                                                     sent.dlFrame.tpdu.size() > 1u ? sent.dlFrame.tpdu[1] : 0u);
    TEST_ASSERT_EQUAL(knx::protocol::TPCI::NumberedControl, hdr.tpci.type());
    TEST_ASSERT_EQUAL_UINT8(0, hdr.tpci.seqNum());
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

void test_ExplicitInboundReceiveProgressQueuesNumberedDataAfterPolledAck(void) {
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
        {0xBA, 0xAD}
    );

    auto beginRes = tl->beginReceive(makeNFrame(remote.raw, 0x110A, dataTpdu));
    TEST_ASSERT_TRUE(beginRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(0, tl->queuedReceiveCount());
    TEST_ASSERT_EQUAL_UINT32(0, nw->getSentFrameCount());

    auto progress = tl->pollReceive();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(TransportLayer::ControlSendProgressState::Success, progress.value());

    TEST_ASSERT_EQUAL_UINT32(0, rx.size());
    TEST_ASSERT_EQUAL_UINT32(1, nw->getSentFrameCount());
    TEST_ASSERT_EQUAL_UINT32(1, tl->queuedReceiveCount());

    const auto sent = nw->getLastSentFrame();
    // Control TPDUs (T_ACK) are exactly 1 byte on the wire.
    TEST_ASSERT_TRUE(sent.dlFrame.tpdu.size() >= 1);
    const auto sentHdr = knx::protocol::unpackTpduHeader(sent.dlFrame.tpdu[0],
                                                         sent.dlFrame.tpdu.size() > 1u ? sent.dlFrame.tpdu[1] : 0u);
    TEST_ASSERT_TRUE(sentHdr.tpci.isAck());
    TEST_ASSERT_EQUAL_UINT8(0, sentHdr.tpci.seqNum());

    TDataFrame queued;
    TEST_ASSERT_TRUE(tl->popReceivedFrame(queued));
    TEST_ASSERT_EQUAL_UINT16(remote.raw, queued.source.raw);
    TEST_ASSERT_EQUAL_UINT8(0, queued.sequenceNumber);
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(queued.payload().size()));
    TEST_ASSERT_EQUAL_UINT8(0xBA, queued.payload()[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAD, queued.payload()[1]);
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
    tl->processRetransmissions(10000);

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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ConnectResponseDoesNotEchoConnect);
    RUN_TEST(test_SingleByteInboundConnectCreatesConnectionAndSendsResponse);
    RUN_TEST(test_DisconnectResponseDoesNotEchoDisconnect);
    RUN_TEST(test_SeqMismatchSendsNakAndDoesNotDeliver);
    RUN_TEST(test_DuplicateSuppressionAcksOnly);
    RUN_TEST(test_ReceiveQueueDeliversNumberedDataWithoutCallback);
    RUN_TEST(test_ExplicitInboundReceiveProgressQueuesNumberedDataAfterPolledAck);
    RUN_TEST(test_ReceiveQueueDropsNewestWhenFull);
    RUN_TEST(test_RetransmitAfterTimeoutResendsSameSeq);
    RUN_TEST(test_MatchingNakSchedulesImmediateRetransmitWithSameSeq);
    RUN_TEST(test_AckClearsRetransmissionSoLaterTimeoutDoesNotResend);
    RUN_TEST(test_ConnectionlessSendProgressionSeam);
    RUN_TEST(test_NewConnectWhileConnectedResynchronisesSequenceNumbers);
    RUN_TEST(test_RepeatedConnectWhileConnectedKeepsSequenceNumbers);
    RUN_TEST(test_IndividualSendStaysConnectionlessWhileConnected);
    return UNITY_END();
}
