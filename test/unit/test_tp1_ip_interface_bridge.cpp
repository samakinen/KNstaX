// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "../mocks/mock_tp1_datalink.hpp"

#include "knx/netip/cemi.hpp"
#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/tunneling_server_endpoint.hpp"
#include "knx/network/tp1_ip_interface_bridge.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::netip;

void setUp(void) {}
void tearDown(void) {}

namespace {

struct RxDatagram {
    std::vector<uint8_t> payload;
    IpAddress sourceAddress{IpAddress(0)};
    uint16_t sourcePort{0};
};

struct TxDatagram {
    std::vector<uint8_t> payload;
    IpAddress destinationAddress{IpAddress(0)};
    uint16_t destinationPort{0};
};

class TestUdpSocket final : public platform::UdpSocket {
public:
    util::Result<void> open(uint16_t port = 0) override
    {
        open_ = true;
        localPort_ = port;
        return util::Result<void>::ok();
    }

    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

    util::Result<void> joinMulticast(IpAddress, IpAddress) override { return util::Result<void>::ok(); }
    void leaveMulticast(IpAddress, IpAddress) override {}
    util::Result<void> setMulticastInterface(IpAddress) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastLoopback(platform::MulticastLoopbackMode) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastTtl(uint8_t) override { return util::Result<void>::ok(); }

    int send(IpAddress destAddr, uint16_t destPort, std::span<const uint8_t> data) override
    {
        if (!open_) {
            return -1;
        }

        TxDatagram tx;
        tx.destinationAddress = destAddr;
        tx.destinationPort = destPort;
        tx.payload.assign(data.begin(), data.end());
        tx_.push_back(std::move(tx));
        return static_cast<int>(data.size());
    }

    int receive(std::span<uint8_t> buffer) override
    {
        IpAddress source(0);
        uint16_t port = 0;
        return receive(buffer, source, port);
    }

    int receive(std::span<uint8_t> buffer, IpAddress& srcAddr, uint16_t& srcPort) override
    {
        if (!open_ || rx_.empty()) {
            return -1;
        }

        const auto& next = rx_.front();
        const size_t bytes = next.payload.size() > buffer.size() ? buffer.size() : next.payload.size();
        std::memcpy(buffer.data(), next.payload.data(), bytes);
        srcAddr = next.sourceAddress;
        srcPort = next.sourcePort;
        rx_.pop_front();
        return static_cast<int>(bytes);
    }

    size_t available() const override
    {
        return rx_.empty() ? 0u : rx_.front().payload.size();
    }

    uint16_t localPort() const override { return localPort_; }

    void pushRx(IpAddress sourceAddress, uint16_t sourcePort, std::span<const uint8_t> payload)
    {
        RxDatagram rx;
        rx.sourceAddress = sourceAddress;
        rx.sourcePort = sourcePort;
        rx.payload.assign(payload.begin(), payload.end());
        rx_.push_back(std::move(rx));
    }

    void clearTx() { tx_.clear(); }
    const std::vector<TxDatagram>& tx() const { return tx_; }

private:
    bool open_{false};
    uint16_t localPort_{0};
    std::deque<RxDatagram> rx_;
    std::vector<TxDatagram> tx_;
};

class TestNetwork final : public platform::NetworkInterface {
public:
    util::Result<void> init() override { return util::Result<void>::ok(); }
    bool isConnected() const override { return true; }

    IpAddress ipAddress() const override { return IpAddress(0); }
    IpAddress subnetMask() const override { return IpAddress(0); }
    IpAddress gateway() const override { return IpAddress(0); }

    void macAddress(std::span<uint8_t> mac) const override
    {
        if (mac.size() < 6) {
            return;
        }
        std::memset(mac.data(), 0, 6);
    }

    std::unique_ptr<platform::UdpSocket> createUdpSocket() override
    {
        auto socket = std::make_unique<TestUdpSocket>();
        udp = socket.get();
        return socket;
    }

    TestUdpSocket* udp{nullptr};
};

control_packet::HpaiEndpoint loopbackHpai(uint16_t port)
{
    return control_packet::HpaiEndpoint{
        .protocol = control_packet::HpaiProtocol::Udp,
        .address = IpAddress::fromOctets(127, 0, 0, 1),
        .port = port,
    };
}

std::vector<uint8_t> buildConnectRequest(uint16_t clientPort)
{
    std::array<uint8_t, 64> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    auto result = control_packet::Codec::encodeConnectionRequest(writer, loopbackHpai(clientPort), loopbackHpai(clientPort));
    TEST_ASSERT_TRUE(result.isOk());
    return std::vector<uint8_t>(writer.span().begin(), writer.span().end());
}

std::vector<uint8_t> buildTunnelingRequest(uint8_t channelId, uint8_t sequence, std::span<const uint8_t> cemi)
{
    std::array<uint8_t, 128> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    auto result = control_packet::Codec::encodeTunnelingRequest(writer, channelId, sequence, cemi);
    TEST_ASSERT_TRUE(result.isOk());
    return std::vector<uint8_t>(writer.span().begin(), writer.span().end());
}

datalink::LDataFrame makeTp1Frame()
{
    datalink::LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = true;
    frame.confirmation = true;
    frame.source = IndividualAddress(0x1101);
    frame.setDestination(GroupAddress(0x2201));
    frame.hopCount = 6;
    frame.setTpdu(protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
    return frame;
}

uint8_t connectSingleChannel(TunnelingServerEndpoint& endpoint, TestUdpSocket& udp, uint16_t clientPort)
{
    const IpAddress clientAddress = IpAddress::fromOctets(127, 0, 0, 1);
    const auto connectRequest = buildConnectRequest(clientPort);
    udp.pushRx(clientAddress, clientPort, connectRequest);

    auto poll = endpoint.poll(0);
    TEST_ASSERT_TRUE(poll.isOk());
    TEST_ASSERT_TRUE(poll.value());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(udp.tx().size()));

    auto connectResponse = control_packet::Codec::decodeChannelStatusResponse(
        udp.tx()[0].payload,
        control_packet::kServiceConnectionResponse);
    TEST_ASSERT_TRUE(connectResponse.isOk());
    TEST_ASSERT_EQUAL_UINT8(0x00u, connectResponse.value().status);

    return connectResponse.value().channelId;
}

std::vector<uint8_t> buildTunnelingAck(uint8_t channelId, uint8_t sequence)
{
    std::array<uint8_t, 32> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    auto result = control_packet::Codec::encodeTunnelingAck(writer, channelId, sequence, control_packet::kStatusNoError);
    TEST_ASSERT_TRUE(result.isOk());
    return std::vector<uint8_t>(writer.span().begin(), writer.span().end());
}

constexpr IndividualAddress kTunnelA(0x110A);
constexpr IndividualAddress kTunnelB(0x110B);
constexpr uint16_t kPortA = 49011;
constexpr uint16_t kPortB = 49012;

/// A decoded TUNNELLING_REQUEST the server sent to a client.
struct ServerCemi {
    uint16_t port{0};
    uint8_t channelId{0};
    uint8_t sequence{0};
    uint8_t messageCode{0};
    datalink::LDataFrame frame{};
};

std::vector<ServerCemi> serverCemis(const TestUdpSocket& udp)
{
    std::vector<ServerCemi> out;
    for (const auto& tx : udp.tx()) {
        auto request = control_packet::Codec::decodeTunnelingRequest(tx.payload);
        if (request.isError()) {
            continue;
        }
        ServerCemi item;
        item.port = tx.destinationPort;
        item.channelId = request.value().channelId;
        item.sequence = request.value().sequence;
        TEST_ASSERT_TRUE(decodeCemiLData(request.value().cemi, item.frame, item.messageCode).isOk());
        out.push_back(item);
    }
    return out;
}

struct Fixture {
    mocks::MockTp1DataLinkLayer tp1;
    TestNetwork network;
    TunnelingServerEndpoint server;
    std::unique_ptr<network::Tp1IpInterfaceBridge> bridge;
    uint8_t channelA{0};
    uint8_t channelB{0};

    explicit Fixture(bool twoClients)
    {
        TEST_ASSERT_TRUE(server.open(network, TunnelingServerEndpoint::Options{
                                                  .tunnelAddresses = {kTunnelA, kTunnelB}}).isOk());
        bridge = std::make_unique<network::Tp1IpInterfaceBridge>(tp1, server);
        TEST_ASSERT_TRUE(bridge->init().isOk());
        channelA = connectSingleChannel(server, *network.udp, kPortA);
        network.udp->clearTx();
        if (twoClients) {
            channelB = connectSingleChannel(server, *network.udp, kPortB);
            network.udp->clearTx();
        }
    }

    void clientSends(uint8_t channelId, uint16_t port, uint8_t sequence, const datalink::LDataFrame& frame,
                     uint8_t messageCode = 0x11)
    {
        std::array<uint8_t, kMaxCemiLDataSize> cemi{};
        auto cemiResult = encodeCemiLData(frame, messageCode, cemi);
        TEST_ASSERT_TRUE(cemiResult.isOk());
        const auto request = buildTunnelingRequest(channelId, sequence,
                                                   std::span<const uint8_t>(cemi.data(), cemiResult.value()));
        network.udp->pushRx(IpAddress::fromOctets(127, 0, 0, 1), port, request);
        auto poll = server.poll(0);
        TEST_ASSERT_TRUE(poll.isOk());
        bridge->poll();
    }

    void clientAcks(uint8_t channelId, uint16_t port, uint8_t sequence)
    {
        const auto ack = buildTunnelingAck(channelId, sequence);
        network.udp->pushRx(IpAddress::fromOctets(127, 0, 0, 1), port, ack);
        TEST_ASSERT_TRUE(server.poll(0).isOk());
        bridge->poll();
    }
};

datalink::LDataFrame makeClientRequest(IndividualAddress source, uint16_t destination, bool group)
{
    datalink::LDataFrame frame = makeTp1Frame();
    frame.source = source;
    if (group) {
        frame.setDestination(GroupAddress(destination));
    } else {
        frame.setDestination(IndividualAddress(destination));
    }
    return frame;
}

} // namespace

void test_tp1_ip_interface_bridge_forwards_tp1_to_ip_clients(void)
{
    Fixture f(false);

    const auto tp1Frame = makeTp1Frame();
    f.tp1.injectRxFrame(tp1Frame);
    // Indications are handed over on the tunnelling task.
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(f.network.udp->tx().size()));
    f.bridge->poll();

    const auto stats = f.bridge->statistics();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(stats.tp1ToIpForwarded));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(stats.tp1ToIpDropped));

    const auto sent = serverCemis(*f.network.udp);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(sent.size()));
    TEST_ASSERT_EQUAL_HEX8(0x29u, sent[0].messageCode);
    TEST_ASSERT_EQUAL_HEX16(tp1Frame.destination.raw, sent[0].frame.destination.raw);
}

void test_tp1_ip_interface_bridge_forwards_ip_cemi_to_tp1(void)
{
    Fixture f(false);

    const auto frame = makeTp1Frame();
    f.clientSends(f.channelA, kPortA, 0, frame);

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(f.tp1.getSentFrameCount()));
    const auto sentFrame = f.tp1.getLastSentFrame();
    TEST_ASSERT_EQUAL_HEX16(frame.destination.raw, sentFrame.destination.raw);
    // A source other than 0000h goes onto the line unchanged (03_08_04 2.2.2).
    TEST_ASSERT_EQUAL_HEX16(frame.source.raw, sentFrame.source.raw);

    const auto stats = f.bridge->statistics();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(stats.ipToTp1Forwarded));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(stats.ipToTp1Dropped));
}

// 03_08_04 2.2.2: an L_Data.req with Source Address 0000h is sent with the
// connection's tunnel address. 03_06_03 4.1.5.3.4: the L_Data.con carries
// the requested frame's addresses and Confirm flag 0 on success.
void test_tp1_ip_interface_bridge_fills_tunnel_address_and_confirms_positively(void)
{
    Fixture f(true);

    f.clientSends(f.channelB, kPortB, 0, makeClientRequest(IndividualAddress(0), 0x2201, true));

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(f.tp1.getSentFrameCount()));
    TEST_ASSERT_EQUAL_HEX16(kTunnelB.raw, f.tp1.getLastSentFrame().source.raw);

    const auto sent = serverCemis(*f.network.udp);
    // The L_Data.con to the sender, and the frame indicated to the other
    // connection as any device on the line would see it.
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(sent.size()));
    TEST_ASSERT_EQUAL_UINT16(kPortB, sent[0].port);
    TEST_ASSERT_EQUAL_HEX8(0x2Eu, sent[0].messageCode);
    TEST_ASSERT_FALSE(sent[0].frame.confirmation); // C = 0: no error
    TEST_ASSERT_EQUAL_HEX16(kTunnelB.raw, sent[0].frame.source.raw);
    TEST_ASSERT_EQUAL_HEX16(0x2201u, sent[0].frame.destination.raw);
    TEST_ASSERT_TRUE(sent[0].frame.destinationType == AddressType::Group);
    TEST_ASSERT_EQUAL_UINT16(kPortA, sent[1].port);
    TEST_ASSERT_EQUAL_HEX8(0x29u, sent[1].messageCode);
    TEST_ASSERT_EQUAL_HEX16(kTunnelB.raw, sent[1].frame.source.raw);

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(f.bridge->statistics().positiveConfirmations));
}

// NAK, BUSY or no acknowledge after the repetitions: Confirm flag 1.
void test_tp1_ip_interface_bridge_confirms_negatively_when_the_line_fails(void)
{
    using State = datalink::Tp1DataLinkLayer::TxProgressState;
    const State failures[] = {State::Timeout, State::TransmissionFailed, State::Busy};
    for (State failure : failures) {
        Fixture f(true);
        f.tp1.setTxOutcome(failure);

        f.clientSends(f.channelA, kPortA, 0, makeClientRequest(IndividualAddress(0), 0x1105, false));

        const auto sent = serverCemis(*f.network.udp);
        // Only the confirmation: a frame that did not make it is not
        // indicated to anyone.
        TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(sent.size()));
        TEST_ASSERT_EQUAL_UINT16(kPortA, sent[0].port);
        TEST_ASSERT_EQUAL_HEX8(0x2Eu, sent[0].messageCode);
        TEST_ASSERT_TRUE(sent[0].frame.confirmation); // C = 1: error
        TEST_ASSERT_EQUAL_HEX16(kTunnelA.raw, sent[0].frame.source.raw);
        TEST_ASSERT_EQUAL_HEX16(0x1105u, sent[0].frame.destination.raw);
        TEST_ASSERT_TRUE(sent[0].frame.destinationType == AddressType::Individual);
        const auto stats = f.bridge->statistics();
        TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(stats.negativeConfirmations));
        TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(stats.ipToTp1Dropped));
    }
}

// Point-to-point frames from TP1 go only to the connection whose tunnel
// address they are for; group frames to all (03_08_04 2.2.2).
void test_tp1_ip_interface_bridge_routes_indications_by_tunnel_address(void)
{
    Fixture f(true);

    auto toB = makeTp1Frame();
    toB.setDestination(kTunnelB);
    f.tp1.injectRxFrame(toB);
    auto toSomeoneElse = makeTp1Frame();
    toSomeoneElse.setDestination(IndividualAddress(0x1150));
    f.tp1.injectRxFrame(toSomeoneElse);
    f.bridge->poll();

    auto sent = serverCemis(*f.network.udp);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(sent.size()));
    TEST_ASSERT_EQUAL_UINT16(kPortB, sent[0].port);
    TEST_ASSERT_EQUAL_HEX16(kTunnelB.raw, sent[0].frame.destination.raw);

    f.clientAcks(f.channelB, kPortB, sent[0].sequence);
    f.network.udp->clearTx();
    f.tp1.injectRxFrame(makeTp1Frame());
    f.bridge->poll();
    sent = serverCemis(*f.network.udp);
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(sent.size()));
    TEST_ASSERT_TRUE(sent[0].port != sent[1].port);
}

// Requests are confirmed one at a time in arrival order.
void test_tp1_ip_interface_bridge_confirms_requests_in_order(void)
{
    Fixture f(false);

    f.clientSends(f.channelA, kPortA, 0, makeClientRequest(IndividualAddress(0), 0x0901, true));
    auto first = serverCemis(*f.network.udp);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(first.size()));
    f.clientAcks(f.channelA, kPortA, first[0].sequence);
    f.network.udp->clearTx();

    f.clientSends(f.channelA, kPortA, 1, makeClientRequest(IndividualAddress(0), 0x0902, true));
    auto second = serverCemis(*f.network.udp);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(second.size()));
    TEST_ASSERT_EQUAL_HEX8(0x2Eu, second[0].messageCode);
    TEST_ASSERT_EQUAL_HEX16(0x0902u, second[0].frame.destination.raw);
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(f.tp1.getSentFrameCount()));
}

void test_tp1_ip_interface_bridge_drops_invalid_ip_cemi(void)
{
    Fixture f(false);

    const std::array<uint8_t, 3> invalidCemi{0x01, 0x02, 0x03};
    const auto request = buildTunnelingRequest(f.channelA, 0, invalidCemi);
    f.network.udp->pushRx(IpAddress::fromOctets(127, 0, 0, 1), kPortA, request);
    auto poll = f.server.poll(0);
    TEST_ASSERT_TRUE(poll.isOk());
    TEST_ASSERT_TRUE(poll.value());
    f.bridge->poll();

    // An L_Data.ind from the client is not a request either.
    f.clientSends(f.channelA, kPortA, 1, makeTp1Frame(), 0x29);

    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(f.tp1.getSentFrameCount()));

    const auto stats = f.bridge->statistics();
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(stats.ipToTp1Forwarded));
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(stats.ipToTp1Dropped));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tp1_ip_interface_bridge_forwards_tp1_to_ip_clients);
    RUN_TEST(test_tp1_ip_interface_bridge_forwards_ip_cemi_to_tp1);
    RUN_TEST(test_tp1_ip_interface_bridge_fills_tunnel_address_and_confirms_positively);
    RUN_TEST(test_tp1_ip_interface_bridge_confirms_negatively_when_the_line_fails);
    RUN_TEST(test_tp1_ip_interface_bridge_routes_indications_by_tunnel_address);
    RUN_TEST(test_tp1_ip_interface_bridge_confirms_requests_in_order);
    RUN_TEST(test_tp1_ip_interface_bridge_drops_invalid_ip_cemi);
    return UNITY_END();
}
