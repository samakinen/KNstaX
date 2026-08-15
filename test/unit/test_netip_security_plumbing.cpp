// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/netip_security.hpp"
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

class PrefixSecurity final : public NetIpSecurity {
public:
    util::Result<size_t> protect(std::span<const uint8_t> in, std::span<uint8_t> out) override {
        if (out.size() < 3 + in.size()) return util::ErrorCode::BufferTooSmall;
        out[0] = 'S';
        out[1] = 'E';
        out[2] = 'C';
        std::memcpy(out.data() + 3, in.data(), in.size());
        return 3 + in.size();
    }

    util::Result<size_t> unprotect(std::span<const uint8_t> in, std::span<uint8_t> out) override {
        if (in.size() < 3) return util::ErrorCode::DecodeFailed;
        if (in[0] != 'S' || in[1] != 'E' || in[2] != 'C') return util::ErrorCode::DecodeFailed;
        if (out.size() < in.size() - 3) return util::ErrorCode::BufferTooSmall;
        std::memcpy(out.data(), in.data() + 3, in.size() - 3);
        return in.size() - 3;
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

    std::vector<uint8_t> securedPkt(plainPkt.size() + 3);
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
    const auto recvRes = ep.receiveRoutingIndication(gotCemi, 10);
    TEST_ASSERT_TRUE(recvRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(cemiView.size(), recvRes.value());
    for (size_t i = 0; i < cemiView.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(cemiView[i], gotCemi[i]);
    }

    ep.close();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_routing_endpoint_calls_security_wrapper_on_send_and_receive);
    return UNITY_END();
}
