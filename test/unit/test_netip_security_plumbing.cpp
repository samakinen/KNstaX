// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/routing_security.hpp"
#include "knx/netip/routing.hpp"
#include "knx/netip/routing_endpoint.hpp"
#include "knx/netip/netip_config.hpp"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/cemi.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::netip;
using namespace knx::datalink;

void setUp(void) {}
void tearDown(void) {}

namespace {

/// Stands in for real IP Secure while keeping the one property the endpoint
/// depends on: a protected datagram is a KNXnet/IP frame whose service type is
/// SECURE_WRAPPER. The endpoint classifies from the header before it decrypts —
/// that is what lets discovery and system broadcasts stay readable on a secured
/// group — so a stub that emitted arbitrary bytes would be testing a transport
/// that cannot exist.
class PrefixSecurity final : public RoutingSecurity {
public:
    static constexpr size_t kHeaderLen = KnxNetIpCodec::kHeaderLen;

    // TIMER_NOTIFY plumbing: what the endpoint handed in, and a canned frame
    // to hand out when `notifyDue` is set.
    util::Result<void> handleTimerNotify(std::span<const uint8_t> datagram) override {
        timerNotifiesHandled.emplace_back(datagram.begin(), datagram.end());
        return util::Result<void>::ok();
    }

    util::Result<size_t> takeDueTimerNotify(std::span<uint8_t> out) override {
        if (!notifyDue) return size_t{0};
        notifyDue = false;
        std::copy(cannedNotify.begin(), cannedNotify.end(), out.begin());
        return cannedNotify.size();
    }

    uint32_t millisecondsUntilTimerNotify() override { return notifyDue ? 0u : 60000u; }

    std::vector<std::vector<uint8_t>> timerNotifiesHandled;
    bool notifyDue{false};
    std::array<uint8_t, 36> cannedNotify{0x06, 0x10, 0x09, 0x55, 0x00, 0x24};

    util::Result<size_t> protect(std::span<const uint8_t> in, std::span<uint8_t> out) override {
        if (out.size() < kHeaderLen + in.size()) return util::ErrorCode::BufferTooSmall;
        auto headerResult = KnxNetIpCodec::encodeHeader(
            ip_secure::SecureWrapper::kServiceTypeSecureWrapper,
            in.size(),
            std::span<uint8_t, kHeaderLen>(out.data(), kHeaderLen));
        if (headerResult.isError()) return headerResult.error();
        std::memcpy(out.data() + kHeaderLen, in.data(), in.size());
        return kHeaderLen + in.size();
    }

    util::Result<size_t> unprotect(std::span<const uint8_t> in, std::span<uint8_t> out) override {
        KnxNetIpHeader header;
        auto headerResult = KnxNetIpCodec::decodeHeader(in, header);
        if (headerResult.isError()) return headerResult.error();
        if (header.serviceType != ip_secure::SecureWrapper::kServiceTypeSecureWrapper) {
            return util::ErrorCode::DecodeFailed;
        }
        if (header.totalLength > in.size()) return util::ErrorCode::InvalidFrameSize;
        const size_t payload = header.totalLength - kHeaderLen;
        if (out.size() < payload) return util::ErrorCode::BufferTooSmall;
        std::memcpy(out.data(), in.data() + kHeaderLen, payload);
        return payload;
    }
};

class TestUdpSocket final : public platform::UdpSocket {
public:
    util::Result<void> open(uint16_t port = 0) override {
        open_ = true;
        port_ = port;
        return util::Result<void>::ok();
    }

    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

    util::Result<void> joinMulticast(IpAddress, IpAddress) override { return util::Result<void>::ok(); }
    void leaveMulticast(IpAddress, IpAddress) override {}

    util::Result<void> setMulticastInterface(IpAddress) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastLoopback(platform::MulticastLoopbackMode) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastTtl(uint8_t) override { return util::Result<void>::ok(); }

    int send(IpAddress destAddr, uint16_t destPort, std::span<const uint8_t> data) override {
        (void)destAddr;
        (void)destPort;
        if (!open_ || (data.data() == nullptr && !data.empty())) return -1;
        lastSent.assign(data.begin(), data.end());
        return static_cast<int>(data.size());
    }

    int receive(std::span<uint8_t> buffer) override {
        IpAddress a(0);
        uint16_t p = 0;
        return receive(buffer, a, p);
    }

    int receive(std::span<uint8_t> buffer, IpAddress& srcAddr, uint16_t& srcPort) override {
        (void)srcAddr;
        (void)srcPort;
        if (!open_ || buffer.data() == nullptr || buffer.size() == 0) return -1;
        if (rx_.empty()) return -1;
        const auto pkt = rx_.front();
        rx_.pop_front();
        const size_t n = (pkt.size() > buffer.size()) ? buffer.size() : pkt.size();
        std::memcpy(buffer.data(), pkt.data(), n);
        return static_cast<int>(n);
    }

    size_t available() const override { return rx_.empty() ? 0 : rx_.front().size(); }
    uint16_t localPort() const override { return port_; }

    void pushRx(std::span<const uint8_t> pkt) { rx_.push_back(std::vector<uint8_t>(pkt.begin(), pkt.end())); }

    std::vector<uint8_t> lastSent;

private:
    bool open_{false};
    uint16_t port_{0};
    std::deque<std::vector<uint8_t>> rx_;
};

class TestNetwork final : public platform::NetworkInterface {
public:
    util::Result<void> init() override { return util::Result<void>::ok(); }
    bool isConnected() const override { return true; }

    IpAddress ipAddress() const override { return IpAddress(0); }
    IpAddress subnetMask() const override { return IpAddress(0); }
    IpAddress gateway() const override { return IpAddress(0); }
    void macAddress(std::span<uint8_t> mac) const override {
        if (mac.data() == nullptr || mac.size() < 6) return;
        for (int i = 0; i < 6; ++i) mac[i] = 0;
    }

    std::unique_ptr<platform::UdpSocket> createUdpSocket() override {
        auto s = std::make_unique<TestUdpSocket>();
        sock = s.get();
        return s;
    }

    TestUdpSocket* sock{nullptr};
};

static LDataFrame makeFrame()
{
    LDataFrame f;
    f.standardFrame = true;
    f.repeated = false;
    f.priority = Priority::Normal;
    f.ackRequested = true;
    f.confirmation = true;
    f.source = IndividualAddress(0x110A);
    f.destination = GroupAddress(0x2301);
    f.destinationType = AddressType::Group;
    f.hopCount = 6;
    f.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
    return f;
}

} // namespace

void test_routing_endpoint_calls_security_wrapper_on_send_and_receive(void)
{
    TestNetwork net;
    PrefixSecurity sec;

    RoutingEndpoint ep;
    RoutingEndpoint::Options opt;
    opt.multicastGroup = knx::IpAddress::fromOctets(239, 255, 250, 1);
    opt.port = NetIpPort(knx::netip::config::kDefaultPort);
    opt.interfaceAddress = knx::IpAddress::fromOctets(127, 0, 0, 1);
    opt.security = &sec;

    TEST_ASSERT_TRUE(ep.open(net, opt).isOk());
    TEST_ASSERT_NOT_NULL(net.sock);

    // Build a cEMI payload and compute the expected (plaintext) routing datagram.
    const LDataFrame frame = makeFrame();
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    auto cemiResult = encodeCemiLData(frame, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult.isOk());
    const auto cemiView = std::span<const uint8_t>(cemi.data(), cemiResult.value());

    std::vector<uint8_t> plainPkt(RoutingCodec::KNXNETIP_HEADER_LEN + cemiView.size());
    auto plainPktResult = RoutingCodec::encodeRoutingIndication(cemiView, plainPkt);
    TEST_ASSERT_TRUE(plainPktResult.isOk());
    plainPkt.resize(plainPktResult.value());

    std::vector<uint8_t> securedPkt(plainPkt.size() + PrefixSecurity::kHeaderLen);
    auto securedLen = sec.protect(plainPkt, securedPkt);
    TEST_ASSERT_TRUE(securedLen.isOk());
    securedPkt.resize(securedLen.value());

    // Send: socket should see secured bytes.
    const auto sendRes = ep.sendRoutingIndication(cemiView);
    TEST_ASSERT_TRUE(sendRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(securedPkt.size(), net.sock->lastSent.size());
    for (size_t i = 0; i < securedPkt.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(securedPkt[i], net.sock->lastSent[i]);
    }

    // Receive: feed secured bytes and ensure we get the original cEMI.
    net.sock->pushRx(securedPkt);

    std::array<uint8_t, kMaxCemiLDataSize> gotCemi{};
    const auto recvRes = ep.receive(gotCemi, 10);
    TEST_ASSERT_TRUE(recvRes.isOk());
    TEST_ASSERT_TRUE(recvRes.value().kind == RoutingEndpoint::InboundKind::RoutingIndication);
    TEST_ASSERT_EQUAL_UINT32(cemiView.size(), recvRes.value().cemiLength);
    for (size_t i = 0; i < cemiView.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(cemiView[i], gotCemi[i]);
    }

    ep.close();
}

namespace {

struct OpenEndpoint {
    TestNetwork net;
    RoutingEndpoint ep;

    explicit OpenEndpoint(RoutingSecurity* security)
    {
        RoutingEndpoint::Options opt;
        opt.multicastGroup = knx::IpAddress::fromOctets(239, 255, 250, 1);
        opt.port = NetIpPort(knx::netip::config::kDefaultPort);
        opt.interfaceAddress = knx::IpAddress::fromOctets(127, 0, 0, 1);
        opt.security = security;
        TEST_ASSERT_TRUE(ep.open(net, opt).isOk());
    }
};

std::array<uint8_t, RoutingCodec::ROUTING_BUSY_FRAME_LEN> busyFrame(uint16_t waitMs)
{
    std::array<uint8_t, RoutingCodec::ROUTING_BUSY_FRAME_LEN> pkt{};
    const RoutingBusyInfo info{.deviceState = 0, .waitTimeMs = waitMs, .controlField = 0};
    TEST_ASSERT_TRUE(RoutingCodec::encodeRoutingBusy(info, pkt).isOk());
    return pkt;
}

} // namespace

// A5: 03_08_09 §2.2.1.4.5 — a secured group takes its routing service family
// wrapped. A plain ROUTING_BUSY used to be honoured, so anyone on the LAN could
// stop every secured sender for up to 65 s per frame.
void test_plain_routing_busy_is_ignored_on_a_secured_group(void)
{
    PrefixSecurity sec;
    OpenEndpoint o(&sec);

    o.net.sock->pushRx(busyFrame(100));
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    const auto res = o.ep.receive(cemi, 10);
    TEST_ASSERT_TRUE(res.isError());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(o.ep.routingBusySeen()));
    TEST_ASSERT_FALSE(o.ep.isSendBlocked());
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(o.ep.plainOnSecuredGroupDropped()));
}

void test_plain_routing_lost_message_is_ignored_on_a_secured_group(void)
{
    PrefixSecurity sec;
    OpenEndpoint o(&sec);

    std::array<uint8_t, RoutingCodec::ROUTING_LOST_MESSAGE_FRAME_LEN> pkt{};
    TEST_ASSERT_TRUE(RoutingCodec::encodeRoutingLostMessage(RoutingLostMessageInfo{0, 5}, pkt).isOk());
    o.net.sock->pushRx(pkt);
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    TEST_ASSERT_TRUE(o.ep.receive(cemi, 10).isError());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(o.ep.routingLostMessagesSeen()));
}

void test_wrapped_routing_busy_is_honoured_on_a_secured_group(void)
{
    PrefixSecurity sec;
    OpenEndpoint o(&sec);

    const auto plain = busyFrame(100);
    std::vector<uint8_t> wrapped(plain.size() + PrefixSecurity::kHeaderLen);
    TEST_ASSERT_TRUE(sec.protect(plain, wrapped).isOk());
    o.net.sock->pushRx(wrapped);

    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    TEST_ASSERT_TRUE(o.ep.receive(cemi, 10).isOk());
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(o.ep.routingBusySeen()));
    TEST_ASSERT_TRUE(o.ep.isSendBlocked());
}

// A5: tw "SHALL not exceed 100 ms" (03_08_05 §2.3.5). 65 535 ms as sent was
// obeyed before.
void test_routing_busy_wait_is_capped(void)
{
    OpenEndpoint o(nullptr);
    o.net.sock->pushRx(busyFrame(0xFFFF));
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    TEST_ASSERT_TRUE(o.ep.receive(cemi, 10).isOk());
    TEST_ASSERT_TRUE(o.ep.isSendBlocked());
    TEST_ASSERT_TRUE(o.ep.millisecondsUntilSendable() <= RoutingFlowControl::kMaxTotalBusyWaitMs);
}

// A6: the random back-off draws from the injected source. A source that
// always answers "maximum" must lengthen the wait beyond tw; before, the
// source was stored and never read, and the wait was always exactly tw.
void test_routing_busy_backoff_uses_the_random_source(void)
{
    OpenEndpoint o(nullptr);
    o.ep.setRandomSource([](std::span<uint8_t> out) { std::fill(out.begin(), out.end(), 0xFF); });
    o.net.sock->pushRx(busyFrame(20));
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    TEST_ASSERT_TRUE(o.ep.receive(cemi, 10).isOk());
    // tw = 20 ms, trandom ~ 1 * N(=1) * 50 ms.
    TEST_ASSERT_TRUE(o.ep.millisecondsUntilSendable() > 40u);
}

// A2: TIMER_NOTIFY reaches the security context; without one it is ignored
// (03_08_09 §2.2.1.4.5: "no TimerNotify frames may be sent or received").
void test_timer_notify_is_handed_to_the_security_context(void)
{
    PrefixSecurity sec;
    OpenEndpoint o(&sec);

    o.net.sock->pushRx(sec.cannedNotify);
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    const auto res = o.ep.receive(cemi, 10);
    TEST_ASSERT_TRUE(res.isOk());
    TEST_ASSERT_TRUE(res.value().kind == RoutingEndpoint::InboundKind::None);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(sec.timerNotifiesHandled.size()));
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(o.ep.timerNotifiesAccepted()));
}

void test_timer_notify_is_ignored_without_secure_routing(void)
{
    OpenEndpoint o(nullptr);
    const std::array<uint8_t, 36> notify{0x06, 0x10, 0x09, 0x55, 0x00, 0x24};
    o.net.sock->pushRx(notify);
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    const auto res = o.ep.receive(cemi, 10);
    TEST_ASSERT_TRUE(res.isOk());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(o.ep.timerNotifiesAccepted()));
}

// A2: a notify that falls due while the receive loop waits is sent from it,
// in the clear (it authenticates itself).
void test_due_timer_notify_is_sent_from_the_receive_loop(void)
{
    PrefixSecurity sec;
    OpenEndpoint o(&sec);
    sec.notifyDue = true;
    sec.cannedNotify[35] = 0xA5;

    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    (void)o.ep.receive(cemi, 1);
    TEST_ASSERT_EQUAL_UINT32(36, static_cast<uint32_t>(o.net.sock->lastSent.size()));
    TEST_ASSERT_EQUAL_UINT8(0x55, o.net.sock->lastSent[3]);
    TEST_ASSERT_EQUAL_UINT8(0xA5, o.net.sock->lastSent[35]);
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(o.ep.timerNotifiesSent()));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_routing_endpoint_calls_security_wrapper_on_send_and_receive);
    RUN_TEST(test_plain_routing_busy_is_ignored_on_a_secured_group);
    RUN_TEST(test_plain_routing_lost_message_is_ignored_on_a_secured_group);
    RUN_TEST(test_wrapped_routing_busy_is_honoured_on_a_secured_group);
    RUN_TEST(test_routing_busy_wait_is_capped);
    RUN_TEST(test_routing_busy_backoff_uses_the_random_source);
    RUN_TEST(test_timer_notify_is_handed_to_the_security_context);
    RUN_TEST(test_timer_notify_is_ignored_without_secure_routing);
    RUN_TEST(test_due_timer_notify_is_sent_from_the_receive_loop);
    return UNITY_END();
}
