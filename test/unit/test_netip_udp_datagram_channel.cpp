// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/udp_datagram_channel.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/netip/secure_udp_datagram_channel.hpp"

#include <algorithm>
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

class PrefixSecurity final : public NetIpSecurity {
public:
    util::Result<size_t> protect(std::span<const uint8_t> in, std::span<uint8_t> out) override
    {
        if (out.size() < in.size() + 3) return util::ErrorCode::BufferTooSmall;
        out[0] = 'S';
        out[1] = 'E';
        out[2] = 'C';
        std::memcpy(out.data() + 3, in.data(), in.size());
        return in.size() + 3;
    }

    util::Result<size_t> unprotect(std::span<const uint8_t> in, std::span<uint8_t> out) override
    {
        if (in.size() < 3 || in[0] != 'S' || in[1] != 'E' || in[2] != 'C') return util::ErrorCode::DecodeFailed;
        if (out.size() < in.size() - 3) return util::ErrorCode::BufferTooSmall;
        std::memcpy(out.data(), in.data() + 3, in.size() - 3);
        return in.size() - 3;
    }
};

class TestUdpSocket final : public platform::UdpSocket {
public:
    util::Result<void> open(uint16_t port = 0) override
    {
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

    int send(IpAddress destAddr, uint16_t destPort, std::span<const uint8_t> data) override
    {
        lastDestAddr = destAddr;
        lastDestPort = destPort;
        lastSent.assign(data.begin(), data.end());
        return static_cast<int>(data.size());
    }

    int receive(std::span<uint8_t> buffer) override
    {
        IpAddress addr(0);
        uint16_t port = 0;
        return receive(buffer, addr, port);
    }

    int receive(std::span<uint8_t> buffer, IpAddress& srcAddr, uint16_t& srcPort) override
    {
        if (rx_.empty()) return -1;
        const auto& next = rx_.front();
        srcAddr = next.addr;
        srcPort = next.port;
        const size_t n = std::min(buffer.size(), next.bytes.size());
        std::memcpy(buffer.data(), next.bytes.data(), n);
        rx_.pop_front();
        return static_cast<int>(n);
    }

    size_t available() const override { return rx_.empty() ? 0 : rx_.front().bytes.size(); }
    uint16_t localPort() const override { return port_; }

    void pushRx(IpAddress addr, uint16_t port, std::span<const uint8_t> bytes)
    {
        rx_.push_back(RxPacket{addr, port, std::vector<uint8_t>(bytes.begin(), bytes.end())});
    }

    IpAddress lastDestAddr{IpAddress(0)};
    uint16_t lastDestPort{0};
    std::vector<uint8_t> lastSent;

private:
    struct RxPacket {
        IpAddress addr;
        uint16_t port;
        std::vector<uint8_t> bytes;
    };

    bool open_{false};
    uint16_t port_{0};
    std::deque<RxPacket> rx_;
};

} // namespace

void test_udp_datagram_channel_exchange_returns_plain_response(void)
{
    TestUdpSocket socket;
    TEST_ASSERT_TRUE(socket.open(12345).isOk());

    const UdpDatagramEndpoint remote{IpAddress::fromOctets(192, 168, 1, 10), NetIpPort(knx::netip::config::kDefaultPort)};
    const std::array<uint8_t, 4> request = {0x01, 0x02, 0x03, 0x04};
    const std::array<uint8_t, 3> response = {0xAA, 0xBB, 0xCC};
    std::array<uint8_t, 16> rxBuffer{};

    socket.pushRx(remote.addr, remote.port.value(), response);

    auto result = UdpDatagramChannel::exchange(socket, remote, request, rxBuffer, 10);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(response.size(), result.value());
    TEST_ASSERT_EQUAL_UINT32(request.size(), socket.lastSent.size());
    for (size_t i = 0; i < request.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(request[i], socket.lastSent[i]);
    }
    for (size_t i = 0; i < response.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(response[i], rxBuffer[i]);
    }
}

void test_udp_datagram_channel_exchange_rejects_wrong_peer(void)
{
    TestUdpSocket socket;
    TEST_ASSERT_TRUE(socket.open(12345).isOk());

    const UdpDatagramEndpoint remote{IpAddress::fromOctets(192, 168, 1, 10), NetIpPort(knx::netip::config::kDefaultPort)};
    const UdpDatagramEndpoint wrongPeer{IpAddress::fromOctets(192, 168, 1, 11), NetIpPort(knx::netip::config::kDefaultPort)};
    const std::array<uint8_t, 2> request = {0x01, 0x02};
    const std::array<uint8_t, 2> response = {0xAA, 0xBB};
    std::array<uint8_t, 8> rxBuffer{};

    socket.pushRx(wrongPeer.addr, wrongPeer.port.value(), response);

    auto result = UdpDatagramChannel::exchange(socket, remote, request, rxBuffer, 10);
    TEST_ASSERT_TRUE(result.isError());
}

void test_udp_datagram_channel_exchange_uses_security_wrapper(void)
{
    TestUdpSocket socket;
    PrefixSecurity security;
    TEST_ASSERT_TRUE(socket.open(12345).isOk());

    const UdpDatagramEndpoint remote{IpAddress::fromOctets(127, 0, 0, 1), NetIpPort(knx::netip::config::kDefaultPort)};
    const std::array<uint8_t, 4> request = {0x10, 0x20, 0x30, 0x40};
    const std::array<uint8_t, 4> plainResponse = {0xDE, 0xAD, 0xBE, 0xEF};
    std::array<uint8_t, 32> rxBuffer{};
    std::array<uint8_t, 32> wrapScratch{};
    std::array<uint8_t, 32> unwrapScratch{};
    SecureUdpDatagramChannel channel(socket, security, wrapScratch, unwrapScratch);

    auto protectedResponseLen = security.protect(plainResponse, wrapScratch);
    TEST_ASSERT_TRUE(protectedResponseLen.isOk());
    socket.pushRx(remote.addr, remote.port.value(), std::span<const uint8_t>(wrapScratch.data(), protectedResponseLen.value()));

    auto result = channel.exchange(remote, request, rxBuffer, 10);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(plainResponse.size(), result.value());
    TEST_ASSERT_EQUAL_UINT8('S', socket.lastSent[0]);
    TEST_ASSERT_EQUAL_UINT8('E', socket.lastSent[1]);
    TEST_ASSERT_EQUAL_UINT8('C', socket.lastSent[2]);
    for (size_t i = 0; i < plainResponse.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(plainResponse[i], rxBuffer[i]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_udp_datagram_channel_exchange_returns_plain_response);
    RUN_TEST(test_udp_datagram_channel_exchange_rejects_wrong_peer);
    RUN_TEST(test_udp_datagram_channel_exchange_uses_security_wrapper);
    return UNITY_END();
}