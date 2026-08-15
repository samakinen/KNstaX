// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/gateway_discovery_client.hpp"
#include "knx/netip/tunneling_session_client.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/netip/header_codec.hpp"

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
#include <cstdio>
#include <chrono>

#include <span>

#include "knx/platform/linux_platform.hpp"

using knx::netip::GatewayDiscoveryClient;
using knx::netip::TunnelingSessionClient;
using knx::NetIpPort;
using knx::netip::GatewayInfo;

static void writeHeader(std::vector<uint8_t>& buf, uint16_t service, uint16_t totalLen) {
    std::array<uint8_t, knx::netip::KnxNetIpCodec::kHeaderLen> header{};
    auto result = knx::netip::KnxNetIpCodec::encodeHeader(
        knx::NetIpServiceType(service),
        totalLen - knx::netip::KnxNetIpCodec::kHeaderLen,
        header);
    if (result.isError()) return;
    buf.insert(buf.end(), header.begin(), header.end());
}

static uint16_t readU16(std::span<const uint8_t> p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

int main() {
    // Mock KNXnet/IP gateway
    int srv = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (srv < 0) return 1;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(0);
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) return 1;
    socklen_t alen = sizeof(addr);
    ::getsockname(srv, (sockaddr*)&addr, &alen);
    uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> running{true};
    std::atomic<bool> disconnectReceived{false};
    std::atomic<bool> descriptionReceived{false};
    std::atomic<bool> keepaliveReceived{false};
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
                // Header + Channel ID + Status (8 bytes total)
                writeHeader(resp, 0x0206, 8);
                resp.push_back(0x01); // channel id
                resp.push_back(0x00); // status ok
                ::sendto(srv, resp.data(), resp.size(), 0, (sockaddr*)&src, slen);
            } else if (st == 0x0203) { // Description Request
                // Expected body: HPAI control endpoint
                // Total length must be 14 bytes.
                if (n >= 14 && readU16(std::span<const uint8_t>(buf + 4, 2)) == 14) {
                    // Validate HPAI
                    if (buf[6] == 0x08 && buf[7] == 0x01) {
                        // Client must advertise a usable control endpoint (port != 0).
                        uint16_t clientPort = readU16(std::span<const uint8_t>(buf + 12, 2));
                        if (clientPort != 0) {
                            descriptionReceived.store(true);

                                // Build DESCRIPTION_RESPONSE: [Header][HPAI ctrl][DIB device][DIB svc]
                                std::vector<uint8_t> resp;
                                resp.resize(6, 0);

                                // HPAI control endpoint (gateway)
                                resp.push_back(0x08);
                                resp.push_back(0x01);
                                resp.push_back(127);
                                resp.push_back(0);
                                resp.push_back(0);
                                resp.push_back(1);
                                resp.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
                                resp.push_back(static_cast<uint8_t>(port & 0xFF));

                                // Device Info DIB (Type 0x01, len 54)
                                resp.push_back(0x36);
                                resp.push_back(0x01);
                                resp.push_back(0x02); // medium
                                resp.push_back(0x00); // status
                                resp.push_back(0x11); // IA high
                                resp.push_back(0x11); // IA low
                                resp.push_back(0x00); // proj-inst high
                                resp.push_back(0x01); // proj-inst low
                                // serial (6)
                                resp.insert(resp.end(), {0, 1, 2, 3, 4, 5});
                                // multicast addr (4)
                                resp.insert(resp.end(), {224, 0, 23, 12});
                                // MAC (6)
                                resp.insert(resp.end(), {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF});
                                // Friendly name (30, padded)
                                const char name[] = "MockGateway";
                                for (size_t i = 0; i < 30; ++i) {
                                    resp.push_back(i < (sizeof(name) - 1) ? static_cast<uint8_t>(name[i]) : 0x00);
                                }

                                // Supported Service Families DIB (Type 0x02)
                                // Provide 2 families: Core (0x02) and Tunneling (0x04)
                                resp.insert(resp.end(), {0x08, 0x02, 0x02, 0x01, 0x04, 0x01, 0x00, 0x00});

                                std::vector<uint8_t> headerOnly;
                                writeHeader(headerOnly, 0x0204, static_cast<uint16_t>(resp.size()));
                                std::copy(headerOnly.begin(), headerOnly.end(), resp.begin());

                            ::sendto(srv, resp.data(), resp.size(), 0, (sockaddr*)&src, slen);
                        }
                    }
                }
            } else if (st == 0x0209) { // Disconnect Request
                // Body: channel id (1) + reserved (1) + HPAI control endpoint (8)
                // Total length must be 16 bytes.
                if (n >= 16 && readU16(std::span<const uint8_t>(buf + 4, 2)) == 16 && buf[6] == 0x01 && buf[7] == 0x00) {
                    // Validate HPAI
                    if (buf[8] == 0x08 && buf[9] == 0x01) {
                        // Client must advertise a usable control endpoint (port != 0).
                        uint16_t clientPort = readU16(std::span<const uint8_t>(buf + 14, 2));
                        if (clientPort != 0) {
                            disconnectReceived.store(true);

                            // Send DISCONNECT_RESPONSE
                            std::vector<uint8_t> resp;
                            writeHeader(resp, 0x020A, 8);
                            resp.push_back(0x01); // channel id
                            resp.push_back(0x00); // status ok
                            ::sendto(srv, resp.data(), resp.size(), 0, (sockaddr*)&src, slen);
                        }
                    }
                }
            } else if (st == 0x0207) { // Connection State Request
                if (n >= 16 && readU16(std::span<const uint8_t>(buf + 4, 2)) == 16 && buf[6] == 0x01 && buf[7] == 0x00) {
                    if (buf[8] == 0x08 && buf[9] == 0x01) {
                        uint16_t clientPort = readU16(std::span<const uint8_t>(buf + 14, 2));
                        if (clientPort != 0) {
                            keepaliveReceived.store(true);

                            std::vector<uint8_t> resp;
                            writeHeader(resp, 0x0208, 8);
                            resp.push_back(0x01); // channel id
                            resp.push_back(0x00); // status ok
                            ::sendto(srv, resp.data(), resp.size(), 0, (sockaddr*)&src, slen);
                        }
                    }
                }
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

    // Client
    GatewayDiscoveryClient discoveryClient;
    TunnelingSessionClient sessionClient;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    if (!net) { running.store(false); server.join(); ::close(srv); return 11; }
    if (!net->init()) { running.store(false); server.join(); ::close(srv); return 12; }

    auto openRes = sessionClient.open(*net, knx::IpAddress::fromOctets(127, 0, 0, 1), NetIpPort(port), 500);
    if (openRes.isError()) { running.store(false); server.join(); ::close(srv); return 2; }

    // DESCRIPTION_REQUEST/RESPONSE round trip
    GatewayInfo info;
    auto descRes = discoveryClient.getDescription(*net, knx::IpAddress::fromOctets(127, 0, 0, 1), NetIpPort(port), 500, info);
    if (descRes.isError()) { running.store(false); server.join(); ::close(srv); return 6; }
    if (info.friendlyName != "MockGateway") { running.store(false); server.join(); ::close(srv); return 7; }
    if (info.deviceDIB.size() != 0x36 || info.supportedServices.size() != 0x08) {
        running.store(false); server.join(); ::close(srv); return 8;
    }

    auto keepaliveRes = sessionClient.sendConnectionStateRequest(500);
    if (keepaliveRes.isError()) { running.store(false); server.join(); ::close(srv); return 14; }

    // Prepare a simple cEMI frame (message code + minimal body)
    std::vector<uint8_t> cemi;
    // Use L_Data.ind (0x29) with no additional info and empty payload for simplicity
    cemi.push_back(0x29); // Message Code
    cemi.push_back(0x00); // Additional info length
    // Control fields (dummy)
    cemi.push_back(0x00);
    cemi.push_back(0x00);
    // Src addr
    cemi.push_back(0x11); cemi.push_back(0x11);
    // Dest addr (group)
    cemi.push_back(0x01); cemi.push_back(0x00);
    // Data length
    cemi.push_back(0x01);
    // TPDU (TPCI + APCI/Data)
    cemi.push_back(0x00);
    cemi.push_back(0x00);

    // Send and wait for ACK
    auto sentRes = sessionClient.sendCemi(cemi, true, 500);
    if (sentRes.isError()) { running.store(false); server.join(); ::close(srv); return 3; }

    // Keepalive thread lifecycle: stop must not block for the full interval.
    sessionClient.startKeepalive(60000);
    const auto kaStart = std::chrono::steady_clock::now();
    sessionClient.stopKeepalive();
    const auto kaStopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - kaStart).count();
    if (kaStopMs > 250) { running.store(false); server.join(); ::close(srv); return 10; }

    // Simulate server sending a tunneling request towards client
    std::vector<uint8_t> req;
    writeHeader(req, 0x0420, static_cast<uint16_t>(10 + cemi.size()));
    req.push_back(0x04);
    req.push_back(0x01);
    req.push_back(0x00);
    req.push_back(0x00);
    req.insert(req.end(), cemi.begin(), cemi.end());
    ::sendto(srv, req.data(), req.size(), 0, (sockaddr*)&lastClient, lastLen);

    // Receive once on client and verify callback
    std::atomic<bool> got{false};
    sessionClient.setReceiveCallback([&](std::span<const uint8_t> frame) {
        if (frame.size() == cemi.size() && std::memcmp(frame.data(), cemi.data(), cemi.size()) == 0) {
            got.store(true);
        }
    });

    // Poll receive once with timeout to process the incoming tunneling request
    (void)sessionClient.poll(500);

    // Closing should best-effort send a DISCONNECT_REQUEST to the known remote
    sessionClient.close();
    for (int i = 0; i < 20 && !disconnectReceived.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int rc = 0;
    if (!got.load()) rc = 4;
    if (!disconnectReceived.load()) rc = 5;
    if (!descriptionReceived.load()) rc = 9;
    if (!keepaliveReceived.load()) rc = 13;

    running.store(false);
    server.join();
    ::close(srv);
    return rc;
}
