// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/tunneling_session_client.hpp"
#include "knx/netip/device_management.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/platform/linux_platform.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <span>
#include <thread>
#include <vector>

using knx::netip::TunnelingSessionClient;
using knx::NetIpPort;

namespace {

void writeHeader(std::vector<uint8_t>& buf, uint16_t service, uint16_t totalLen)
{
    std::array<uint8_t, knx::netip::KnxNetIpCodec::kHeaderLen> header{};
    auto result = knx::netip::KnxNetIpCodec::encodeHeader(
        knx::NetIpServiceType(service),
        totalLen - knx::netip::KnxNetIpCodec::kHeaderLen,
        header);
    if (result.isError()) return;
    buf.insert(buf.end(), header.begin(), header.end());
}

} // namespace

int main()
{
    int srv = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (srv < 0) return 1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return 1;
    socklen_t alen = sizeof(addr);
    ::getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &alen);
    const uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> running{true};
    std::atomic<bool> disconnectReceived{false};
    std::atomic<bool> keepaliveReceived{false};
    sockaddr_in lastClient{};
    socklen_t lastLen = sizeof(lastClient);
    sockaddr_in lastTunnelingClient{};
    socklen_t lastTunnelingLen = sizeof(lastTunnelingClient);

    std::thread server([&]() {
        uint8_t buf[1500];
        while (running.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(srv, &rfds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200 * 1000;
            const int ready = ::select(srv + 1, &rfds, nullptr, nullptr, &tv);
            if (ready <= 0) continue;
            sockaddr_in src{};
            socklen_t slen = sizeof(src);
            const ssize_t n = ::recvfrom(srv, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&src), &slen);
            if (n < 6) continue;
            const uint16_t serviceType = (static_cast<uint16_t>(buf[2]) << 8) | buf[3];
            lastClient = src;
            lastLen = slen;

            if (serviceType == 0x0205) {
                lastTunnelingClient = src;
                lastTunnelingLen = slen;
                std::vector<uint8_t> resp;
                writeHeader(resp, 0x0206, 8);
                resp.push_back(0x01);
                resp.push_back(0x00);
                ::sendto(srv, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&src), slen);
            } else if (serviceType == 0x0209) {
                disconnectReceived.store(true);
                std::vector<uint8_t> resp;
                writeHeader(resp, 0x020A, 8);
                resp.push_back(0x01);
                resp.push_back(0x00);
                ::sendto(srv, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&src), slen);
            } else if (serviceType == 0x0207) {
                lastTunnelingClient = src;
                lastTunnelingLen = slen;
                keepaliveReceived.store(true);
                std::vector<uint8_t> resp;
                writeHeader(resp, 0x0208, 8);
                resp.push_back(0x01);
                resp.push_back(0x00);
                ::sendto(srv, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&src), slen);
            } else if (serviceType == 0x0420 && n >= 10) {
                lastTunnelingClient = src;
                lastTunnelingLen = slen;
                const uint8_t seq = buf[8];
                std::vector<uint8_t> ack;
                writeHeader(ack, 0x0421, 10);
                ack.push_back(0x04);
                ack.push_back(0x01);
                ack.push_back(seq);
                ack.push_back(0x00);
                ::sendto(srv, ack.data(), ack.size(), 0, reinterpret_cast<sockaddr*>(&src), slen);
            } else if (serviceType == 0x0310 && n >= 17) {
                std::vector<uint8_t> resp;
                writeHeader(resp, 0x0311, 19);
                resp.push_back(0x04);
                resp.push_back(buf[7]);
                resp.push_back(buf[8]);
                resp.push_back(0x00);
                resp.push_back(0xFB);
                resp.push_back(buf[11]);
                resp.push_back(buf[12]);
                resp.push_back(buf[13]);
                resp.push_back(buf[14]);
                resp.push_back(buf[15]);
                resp.push_back(buf[16]);
                resp.push_back(0x12);
                resp.push_back(0x34);
                ::sendto(srv, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&src), slen);
            }
        }
    });

    TunnelingSessionClient client;
    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    if (!net) {
        running.store(false);
        server.join();
        ::close(srv);
        return 11;
    }
    if (!net->init()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 12;
    }

    auto openRes = client.beginOpen(*net, knx::IpAddress::fromOctets(127, 0, 0, 1), NetIpPort(port), 500);
    if (openRes.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 2;
    }
    bool opened = false;
    for (int attempt = 0; attempt < 100 && !opened; ++attempt) {
        auto openProgress = client.pollOpen();
        if (openProgress.isError()) {
            running.store(false);
            server.join();
            ::close(srv);
            return 21;
        }
        if (openProgress.value() == knx::util::OperationProgressState::Success) {
            opened = true;
        } else if (openProgress.value() == knx::util::OperationProgressState::Timeout) {
            running.store(false);
            server.join();
            ::close(srv);
            return 22;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!opened ||
        client.activeSessionOperation() != knx::netip::TunnelingSessionClient::SessionOperationType::None) {
        running.store(false);
        server.join();
        ::close(srv);
        return 23;
    }

    auto dmHeader = client.acquireDeviceManagementConnectionHeader();
    if (dmHeader.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 13;
    }
    if (dmHeader.value().channelId.value() != 0x01 || dmHeader.value().sequenceCounter != 0x00) {
        running.store(false);
        server.join();
        ::close(srv);
        return 14;
    }
    if (client.sequence().value() != 0x01) {
        running.store(false);
        server.join();
        ::close(srv);
        return 15;
    }

    auto keepaliveRes = client.sendConnectionStateRequest(500);
    if (keepaliveRes.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 3;
    }

    std::atomic<bool> pollDone{false};
    std::thread poller([&]() {
        auto pollRes = client.poll(200);
        if (pollRes.isError()) {
            pollDone.store(false);
            return;
        }
        pollDone.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto concurrentKeepaliveStart = std::chrono::steady_clock::now();
    auto concurrentKeepaliveRes = client.sendConnectionStateRequest(500);
    const auto concurrentKeepaliveMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - concurrentKeepaliveStart).count();

    poller.join();

    if (concurrentKeepaliveRes.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 31;
    }
    if (concurrentKeepaliveMs >= 180) {
        running.store(false);
        server.join();
        ::close(srv);
        return 32;
    }
    if (!pollDone.load()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 33;
    }

    std::vector<uint8_t> cemi = {0x29, 0x00, 0x00, 0x00, 0x11, 0x11, 0x01, 0x00, 0x01, 0x00, 0x00};
    auto sentRes = client.sendCemi(cemi, true, 500);
    if (sentRes.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 4;
    }

    knx::netip::DeviceManagementClient dmClient;
    auto dmOpenRes = dmClient.open(*net, knx::IpAddress::fromOctets(127, 0, 0, 1), NetIpPort(port));
    if (dmOpenRes.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 16;
    }
    dmClient.bindSession(client);

    knx::netip::device_management::PropertyAccessTarget target;
    target.objectType = knx::InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = knx::InterfaceObjectInstance(0x01);
    target.propertyId = knx::application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;
    std::array<uint8_t, 32> dmResponse{};
    auto dmBeginRes = dmClient.beginReadProperty(target, dmResponse, 500);
    if (dmBeginRes.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 17;
    }
    if (!dmClient.isOperationPending()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 24;
    }

    knx::netip::device_management::PropertyReadConfirmationView dmReadRes{};
    bool dmCompleted = false;
    for (int attempt = 0; attempt < 100 && !dmCompleted; ++attempt) {
        auto dmProgress = dmClient.pollReadProperty(dmReadRes);
        if (dmProgress.isError()) {
            running.store(false);
            server.join();
            ::close(srv);
            return 17;
        }
        if (dmProgress.value() == knx::util::OperationProgressState::Success) {
            dmCompleted = true;
        } else if (dmProgress.value() == knx::util::OperationProgressState::Timeout) {
            running.store(false);
            server.join();
            ::close(srv);
            return 17;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!dmCompleted) {
        running.store(false);
        server.join();
        ::close(srv);
        return 17;
    }
    if (dmClient.isOperationPending()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 25;
    }
    if (dmReadRes.connection.channelId.value() != 0x01 || dmReadRes.connection.sequenceCounter != 0x02) {
        running.store(false);
        server.join();
        ::close(srv);
        return 18;
    }
    if (dmReadRes.data.size() != 2 || dmReadRes.data[0] != 0x12 || dmReadRes.data[1] != 0x34) {
        running.store(false);
        server.join();
        ::close(srv);
        return 19;
    }
    if (client.sequence().value() != 0x03) {
        running.store(false);
        server.join();
        ::close(srv);
        return 20;
    }

    client.startKeepalive(60000);
    const auto stopStart = std::chrono::steady_clock::now();
    client.stopKeepalive();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stopStart).count();
    if (stopMs > 250) {
        running.store(false);
        server.join();
        ::close(srv);
        return 5;
    }

    std::vector<uint8_t> req;
    writeHeader(req, 0x0420, static_cast<uint16_t>(10 + cemi.size()));
    req.push_back(0x04);
    req.push_back(0x01);
    req.push_back(0x00);
    req.push_back(0x00);
    req.insert(req.end(), cemi.begin(), cemi.end());
    ::sendto(srv, req.data(), req.size(), 0, reinterpret_cast<sockaddr*>(&lastTunnelingClient), lastTunnelingLen);

    std::atomic<bool> got{false};
    client.setReceiveCallback([&](std::span<const uint8_t> frame) {
        if (frame.size() == cemi.size() && std::memcmp(frame.data(), cemi.data(), cemi.size()) == 0) {
            got.store(true);
        }
    });
    (void)client.poll(500);

    dmClient.close();
    client.close();
    for (int i = 0; i < 20 && !disconnectReceived.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (client.getTimeSinceLastActivity() != 0) return 9;

    int rc = 0;
    if (!got.load()) rc = 6;
    if (!disconnectReceived.load()) rc = 7;
    if (!keepaliveReceived.load()) rc = 8;

    running.store(false);
    server.join();
    ::close(srv);
    return rc;
}