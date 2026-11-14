// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/tunneling_server_endpoint.hpp"
#include "knx/platform/platform.hpp"

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

class ManualTimingPlatform final : public platform::TimingPlatform {
public:
    uint32_t millis() const override { return nowMs_; }
    uint64_t micros() const override { return static_cast<uint64_t>(nowMs_) * 1000ULL; }

    void delay(uint32_t ms) override { nowMs_ += ms; }
    void delayMicroseconds(uint32_t us) override { nowMs_ += (us + 999u) / 1000u; }

    void advanceMs(uint32_t ms) { nowMs_ += ms; }

private:
    uint32_t nowMs_{0};
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

std::vector<uint8_t> buildConnectionStateRequest(uint8_t channelId, uint16_t clientPort)
{
    std::array<uint8_t, 64> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    auto result = control_packet::Codec::encodeConnectionStateRequest(writer, channelId, loopbackHpai(clientPort));
    TEST_ASSERT_TRUE(result.isOk());
    return std::vector<uint8_t>(writer.span().begin(), writer.span().end());
}

std::vector<uint8_t> buildDisconnectRequest(uint8_t channelId, uint16_t clientPort)
{
    std::array<uint8_t, 64> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    auto result = control_packet::Codec::encodeDisconnectRequest(writer, channelId, loopbackHpai(clientPort));
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

} // namespace

void test_tunneling_server_endpoint_connect_state_disconnect_lifecycle(void)
{
    TestNetwork network;
    TunnelingServerEndpoint endpoint;

    TunnelingServerEndpoint::Options options{};
    options.port = NetIpPort(3671);
    options.maxChannels = 2;
    options.tunnelAddresses = {IndividualAddress(0x110A), IndividualAddress(0x110B)};

    auto openResult = endpoint.open(network, options);
    TEST_ASSERT_TRUE(openResult.isOk());
    TEST_ASSERT_NOT_NULL(network.udp);

    const IpAddress clientAddress = IpAddress::fromOctets(127, 0, 0, 1);
    constexpr uint16_t kClientPort = 45000;

    const auto connectRequest = buildConnectRequest(kClientPort);
    network.udp->pushRx(clientAddress, kClientPort, connectRequest);

    auto pollConnect = endpoint.poll(0);
    TEST_ASSERT_TRUE(pollConnect.isOk());
    TEST_ASSERT_TRUE(pollConnect.value());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(endpoint.activeChannelCount()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));

    const auto connectResponse = control_packet::Codec::decodeChannelStatusResponse(
        network.udp->tx()[0].payload,
        control_packet::kServiceConnectionResponse);
    TEST_ASSERT_TRUE(connectResponse.isOk());
    TEST_ASSERT_EQUAL_UINT8(1u, connectResponse.value().channelId);
    TEST_ASSERT_EQUAL_UINT8(0x00u, connectResponse.value().status);

    const auto stateRequest = buildConnectionStateRequest(connectResponse.value().channelId, kClientPort);
    network.udp->pushRx(clientAddress, kClientPort, stateRequest);

    auto pollState = endpoint.poll(0);
    TEST_ASSERT_TRUE(pollState.isOk());
    TEST_ASSERT_TRUE(pollState.value());
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(network.udp->tx().size()));

    const auto stateResponse = control_packet::Codec::decodeChannelStatusResponse(
        network.udp->tx()[1].payload,
        control_packet::kServiceConnectionStateResponse);
    TEST_ASSERT_TRUE(stateResponse.isOk());
    TEST_ASSERT_EQUAL_UINT8(connectResponse.value().channelId, stateResponse.value().channelId);
    TEST_ASSERT_EQUAL_UINT8(0x00u, stateResponse.value().status);

    const auto disconnectRequest = buildDisconnectRequest(connectResponse.value().channelId, kClientPort);
    network.udp->pushRx(clientAddress, kClientPort, disconnectRequest);

    auto pollDisconnect = endpoint.poll(0);
    TEST_ASSERT_TRUE(pollDisconnect.isOk());
    TEST_ASSERT_TRUE(pollDisconnect.value());
    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(network.udp->tx().size()));

    const auto disconnectResponse = control_packet::Codec::decodeChannelStatusResponse(
        network.udp->tx()[2].payload,
        control_packet::kServiceDisconnectResponse);
    TEST_ASSERT_TRUE(disconnectResponse.isOk());
    TEST_ASSERT_EQUAL_UINT8(connectResponse.value().channelId, disconnectResponse.value().channelId);
    TEST_ASSERT_EQUAL_UINT8(0x00u, disconnectResponse.value().status);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(endpoint.activeChannelCount()));
}

void test_tunneling_server_endpoint_tunneling_ack_and_callback_sequence_rules(void)
{
    TestNetwork network;
    TunnelingServerEndpoint endpoint;

    TunnelingServerEndpoint::Options options{};
    options.port = NetIpPort(3671);
    options.maxChannels = 1;
    options.tunnelAddresses = {IndividualAddress(0x110A), IndividualAddress(0x110B)};

    auto openResult = endpoint.open(network, options);
    TEST_ASSERT_TRUE(openResult.isOk());

    const IpAddress clientAddress = IpAddress::fromOctets(127, 0, 0, 1);
    constexpr uint16_t kClientPort = 46000;

    const auto connectRequest = buildConnectRequest(kClientPort);
    network.udp->pushRx(clientAddress, kClientPort, connectRequest);
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));
    const auto connectResponse = control_packet::Codec::decodeChannelStatusResponse(
        network.udp->tx()[0].payload,
        control_packet::kServiceConnectionResponse);
    TEST_ASSERT_TRUE(connectResponse.isOk());
    const uint8_t channelId = connectResponse.value().channelId;

    std::vector<uint8_t> deliveredCemi;
    uint8_t deliveredChannel = 0;
    endpoint.setReceiveCallback([&](ChannelId channel, std::span<const uint8_t> cemi) {
        deliveredChannel = channel.value();
        deliveredCemi.assign(cemi.begin(), cemi.end());
    });

    network.udp->clearTx();

    const std::array<uint8_t, 4> cemi{0x29, 0x00, 0xBC, 0x01};
    const auto validRequest = buildTunnelingRequest(channelId, 0, cemi);
    network.udp->pushRx(clientAddress, kClientPort, validRequest);

    auto pollValid = endpoint.poll(0);
    TEST_ASSERT_TRUE(pollValid.isOk());
    TEST_ASSERT_TRUE(pollValid.value());

    TEST_ASSERT_EQUAL_UINT8(channelId, deliveredChannel);
    TEST_ASSERT_EQUAL_UINT32(cemi.size(), static_cast<uint32_t>(deliveredCemi.size()));
    TEST_ASSERT_EQUAL_HEX8(0x29, deliveredCemi[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, deliveredCemi[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBC, deliveredCemi[2]);
    TEST_ASSERT_EQUAL_HEX8(0x01, deliveredCemi[3]);

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));
    auto validAck = control_packet::Codec::decodeTunnelingAck(network.udp->tx()[0].payload);
    TEST_ASSERT_TRUE(validAck.isOk());
    TEST_ASSERT_EQUAL_UINT8(channelId, validAck.value().channelId);
    TEST_ASSERT_EQUAL_UINT8(0u, validAck.value().sequence);
    TEST_ASSERT_EQUAL_UINT8(0x00u, validAck.value().status);

    network.udp->clearTx();
    deliveredCemi.clear();

    const auto wrongSequence = buildTunnelingRequest(channelId, 9, cemi);
    network.udp->pushRx(clientAddress, kClientPort, wrongSequence);

    auto pollWrongSequence = endpoint.poll(0);
    TEST_ASSERT_TRUE(pollWrongSequence.isOk());
    TEST_ASSERT_TRUE(pollWrongSequence.value());

    // 03/08/04 §2.6.1: neither the expected counter nor one less -> "shall
    // not reply and shall discard the received frame".
    TEST_ASSERT_TRUE(deliveredCemi.empty());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(network.udp->tx().size()));

    // One less than expected (a repetition of 0): acknowledged, not delivered.
    const auto repeated = buildTunnelingRequest(channelId, 0, cemi);
    network.udp->pushRx(clientAddress, kClientPort, repeated);
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());
    TEST_ASSERT_TRUE(deliveredCemi.empty());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));
    auto repeatAck = control_packet::Codec::decodeTunnelingAck(network.udp->tx()[0].payload);
    TEST_ASSERT_TRUE(repeatAck.isOk());
    TEST_ASSERT_EQUAL_UINT8(0u, repeatAck.value().sequence);
    TEST_ASSERT_EQUAL_UINT8(0x00u, repeatAck.value().status);

    // The next expected one (1) is delivered again.
    network.udp->clearTx();
    network.udp->pushRx(clientAddress, kClientPort, buildTunnelingRequest(channelId, 1, cemi));
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());
    TEST_ASSERT_EQUAL_UINT32(cemi.size(), static_cast<uint32_t>(deliveredCemi.size()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));
}

void test_tunneling_server_endpoint_connect_response_carries_hpai_and_crd(void)
{
    // 03/08/02 §7.8.2, Figure 49: channel, status, data endpoint HPAI, CRD
    // with the tunnel's Individual Address — sent to the control endpoint
    // named in the request.
    TestNetwork network;
    TunnelingServerEndpoint endpoint;
    TunnelingServerEndpoint::Options options{};
    options.port = NetIpPort(3671);
    options.tunnelAddresses = {IndividualAddress(0x110A)};
    options.dataEndpointAddress = IpAddress::fromOctets(192, 168, 200, 20);
    TEST_ASSERT_TRUE(endpoint.open(network, options).isOk());

    const IpAddress clientAddress = IpAddress::fromOctets(192, 168, 200, 7);
    std::array<uint8_t, 64> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    const control_packet::HpaiEndpoint control{control_packet::HpaiProtocol::Udp, clientAddress, 50000};
    const control_packet::HpaiEndpoint data{control_packet::HpaiProtocol::Udp, clientAddress, 50001};
    TEST_ASSERT_TRUE(control_packet::Codec::encodeConnectionRequest(writer, control, data).isOk());
    network.udp->pushRx(clientAddress, 40000, writer.span());
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));
    const auto& tx = network.udp->tx()[0];
    TEST_ASSERT_TRUE(tx.destinationAddress == clientAddress);
    TEST_ASSERT_EQUAL_UINT16(50000, tx.destinationPort);  // control endpoint, not the UDP source
    const std::array<uint8_t, 20> expected{0x06, 0x10, 0x02, 0x06, 0x00, 0x14, 0x01, 0x00, 0x08, 0x01,
                                           192,  168,  200,  20,   0x0E, 0x57, 0x04, 0x04, 0x11, 0x0A};
    TEST_ASSERT_EQUAL_UINT32(expected.size(), static_cast<uint32_t>(tx.payload.size()));
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), tx.payload.data(), expected.size());

    const auto decoded = control_packet::Codec::decodeConnectResponse(tx.payload);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL_HEX16(0x110A, decoded.value().tunnelAddress);

    // Tunnelling requests from the client go to its data endpoint.
    network.udp->clearTx();
    const std::array<uint8_t, 2> cemi{0x11, 0x00};
    std::array<uint8_t, 64> reqBytes{};
    PacketWriter req{std::span<uint8_t>(reqBytes)};
    TEST_ASSERT_TRUE(control_packet::Codec::encodeTunnelingRequest(req, 1, 0, cemi).isOk());
    network.udp->pushRx(clientAddress, 50001, req.span());
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));
    TEST_ASSERT_EQUAL_UINT16(50001, network.udp->tx()[0].destinationPort);
}

void test_tunneling_server_endpoint_refuses_what_it_cannot_serve(void)
{
    TestNetwork network;
    TunnelingServerEndpoint endpoint;
    TunnelingServerEndpoint::Options options{};
    options.tunnelAddresses = {IndividualAddress(0x110A)};
    TEST_ASSERT_TRUE(endpoint.open(network, options).isOk());
    const IpAddress clientAddress = IpAddress::fromOctets(127, 0, 0, 1);

    const auto statusOf = [&](std::span<const uint8_t> request) {
        network.udp->clearTx();
        network.udp->pushRx(clientAddress, 45000, request);
        TEST_ASSERT_TRUE(endpoint.poll(0).isOk());
        TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));
        auto response = control_packet::Codec::decodeConnectResponse(network.udp->tx()[0].payload);
        TEST_ASSERT_TRUE(response.isOk());
        return response.value().status;
    };

    // Layer 80h (busmonitor) is not served: E_TUNNELLING_LAYER.
    std::array<uint8_t, 64> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    TEST_ASSERT_TRUE(control_packet::Codec::encodeConnectionRequest(writer, loopbackHpai(45000), loopbackHpai(45000), 0x80).isOk());
    TEST_ASSERT_EQUAL_HEX8(0x29, statusOf(writer.span()));

    // Connection type 03h (device management): E_CONNECTION_TYPE.
    auto request = buildConnectRequest(45000);
    request[request.size() - 3] = 0x03;
    TEST_ASSERT_EQUAL_HEX8(0x22, statusOf(request));

    // One address, two clients: E_NO_MORE_CONNECTIONS for the second.
    TEST_ASSERT_EQUAL_HEX8(0x00, statusOf(buildConnectRequest(45000)));
    TEST_ASSERT_EQUAL_HEX8(0x24, statusOf(buildConnectRequest(45001)));
}

void test_tunneling_server_endpoint_repeats_once_then_disconnects(void)
{
    // 03/08/04 §2.6.1: no TUNNELLING_ACK within 1 s -> repeat once with the
    // same sequence counter, then DISCONNECT_REQUEST.
    TestNetwork network;
    TunnelingServerEndpoint endpoint;
    ManualTimingPlatform timing;
    TunnelingServerEndpoint::Options options{};
    options.tunnelAddresses = {IndividualAddress(0x110A)};
    TEST_ASSERT_TRUE(endpoint.open(network, options).isOk());
    endpoint.setTimingPlatform(&timing);

    const IpAddress clientAddress = IpAddress::fromOctets(127, 0, 0, 1);
    network.udp->pushRx(clientAddress, 45000, buildConnectRequest(45000));
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());
    network.udp->clearTx();

    const std::array<uint8_t, 2> cemi1{0x29, 0x01};
    const std::array<uint8_t, 2> cemi2{0x29, 0x02};
    TEST_ASSERT_TRUE(endpoint.sendCemi(ChannelId(1), cemi1).isOk());
    TEST_ASSERT_TRUE(endpoint.sendCemi(ChannelId(1), cemi2).isOk());  // queued behind the first
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));

    // ACK for sequence 0 releases the second frame with sequence 1.
    std::array<uint8_t, 16> ackBytes{};
    PacketWriter ack{std::span<uint8_t>(ackBytes)};
    TEST_ASSERT_TRUE(control_packet::Codec::encodeTunnelingAck(ack, 1, 0, 0x00).isOk());
    network.udp->pushRx(clientAddress, 45000, ack.span());
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(network.udp->tx().size()));
    auto second = control_packet::Codec::decodeTunnelingRequest(network.udp->tx()[1].payload);
    TEST_ASSERT_TRUE(second.isOk());
    TEST_ASSERT_EQUAL_UINT8(1u, second.value().sequence);

    // No ACK: after 1 s the same frame again, same sequence counter.
    timing.advanceMs(1000);
    (void)endpoint.poll(0);
    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(network.udp->tx().size()));
    auto repeat = control_packet::Codec::decodeTunnelingRequest(network.udp->tx()[2].payload);
    TEST_ASSERT_TRUE(repeat.isOk());
    TEST_ASSERT_EQUAL_UINT8(1u, repeat.value().sequence);

    // Still no ACK: DISCONNECT_REQUEST to the control endpoint, channel gone.
    timing.advanceMs(1000);
    (void)endpoint.poll(0);
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(network.udp->tx().size()));
    KnxNetIpHeader header{};
    TEST_ASSERT_TRUE(KnxNetIpCodec::decodeHeader(network.udp->tx()[3].payload, header).isOk());
    TEST_ASSERT_EQUAL_HEX16(0x0209, header.serviceType.value());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(endpoint.activeChannelCount()));
}

void test_tunneling_server_endpoint_heartbeat_supervision(void)
{
    // 03/08/02 §5.4: 120 s without a correctly received frame -> the server
    // sends DISCONNECT_REQUEST; frames with an unexpected sequence counter do
    // not retrigger the timeout.
    TestNetwork network;
    TunnelingServerEndpoint endpoint;
    ManualTimingPlatform timing;
    TunnelingServerEndpoint::Options options{};
    options.tunnelAddresses = {IndividualAddress(0x110A)};
    TEST_ASSERT_EQUAL_UINT32(120000u, options.channelIdleTimeoutMs);
    TEST_ASSERT_TRUE(endpoint.open(network, options).isOk());
    endpoint.setTimingPlatform(&timing);

    const IpAddress clientAddress = IpAddress::fromOctets(127, 0, 0, 1);
    network.udp->pushRx(clientAddress, 45000, buildConnectRequest(45000));
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());

    timing.advanceMs(100000);
    network.udp->pushRx(clientAddress, 45000, buildConnectionStateRequest(1, 45000));
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());  // heartbeat: restarts the 120 s

    timing.advanceMs(100000);
    const std::array<uint8_t, 2> cemi{0x11, 0x00};
    network.udp->pushRx(clientAddress, 45000, buildTunnelingRequest(1, 7, cemi));  // unexpected sequence
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(endpoint.activeChannelCount()));

    network.udp->clearTx();
    timing.advanceMs(20000);  // 120 s since the heartbeat
    (void)endpoint.poll(0);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(endpoint.activeChannelCount()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(network.udp->tx().size()));
    KnxNetIpHeader header{};
    TEST_ASSERT_TRUE(KnxNetIpCodec::decodeHeader(network.udp->tx()[0].payload, header).isOk());
    TEST_ASSERT_EQUAL_HEX16(0x0209, header.serviceType.value());
}

void test_tunneling_server_endpoint_send_cemi_to_all_fans_out_to_active_channels(void)
{
    TestNetwork network;
    TunnelingServerEndpoint endpoint;

    TunnelingServerEndpoint::Options options{};
    options.port = NetIpPort(3671);
    options.maxChannels = 2;
    options.tunnelAddresses = {IndividualAddress(0x110A), IndividualAddress(0x110B)};

    auto openResult = endpoint.open(network, options);
    TEST_ASSERT_TRUE(openResult.isOk());

    const IpAddress clientAddress = IpAddress::fromOctets(127, 0, 0, 1);
    constexpr uint16_t kClientPortA = 47001;
    constexpr uint16_t kClientPortB = 47002;

    const auto connectRequestA = buildConnectRequest(kClientPortA);
    const auto connectRequestB = buildConnectRequest(kClientPortB);
    network.udp->pushRx(clientAddress, kClientPortA, connectRequestA);
    network.udp->pushRx(clientAddress, kClientPortB, connectRequestB);

    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());
    TEST_ASSERT_TRUE(endpoint.poll(0).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(endpoint.activeChannelCount()));

    network.udp->clearTx();

    const std::array<uint8_t, 3> cemi{0x29, 0x00, 0x11};
    auto sendAllResult = endpoint.sendCemiToAll(cemi);
    TEST_ASSERT_TRUE(sendAllResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(sendAllResult.value()));
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(network.udp->tx().size()));

    auto tx0 = control_packet::Codec::decodeTunnelingRequest(network.udp->tx()[0].payload);
    auto tx1 = control_packet::Codec::decodeTunnelingRequest(network.udp->tx()[1].payload);
    TEST_ASSERT_TRUE(tx0.isOk());
    TEST_ASSERT_TRUE(tx1.isOk());

    TEST_ASSERT_EQUAL_UINT32(cemi.size(), static_cast<uint32_t>(tx0.value().cemi.size()));
    TEST_ASSERT_EQUAL_UINT32(cemi.size(), static_cast<uint32_t>(tx1.value().cemi.size()));
    TEST_ASSERT_EQUAL_HEX8(cemi[0], tx0.value().cemi[0]);
    TEST_ASSERT_EQUAL_HEX8(cemi[1], tx0.value().cemi[1]);
    TEST_ASSERT_EQUAL_HEX8(cemi[2], tx0.value().cemi[2]);
    TEST_ASSERT_EQUAL_HEX8(cemi[0], tx1.value().cemi[0]);
    TEST_ASSERT_EQUAL_HEX8(cemi[1], tx1.value().cemi[1]);
    TEST_ASSERT_EQUAL_HEX8(cemi[2], tx1.value().cemi[2]);
}

void test_tunneling_server_endpoint_prunes_idle_channels_on_poll(void)
{
    TestNetwork network;
    TunnelingServerEndpoint endpoint;
    ManualTimingPlatform timing;

    TunnelingServerEndpoint::Options options{};
    options.port = NetIpPort(3671);
    options.maxChannels = 1;
    options.tunnelAddresses = {IndividualAddress(0x110A), IndividualAddress(0x110B)};
    options.channelIdleTimeoutMs = 10;

    auto openResult = endpoint.open(network, options);
    TEST_ASSERT_TRUE(openResult.isOk());
    endpoint.setTimingPlatform(&timing);

    const IpAddress clientAddress = IpAddress::fromOctets(127, 0, 0, 1);
    constexpr uint16_t kClientPort = 48000;
    const auto connectRequest = buildConnectRequest(kClientPort);
    network.udp->pushRx(clientAddress, kClientPort, connectRequest);

    auto pollConnect = endpoint.poll(0);
    TEST_ASSERT_TRUE(pollConnect.isOk());
    TEST_ASSERT_TRUE(pollConnect.value());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(endpoint.activeChannelCount()));

    timing.advanceMs(11);

    auto pollPrune = endpoint.poll(0);
    TEST_ASSERT_TRUE(pollPrune.isOk());
    TEST_ASSERT_FALSE(pollPrune.value());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(endpoint.activeChannelCount()));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tunneling_server_endpoint_connect_state_disconnect_lifecycle);
    RUN_TEST(test_tunneling_server_endpoint_tunneling_ack_and_callback_sequence_rules);
    RUN_TEST(test_tunneling_server_endpoint_send_cemi_to_all_fans_out_to_active_channels);
    RUN_TEST(test_tunneling_server_endpoint_prunes_idle_channels_on_poll);
    RUN_TEST(test_tunneling_server_endpoint_connect_response_carries_hpai_and_crd);
    RUN_TEST(test_tunneling_server_endpoint_refuses_what_it_cannot_serve);
    RUN_TEST(test_tunneling_server_endpoint_repeats_once_then_disconnects);
    RUN_TEST(test_tunneling_server_endpoint_heartbeat_supervision);
    return UNITY_END();
}
