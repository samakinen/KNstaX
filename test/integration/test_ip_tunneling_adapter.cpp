// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"
#include "knx/physical/ip_tunneling_physical.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/application/apci_services.hpp"

#include "knx/platform/linux_platform.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>

#include <thread>
#include <atomic>
#include <array>
#include <vector>
#include <cstring>

using namespace knx;
using namespace knx::datalink;
using namespace knx::physical;

static void writeHeader(std::vector<uint8_t>& buf, uint16_t service, uint16_t totalLen) {
    std::array<uint8_t, knx::netip::KnxNetIpCodec::kHeaderLen> header{};
    auto result = knx::netip::KnxNetIpCodec::encodeHeader(
        knx::NetIpServiceType(service),
        totalLen - knx::netip::KnxNetIpCodec::kHeaderLen,
        header);
    if (result.isError()) return;
    buf.insert(buf.end(), header.begin(), header.end());
}

void setUp(void) {}
void tearDown(void) {}

static Tp1DataLinkConfig polledRxConfig() {
    auto config = Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;
    return config;
}

void test_adapter_send_receive(void) {
    // Mock KNXnet/IP gateway
    int srv = ::socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_TRUE(srv >= 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(0);
    TEST_ASSERT_TRUE(::bind(srv, (sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t alen = sizeof(addr);
    ::getsockname(srv, (sockaddr*)&addr, &alen);
    uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> running{true};
    sockaddr_in lastClient{}; socklen_t lastLen = sizeof(lastClient);

    std::thread server([&]() {
        uint8_t buf[1500];
        while (running.load()) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(srv, &rfds);
            timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
            int r = ::select(srv + 1, &rfds, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            sockaddr_in src{}; socklen_t slen = sizeof(src);
            ssize_t n = ::recvfrom(srv, buf, sizeof(buf), 0, (sockaddr*)&src, &slen);
            if (n < 6) continue;
            uint16_t st = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
            lastClient = src; lastLen = slen;

            if (st == 0x0205) { // Connection Request
                std::vector<uint8_t> resp;
                writeHeader(resp, 0x0206, 8);
                resp.push_back(0x01); // channel id
                resp.push_back(0x00); // status ok
                ::sendto(srv, resp.data(), resp.size(), 0, (sockaddr*)&src, slen);
            } else if (st == 0x0420) { // Tunneling Request
                // ACK back
                if (n >= 10) {
                    uint8_t seq = buf[8];
                    std::vector<uint8_t> ack;
                    writeHeader(ack, 0x0421, 10);
                    ack.push_back(0x04); // tunneling hdr len
                    ack.push_back(0x01); // channel id
                    ack.push_back(seq);
                    ack.push_back(0x00); // status ok
                    ::sendto(srv, ack.data(), ack.size(), 0, (sockaddr*)&src, slen);
                }
            }
        }
    });

    // Adapter + Data Link Layer
    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpTunnelingPhysical phys;
    phys.setNetworkInterface(net);
    phys.setGateway(knx::IpAddress::fromOctets(127, 0, 0, 1), NetIpPort(port));
    Tp1DataLinkLayer dl(platform, phys);
    TEST_ASSERT_TRUE(dl.init(IndividualAddress(0x1111)).isOk());
    // Accept destination group address used below
    TEST_ASSERT_TRUE(dl.addGroupAddress(GroupAddress(0x0100)).isOk());
    // Send a simple L_Data frame through data link
    LDataFrame out;
    out.standardFrame = true;
    out.repeated = false;
    out.priority = Priority::Low;
    out.ackRequested = false; // Don't wait for ACK in this test
    out.confirmation = false;
    out.source = IndividualAddress(0x1111);
    out.destination = GroupAddress(0x0100);
    out.destinationType = AddressType::Group;
    out.hopCount = 6;
    out.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueRead, {});
    TEST_ASSERT_TRUE(dl.sendFrame(out).isOk());

    // Prepare inbound cEMI tunneled L_Data.ind
    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    // Use incoming indication message code 0x29
    auto cemiResult = netip::encodeCemiLData(out, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult.isOk());
    std::vector<uint8_t> req;
    writeHeader(req, 0x0420, static_cast<uint16_t>(10 + cemiResult.value()));
    req.push_back(0x04);
    req.push_back(0x01);
    req.push_back(0x00);
    req.push_back(0x00);
    req.insert(req.end(), cemi.begin(), cemi.begin() + cemiResult.value());
    ::sendto(srv, req.data(), req.size(), 0, (sockaddr*)&lastClient, lastLen);

    std::atomic<bool> got{false};
    dl.setReceiveCallback([&](const LDataFrame& in){
        got.store(true);
    });

    // Wait only as long as needed for the loopback delivery to be processed.
    for (int i = 0; i < 40 && !got.load(); ++i) {
        ::usleep(5 * 1000);
    }

    TEST_ASSERT_TRUE(got.load());

    running.store(false);
    server.join();
    ::close(srv);
}

void test_adapter_send_receive_with_explicit_progression(void) {
    int srv = ::socket(AF_INET, SOCK_DGRAM, 0);
    TEST_ASSERT_TRUE(srv >= 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(0);
    TEST_ASSERT_TRUE(::bind(srv, (sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t alen = sizeof(addr);
    ::getsockname(srv, (sockaddr*)&addr, &alen);
    uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> running{true};
    sockaddr_in lastClient{}; socklen_t lastLen = sizeof(lastClient);

    std::thread server([&]() {
        uint8_t buf[1500];
        while (running.load()) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(srv, &rfds);
            timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
            int r = ::select(srv + 1, &rfds, nullptr, nullptr, &tv);
            if (r <= 0) continue;
            sockaddr_in src{}; socklen_t slen = sizeof(src);
            ssize_t n = ::recvfrom(srv, buf, sizeof(buf), 0, (sockaddr*)&src, &slen);
            if (n < 6) continue;
            uint16_t st = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
            lastClient = src; lastLen = slen;

            if (st == 0x0205) {
                std::vector<uint8_t> resp;
                writeHeader(resp, 0x0206, 8);
                resp.push_back(0x01);
                resp.push_back(0x00);
                ::sendto(srv, resp.data(), resp.size(), 0, (sockaddr*)&src, slen);
            } else if (st == 0x0420 && n >= 10) {
                uint8_t seq = buf[8];
                std::vector<uint8_t> ack;
                writeHeader(ack, 0x0421, 10);
                ack.push_back(0x04);
                ack.push_back(0x01);
                ack.push_back(seq);
                ack.push_back(0x00);
                ::sendto(srv, ack.data(), ack.size(), 0, (sockaddr*)&src, slen);
            }
        }
    });

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpTunnelingPhysical phys;
    phys.setNetworkInterface(net);
    phys.setGateway(knx::IpAddress::fromOctets(127, 0, 0, 1), NetIpPort(port));
    Tp1DataLinkLayer dl(platform, phys, nullptr, polledRxConfig());
    TEST_ASSERT_TRUE(dl.init(IndividualAddress(0x1111)).isOk());
    TEST_ASSERT_TRUE(dl.addGroupAddress(GroupAddress(0x0100)).isOk());

    LDataFrame out;
    out.standardFrame = true;
    out.repeated = false;
    out.priority = Priority::Low;
    out.ackRequested = false;
    out.confirmation = false;
    out.source = IndividualAddress(0x1111);
    out.destination = GroupAddress(0x0100);
    out.destinationType = AddressType::Group;
    out.hopCount = 6;
    out.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueRead, {});
    TEST_ASSERT_TRUE(dl.sendFrame(out).isOk());

    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto cemiResult = netip::encodeCemiLData(out, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult.isOk());
    std::vector<uint8_t> req;
    writeHeader(req, 0x0420, static_cast<uint16_t>(10 + cemiResult.value()));
    req.push_back(0x04);
    req.push_back(0x01);
    req.push_back(0x00);
    req.push_back(0x00);
    req.insert(req.end(), cemi.begin(), cemi.begin() + cemiResult.value());
    ::sendto(srv, req.data(), req.size(), 0, (sockaddr*)&lastClient, lastLen);

    std::atomic<bool> got{false};
    dl.setReceiveCallback([&](const LDataFrame&) {
        got.store(true);
    });

    for (int i = 0; i < 40 && !got.load(); ++i) {
        (void)dl.processRxAvailable(0);
        ::usleep(5 * 1000);
    }

    TEST_ASSERT_TRUE(got.load());

    running.store(false);
    server.join();
    ::close(srv);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_adapter_send_receive);
    RUN_TEST(test_adapter_send_receive_with_explicit_progression);
    return UNITY_END();
}
