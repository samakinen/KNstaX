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

} // namespace

void test_tp1_ip_interface_smoke_bidirectional_forwarding(void)
{
    mocks::MockTp1DataLinkLayer tp1;
    TestNetwork network;
    TunnelingServerEndpoint server;

    auto openResult = server.open(network, TunnelingServerEndpoint::Options{});
    TEST_ASSERT_TRUE(openResult.isOk());

    network::Tp1IpInterfaceBridge bridge(tp1, server);
    TEST_ASSERT_TRUE(bridge.init().isOk());

    const IpAddress clientAddress = IpAddress::fromOctets(127, 0, 0, 1);
    constexpr uint16_t kClientPort = 50001;

    const auto connectRequest = buildConnectRequest(kClientPort);
    network.udp->pushRx(clientAddress, kClientPort, connectRequest);
    auto pollConnect = server.poll(0);
    TEST_ASSERT_TRUE(pollConnect.isOk());
    TEST_ASSERT_TRUE(pollConnect.value());

    auto connectResponse = control_packet::Codec::decodeChannelStatusResponse(
        network.udp->tx()[0].payload,
        control_packet::kServiceConnectionResponse);
    TEST_ASSERT_TRUE(connectResponse.isOk());
    TEST_ASSERT_EQUAL_UINT8(0x00u, connectResponse.value().status);
    const uint8_t channelId = connectResponse.value().channelId;

    network.udp->clearTx();

    const auto tp1Frame = makeTp1Frame();
    tp1.injectRxFrame(tp1Frame);

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));

    const auto outgoingTunnel = control_packet::Codec::decodeTunnelingRequest(network.udp->tx()[0].payload);
    TEST_ASSERT_TRUE(outgoingTunnel.isOk());

    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    auto cemiResult = encodeCemiLData(tp1Frame, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult.isOk());

    const auto inboundReq = buildTunnelingRequest(channelId, 0, std::span<const uint8_t>(cemi.data(), cemiResult.value()));
    network.udp->pushRx(clientAddress, kClientPort, inboundReq);

    auto pollTunnel = server.poll(0);
    TEST_ASSERT_TRUE(pollTunnel.isOk());
    TEST_ASSERT_TRUE(pollTunnel.value());

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(tp1.getSentFrameCount()));

    const auto stats = bridge.statistics();
    TEST_ASSERT_TRUE(stats.tp1ToIpForwarded >= 1u);
    TEST_ASSERT_TRUE(stats.ipToTp1Forwarded >= 1u);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tp1_ip_interface_smoke_bidirectional_forwarding);
    return UNITY_END();
}
