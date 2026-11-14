// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_netip_tunneling_client_rules.cpp
 * @brief The KNXnet/IP tunnelling client against a scripted gateway on a
 *        simulated clock: CONNECT_RESPONSE, sequence counters, repetition,
 *        heartbeat (03/08/02 §5.4, §7.8.2; 03/08/04 §2.6.1).
 */

#include "unity.h"

#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/netip/tunneling_session_client.hpp"
#include "knx/platform/platform.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::netip;

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr uint16_t kControlPort = 3671;
constexpr uint16_t kDataPort = 3672;
const IpAddress kGateway = IpAddress::fromOctets(10, 0, 0, 2);

struct Datagram {
    std::vector<uint8_t> payload;
    IpAddress address{IpAddress(0)};
    uint16_t port{0};
};

uint16_t serviceOf(std::span<const uint8_t> frame)
{
    return static_cast<uint16_t>((frame[2] << 8) | frame[3]);
}

/// A UDP socket whose peer is a function: every send() is offered to the
/// responder, which may queue replies.
class ScriptedSocket final : public platform::UdpSocket {
public:
    using Responder = std::function<void(const Datagram& sent, ScriptedSocket& socket)>;

    util::Result<void> open(uint16_t) override { open_ = true; return util::Result<void>::ok(); }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    util::Result<void> joinMulticast(IpAddress, IpAddress) override { return util::Result<void>::ok(); }
    void leaveMulticast(IpAddress, IpAddress) override {}
    util::Result<void> setMulticastInterface(IpAddress) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastLoopback(platform::MulticastLoopbackMode) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastTtl(uint8_t) override { return util::Result<void>::ok(); }

    int send(IpAddress destAddr, uint16_t destPort, std::span<const uint8_t> data) override
    {
        if (!open_) return -1;
        Datagram datagram{{data.begin(), data.end()}, destAddr, destPort};
        sent.push_back(datagram);
        if (log != nullptr) log->push_back(datagram);
        if (responder) responder(datagram, *this);
        return static_cast<int>(data.size());
    }

    int receive(std::span<uint8_t> buffer) override
    {
        IpAddress address(0);
        uint16_t port = 0;
        return receive(buffer, address, port);
    }

    int receive(std::span<uint8_t> buffer, IpAddress& srcAddr, uint16_t& srcPort) override
    {
        if (!open_ || inbound.empty()) return -1;
        const Datagram next = inbound.front();
        inbound.pop_front();
        const size_t bytes = std::min(next.payload.size(), buffer.size());
        std::memcpy(buffer.data(), next.payload.data(), bytes);
        srcAddr = next.address;
        srcPort = next.port;
        return static_cast<int>(bytes);
    }

    size_t available() const override { return inbound.empty() ? 0u : inbound.front().payload.size(); }
    uint16_t localPort() const override { return 50000; }

    void reply(uint16_t fromPort, std::span<const uint8_t> payload)
    {
        inbound.push_back(Datagram{{payload.begin(), payload.end()}, kGateway, fromPort});
    }

    size_t countSent(uint16_t service) const
    {
        size_t n = 0;
        for (const auto& d : sent) n += serviceOf(d.payload) == service ? 1u : 0u;
        return n;
    }

    std::vector<Datagram> sentOf(uint16_t service) const
    {
        std::vector<Datagram> out;
        for (const auto& d : sent) {
            if (serviceOf(d.payload) == service) out.push_back(d);
        }
        return out;
    }

    Responder responder;
    std::vector<Datagram> sent;
    /// Outlives the socket: the client destroys its socket on close().
    std::vector<Datagram>* log{nullptr};
    std::deque<Datagram> inbound;

private:
    bool open_{false};
};

class FakeNetwork final : public platform::NetworkInterface {
public:
    util::Result<void> init() override { return util::Result<void>::ok(); }
    bool isConnected() const override { return true; }
    IpAddress ipAddress() const override { return IpAddress::fromOctets(10, 0, 0, 7); }
    IpAddress subnetMask() const override { return IpAddress::fromOctets(255, 255, 255, 0); }
    IpAddress gateway() const override { return IpAddress(0); }
    void macAddress(std::span<uint8_t> mac) const override { std::fill(mac.begin(), mac.end(), 0); }
    std::unique_ptr<platform::UdpSocket> createUdpSocket() override
    {
        auto socket = std::make_unique<ScriptedSocket>();
        socket->responder = responder;
        socket->log = &sentLog;
        last = socket.get();
        return socket;
    }

    ScriptedSocket::Responder responder;
    ScriptedSocket* last{nullptr};
    std::vector<Datagram> sentLog;

    size_t countSent(uint16_t service) const
    {
        size_t n = 0;
        for (const auto& d : sentLog) n += serviceOf(d.payload) == service ? 1u : 0u;
        return n;
    }

    std::vector<Datagram> sentOf(uint16_t service) const
    {
        std::vector<Datagram> out;
        for (const auto& d : sentLog) {
            if (serviceOf(d.payload) == service) out.push_back(d);
        }
        return out;
    }
};

class ManualClock final : public platform::TimingPlatform {
public:
    uint32_t millis() const override { return now_; }
    uint64_t micros() const override { return static_cast<uint64_t>(now_) * 1000u; }
    void delay(uint32_t ms) override { now_ += ms == 0 ? 1 : ms; }
    void delayMicroseconds(uint32_t us) override { now_ += (us + 999u) / 1000u; }

private:
    uint32_t now_{1};
};

std::vector<uint8_t> channelStatus(uint16_t service, uint8_t channel, uint8_t status)
{
    std::array<uint8_t, 16> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    TEST_ASSERT_TRUE(control_packet::Codec::encodeChannelStatusResponse(writer, NetIpServiceType(service), channel, status).isOk());
    return {writer.span().begin(), writer.span().end()};
}

std::vector<uint8_t> tunnelingAck(uint8_t channel, uint8_t sequence, uint8_t status)
{
    std::array<uint8_t, 16> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    TEST_ASSERT_TRUE(control_packet::Codec::encodeTunnelingAck(writer, channel, sequence, status).isOk());
    return {writer.span().begin(), writer.span().end()};
}

std::vector<uint8_t> tunnelingRequest(uint8_t channel, uint8_t sequence, std::span<const uint8_t> cemi)
{
    std::array<uint8_t, 64> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    TEST_ASSERT_TRUE(control_packet::Codec::encodeTunnelingRequest(writer, channel, sequence, cemi).isOk());
    return {writer.span().begin(), writer.span().end()};
}

/// The gateway: answers CONNECT_REQUEST with channel 7, data endpoint on
/// kDataPort and IA 1.1.10; DISCONNECT_REQUEST and (if enabled)
/// CONNECTIONSTATE_REQUEST with E_NO_ERROR; TUNNELLING_REQUESTs per `ack`.
struct Gateway {
    uint8_t connectStatus{0x00};
    bool answerHeartbeat{true};
    /// Status for each TUNNELLING_REQUEST received, in order; once exhausted,
    /// no ACK at all.
    std::deque<int> acks{};

    void operator()(const Datagram& sent, ScriptedSocket& socket)
    {
        const uint16_t service = serviceOf(sent.payload);
        if (service == 0x0205) {
            std::array<uint8_t, 32> bytes{};
            PacketWriter writer{std::span<uint8_t>(bytes)};
            const control_packet::HpaiEndpoint data{control_packet::HpaiProtocol::Udp, kGateway, kDataPort};
            TEST_ASSERT_TRUE(control_packet::Codec::encodeConnectResponse(writer, connectStatus == 0 ? 7 : 0,
                                                                          connectStatus, data, 0x110A).isOk());
            socket.reply(kControlPort, writer.span());
        } else if (service == 0x0209) {
            socket.reply(kControlPort, channelStatus(0x020A, 7, 0x00));
        } else if (service == 0x0207 && answerHeartbeat) {
            socket.reply(kControlPort, channelStatus(0x0208, 7, 0x00));
        } else if (service == 0x0420) {
            if (!acks.empty()) {
                const int status = acks.front();
                acks.pop_front();
                if (status >= 0) {
                    socket.reply(kDataPort, tunnelingAck(7, sent.payload[8], static_cast<uint8_t>(status)));
                }
            }
        }
    }
};

struct Harness {
    FakeNetwork network;
    ManualClock clock;
    TunnelingSessionClient client;
    std::shared_ptr<Gateway> gateway = std::make_shared<Gateway>();
    std::vector<std::vector<uint8_t>> delivered;

    Harness()
    {
        auto gw = gateway;
        network.responder = [gw](const Datagram& d, ScriptedSocket& s) { (*gw)(d, s); };
        client.setTimingPlatform(&clock);
        client.setReceiveCallback([this](std::span<const uint8_t> cemi) {
            delivered.emplace_back(cemi.begin(), cemi.end());
        });
    }

    util::Result<void> open() { return client.open(network, kGateway, NetIpPort(kControlPort), 1000); }
    ScriptedSocket& socket() { return *network.last; }
};

} // namespace

void test_connect_response_hpai_and_crd_are_used(void)
{
    // 03/08/02 §7.8.2: TUNNELLING_REQUESTs go to the data endpoint of the
    // CONNECT_RESPONSE; the CRD names the tunnel's Individual Address.
    Harness h;
    TEST_ASSERT_TRUE(h.open().isOk());
    TEST_ASSERT_EQUAL_HEX16(0x110A, h.client.tunnelAddress());
    TEST_ASSERT_EQUAL_UINT8(7, h.client.channelId().value());

    h.gateway->acks = {0x00};
    const std::array<uint8_t, 2> cemi{0x11, 0x00};
    TEST_ASSERT_TRUE(h.client.sendCemi(cemi).isOk());
    const auto requests = h.socket().sentOf(0x0420);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(requests.size()));
    TEST_ASSERT_EQUAL_UINT16(kDataPort, requests[0].port);
    TEST_ASSERT_EQUAL_UINT8(1, h.client.sequence().value());
}

void test_connect_refused_reports_failure(void)
{
    Harness h;
    h.gateway->connectStatus = control_packet::kStatusNoMoreConnections;
    TEST_ASSERT_TRUE(h.open().isError());
    TEST_ASSERT_FALSE(h.client.isOpen());
}

void test_inbound_sequence_rules(void)
{
    // 03/08/04 §2.6.1, client side: expected -> ACK and process; one less ->
    // ACK and discard; anything else -> discard without reply.
    Harness h;
    TEST_ASSERT_TRUE(h.open().isOk());
    auto& socket = h.socket();
    const std::array<uint8_t, 2> cemi{0x29, 0x00};
    const auto feed = [&](uint8_t sequence) {
        socket.reply(kDataPort, tunnelingRequest(7, sequence, cemi));
        (void)h.client.poll(0);
    };

    feed(0);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(h.delivered.size()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(socket.countSent(0x0421)));
    TEST_ASSERT_EQUAL_UINT16(kDataPort, socket.sentOf(0x0421).back().port);

    feed(0);  // repetition
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(h.delivered.size()));
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(socket.countSent(0x0421)));

    feed(5);  // out of order
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(h.delivered.size()));
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(socket.countSent(0x0421)));

    feed(1);
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(h.delivered.size()));
    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(socket.countSent(0x0421)));
}

void test_unacknowledged_request_is_repeated_once_with_same_sequence(void)
{
    Harness h;
    TEST_ASSERT_TRUE(h.open().isOk());
    h.gateway->acks = {-1, 0x00};  // first copy lost, the repeat confirmed
    const std::array<uint8_t, 2> cemi{0x11, 0x00};
    TEST_ASSERT_TRUE(h.client.sendCemi(cemi).isOk());
    const auto requests = h.socket().sentOf(0x0420);
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(requests.size()));
    TEST_ASSERT_EQUAL_UINT8(0, requests[0].payload[8]);
    TEST_ASSERT_EQUAL_UINT8(0, requests[1].payload[8]);
    TEST_ASSERT_EQUAL_UINT8(1, h.client.sequence().value());
    TEST_ASSERT_TRUE(h.client.isOpen());
}

void test_error_ack_counts_as_unconfirmed(void)
{
    // "or the status of a received TUNNELLING_ACK frame signals any kind of
    // error condition" -> repeat.
    Harness h;
    TEST_ASSERT_TRUE(h.open().isOk());
    h.gateway->acks = {control_packet::kStatusSequenceNumber, 0x00};
    const std::array<uint8_t, 2> cemi{0x11, 0x00};
    TEST_ASSERT_TRUE(h.client.sendCemi(cemi).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(h.socket().countSent(0x0420)));
}

void test_twice_unacknowledged_request_disconnects(void)
{
    Harness h;
    TEST_ASSERT_TRUE(h.open().isOk());
    const std::array<uint8_t, 2> cemi{0x11, 0x00};
    TEST_ASSERT_TRUE(h.client.sendCemi(cemi).isError());
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(h.network.countSent(0x0420)));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(h.network.countSent(0x0209)));
    TEST_ASSERT_EQUAL_UINT16(kControlPort, h.network.sentOf(0x0209).back().port);
    TEST_ASSERT_FALSE(h.client.isOpen());
}

void test_heartbeat_repeats_three_times_then_disconnects(void)
{
    // 03/08/02 §5.4: CONNECTIONSTATE_REQUEST, repeated three times on
    // timeout, then DISCONNECT_REQUEST.
    Harness h;
    TEST_ASSERT_TRUE(h.open().isOk());
    TEST_ASSERT_TRUE(h.client.heartbeat().isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(h.network.countSent(0x0207)));

    h.gateway->answerHeartbeat = false;
    TEST_ASSERT_TRUE(h.client.heartbeat().isError());
    TEST_ASSERT_EQUAL_UINT32(5u, static_cast<uint32_t>(h.network.countSent(0x0207)));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(h.network.countSent(0x0209)));
    TEST_ASSERT_FALSE(h.client.isOpen());
}

void test_server_disconnect_request_is_answered(void)
{
    Harness h;
    TEST_ASSERT_TRUE(h.open().isOk());
    auto& socket = h.socket();

    std::array<uint8_t, 32> bytes{};
    PacketWriter writer{std::span<uint8_t>(bytes)};
    const control_packet::HpaiEndpoint control{control_packet::HpaiProtocol::Udp, kGateway, kControlPort};
    TEST_ASSERT_TRUE(control_packet::Codec::encodeDisconnectRequest(writer, 7, control).isOk());
    socket.reply(kControlPort, writer.span());
    (void)h.client.poll(0);

    const auto responses = h.network.sentOf(0x020A);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(responses.size()));
    TEST_ASSERT_EQUAL_UINT8(7, responses[0].payload[6]);
    TEST_ASSERT_EQUAL_UINT8(0, responses[0].payload[7]);
    TEST_ASSERT_FALSE(h.client.isOpen());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_connect_response_hpai_and_crd_are_used);
    RUN_TEST(test_connect_refused_reports_failure);
    RUN_TEST(test_inbound_sequence_rules);
    RUN_TEST(test_unacknowledged_request_is_repeated_once_with_same_sequence);
    RUN_TEST(test_error_ack_counts_as_unconfirmed);
    RUN_TEST(test_twice_unacknowledged_request_disconnects);
    RUN_TEST(test_heartbeat_repeats_three_times_then_disconnects);
    RUN_TEST(test_server_disconnect_request_is_answered);
    return UNITY_END();
}
