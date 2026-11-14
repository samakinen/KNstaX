// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "../common/virtual_bitbang_tp1_driver.hpp"
#include "../common/virtual_tp1_test_scheduler.hpp"
#include "../mocks/mock_tp1_datalink.hpp"

#include "knx/datalink/frame_codec.hpp"
#include "knx/physical/bitbang_medium_backend_adapter.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/virtual_tp1_bus_peer.hpp"
#include "knx/physical/virtual_tp1_test_runtime.hpp"

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
#include <utility>
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

    auto openResult = server.open(network, TunnelingServerEndpoint::Options{.tunnelAddresses = {knx::IndividualAddress(0x110A), knx::IndividualAddress(0x110B)}});
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
    bridge.poll();

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));

    const auto outgoingTunnel = control_packet::Codec::decodeTunnelingRequest(network.udp->tx()[0].payload);
    TEST_ASSERT_TRUE(outgoingTunnel.isOk());

    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    auto cemiResult = encodeCemiLData(tp1Frame, 0x11, cemi); // L_Data.req
    TEST_ASSERT_TRUE(cemiResult.isOk());

    const auto inboundReq = buildTunnelingRequest(channelId, 0, std::span<const uint8_t>(cemi.data(), cemiResult.value()));
    network.udp->pushRx(clientAddress, kClientPort, inboundReq);

    auto pollTunnel = server.poll(0);
    TEST_ASSERT_TRUE(pollTunnel.isOk());
    TEST_ASSERT_TRUE(pollTunnel.value());
    bridge.poll();

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(tp1.getSentFrameCount()));

    const auto stats = bridge.statistics();
    TEST_ASSERT_TRUE(stats.tp1ToIpForwarded >= 1u);
    TEST_ASSERT_TRUE(stats.ipToTp1Forwarded >= 1u);
}

// ── The bridge on the virtual TP1 bus ─────────────────────────────────────
// The production line underneath: Tp1DataLinkLayer, Tp1MacPhysical and the
// bit-bang ISR core, in bit cells on the virtual bus.

namespace {

constexpr IndividualAddress kInterfaceAddress(0x1100); // 1.1.0
constexpr IndividualAddress kTunnel1(0x110A);          // 1.1.10
constexpr IndividualAddress kTunnel2(0x110B);          // 1.1.11
constexpr IndividualAddress kBusDevice(0x1114);        // 1.1.20

struct VirtualInterface {
    physical::VirtualTp1TestRuntime runtime;
    physical::VirtualTp1BusPeer busPeer{runtime.bus()};
    testsupport::VirtualBitBangTp1Driver driver{runtime.bus()};
    std::unique_ptr<physical::Tp1MacPhysical> mac;
    std::unique_ptr<datalink::Tp1DataLinkLayer> line;
    std::unique_ptr<testsupport::VirtualTp1TestScheduler> scheduler;
    TestNetwork network;
    TunnelingServerEndpoint server;
    std::unique_ptr<network::Tp1IpInterfaceBridge> bridge;

    VirtualInterface()
    {
        mac = std::make_unique<physical::Tp1MacPhysical>(
            std::make_unique<physical::BitBangMediumBackendAdapter>(driver, driver));
        mac->setTimingPlatform(&runtime.platform());
        auto config = datalink::Tp1DataLinkConfig::defaults();
        config.enableRxTask = false;
        line = std::make_unique<datalink::Tp1DataLinkLayer>(runtime.platform(), *mac, config);
        TEST_ASSERT_TRUE(line->init(kInterfaceAddress).isOk());
        mac->setReceiveCallback(nullptr, nullptr);
        scheduler = std::make_unique<testsupport::VirtualTp1TestScheduler>(runtime, busPeer, driver);

        TEST_ASSERT_TRUE(server.open(network, TunnelingServerEndpoint::Options{
                                                  .tunnelAddresses = {kTunnel1, kTunnel2}}).isOk());
        bridge = std::make_unique<network::Tp1IpInterfaceBridge>(*line, server);
        TEST_ASSERT_TRUE(bridge->init().isOk());
        runtime.advanceTimeMs(10u);
        driver.clearSentFrames();
    }

    ~VirtualInterface()
    {
        bridge->close();
        server.close();
        line->close();
        mac->close();
        driver.close();
    }

    uint8_t connect(uint16_t clientPort)
    {
        network.udp->clearTx();
        network.udp->pushRx(IpAddress::fromOctets(127, 0, 0, 1), clientPort, buildConnectRequest(clientPort));
        TEST_ASSERT_TRUE(server.poll(0).isOk());
        auto response = control_packet::Codec::decodeChannelStatusResponse(
            network.udp->tx().back().payload, control_packet::kServiceConnectionResponse);
        TEST_ASSERT_TRUE(response.isOk());
        TEST_ASSERT_EQUAL_UINT8(0x00u, response.value().status);
        network.udp->clearTx();
        return response.value().channelId;
    }

    void request(uint8_t channelId, uint16_t clientPort, uint8_t sequence, const datalink::LDataFrame& frame)
    {
        std::array<uint8_t, kMaxCemiLDataSize> cemi{};
        auto cemiResult = encodeCemiLData(frame, 0x11, cemi);
        TEST_ASSERT_TRUE(cemiResult.isOk());
        network.udp->pushRx(IpAddress::fromOctets(127, 0, 0, 1), clientPort,
                            buildTunnelingRequest(channelId, sequence,
                                                  std::span<const uint8_t>(cemi.data(), cemiResult.value())));
        TEST_ASSERT_TRUE(server.poll(0).isOk());
        bridge->poll();
    }

    /// The cEMI frames the server sent, with the client port they went to.
    std::vector<std::pair<uint16_t, std::pair<uint8_t, datalink::LDataFrame>>> sentCemis() const
    {
        std::vector<std::pair<uint16_t, std::pair<uint8_t, datalink::LDataFrame>>> out;
        for (const auto& tx : network.udp->tx()) {
            auto decoded = control_packet::Codec::decodeTunnelingRequest(tx.payload);
            if (decoded.isError()) {
                continue;
            }
            datalink::LDataFrame frame;
            uint8_t code = 0;
            TEST_ASSERT_TRUE(decodeCemiLData(decoded.value().cemi, frame, code).isOk());
            out.push_back({tx.destinationPort, {code, frame}});
        }
        return out;
    }

    template <typename Done>
    bool run(Done done, uint32_t ms)
    {
        for (uint32_t i = 0; i < ms; ++i) {
            bridge->poll();
            if (done()) {
                return true;
            }
            runtime.advanceTimeMs(1u);
        }
        bridge->poll();
        return done();
    }

    datalink::LDataFrame sentOnBus(size_t index) const
    {
        const auto raw = driver.sentFrame(index);
        datalink::LDataFrame frame;
        TEST_ASSERT_TRUE(datalink::FrameCodec::decodeFrame(raw, frame).isOk());
        return frame;
    }
};

datalink::LDataFrame makeRequest(uint16_t destination, bool group)
{
    datalink::LDataFrame frame = makeTp1Frame();
    frame.source = IndividualAddress(0);
    if (group) {
        frame.setDestination(GroupAddress(destination));
    } else {
        frame.setDestination(IndividualAddress(destination));
    }
    return frame;
}

} // namespace

// A request with Source Address 0000h goes onto the bus from the tunnel
// address; the bus device's L2 ACK gives a positive L_Data.con.
void test_tp1_ip_interface_virtual_bus_positive_confirmation(void)
{
    VirtualInterface vi;
    constexpr uint16_t kPort = 50101;
    const uint8_t channel = vi.connect(kPort);

    vi.request(channel, kPort, 0, makeRequest(kBusDevice.raw, false));
    TEST_ASSERT_TRUE(vi.run([&] { return vi.driver.sentFrameCount() >= 1u; }, 200u));
    const auto onBus = vi.sentOnBus(0);
    TEST_ASSERT_EQUAL_HEX16(kTunnel1.raw, onBus.source.raw);
    TEST_ASSERT_EQUAL_HEX16(kBusDevice.raw, onBus.destination.raw);
    // Nothing is confirmed before the line has an outcome.
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(vi.sentCemis().size()));

    TEST_ASSERT_TRUE(vi.scheduler->driveTxOutcomeForSentFrame(0u, testsupport::VirtualTp1PeerResponse::Ack));
    TEST_ASSERT_TRUE(vi.run([&] { return !vi.sentCemis().empty(); }, 200u));

    const auto sent = vi.sentCemis();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(sent.size()));
    TEST_ASSERT_EQUAL_UINT16(kPort, sent[0].first);
    TEST_ASSERT_EQUAL_HEX8(0x2Eu, sent[0].second.first);
    TEST_ASSERT_FALSE(sent[0].second.second.confirmation);
    TEST_ASSERT_EQUAL_HEX16(kTunnel1.raw, sent[0].second.second.source.raw);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(vi.driver.sentFrameCount()));
}

// Nobody acknowledges: the data link repeats the frame three times
// (03_02_02), then the client gets a negative L_Data.con.
void test_tp1_ip_interface_virtual_bus_negative_confirmation_without_ack(void)
{
    VirtualInterface vi;
    constexpr uint16_t kPort = 50102;
    const uint8_t channel = vi.connect(kPort);

    vi.request(channel, kPort, 0, makeRequest(kBusDevice.raw, false));
    TEST_ASSERT_TRUE(vi.run([&] { return !vi.sentCemis().empty(); }, 2000u));

    const auto sent = vi.sentCemis();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(sent.size()));
    TEST_ASSERT_EQUAL_HEX8(0x2Eu, sent[0].second.first);
    TEST_ASSERT_TRUE(sent[0].second.second.confirmation);
    TEST_ASSERT_EQUAL_HEX16(kBusDevice.raw, sent[0].second.second.destination.raw);
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(vi.driver.sentFrameCount()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(vi.bridge->statistics().negativeConfirmations));
}

// The ISR acknowledges a frame for a tunnel address, and the bridge hands it
// only to the connection that has that address. A frame for an address the
// interface does not assume is neither acknowledged nor forwarded.
void test_tp1_ip_interface_virtual_bus_acknowledges_and_routes_tunnel_addresses(void)
{
    VirtualInterface vi;
    constexpr uint16_t kPort1 = 50103;
    constexpr uint16_t kPort2 = 50104;
    (void)vi.connect(kPort1);
    (void)vi.connect(kPort2);

    auto toTunnel2 = makeTp1Frame();
    toTunnel2.source = kBusDevice;
    toTunnel2.setDestination(kTunnel2);
    vi.runtime.bus().clearCapturedTxTransitions();
    TEST_ASSERT_TRUE(vi.scheduler->injectLDataFrame(toTunnel2));
    vi.runtime.advanceTimeMs(5u);
    TEST_ASSERT_FALSE(vi.runtime.bus().capturedTxTransitions().empty()); // the L2 ACK
    (void)vi.line->processRxAvailable(0u);
    vi.bridge->poll();

    auto sent = vi.sentCemis();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(sent.size()));
    TEST_ASSERT_EQUAL_UINT16(kPort2, sent[0].first);
    TEST_ASSERT_EQUAL_HEX8(0x29u, sent[0].second.first);
    TEST_ASSERT_EQUAL_HEX16(kTunnel2.raw, sent[0].second.second.destination.raw);

    vi.network.udp->clearTx();
    vi.runtime.advanceTimeMs(10u);
    auto toOther = makeTp1Frame();
    toOther.source = kBusDevice;
    toOther.setDestination(IndividualAddress(0x1150));
    vi.runtime.bus().clearCapturedTxTransitions();
    TEST_ASSERT_TRUE(vi.scheduler->injectLDataFrame(toOther));
    vi.runtime.advanceTimeMs(5u);
    TEST_ASSERT_TRUE(vi.runtime.bus().capturedTxTransitions().empty());
    (void)vi.line->processRxAvailable(0u);
    vi.bridge->poll();
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(vi.sentCemis().size()));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tp1_ip_interface_smoke_bidirectional_forwarding);
    RUN_TEST(test_tp1_ip_interface_virtual_bus_positive_confirmation);
    RUN_TEST(test_tp1_ip_interface_virtual_bus_negative_confirmation_without_ack);
    RUN_TEST(test_tp1_ip_interface_virtual_bus_acknowledges_and_routes_tunnel_addresses);
    return UNITY_END();
}
