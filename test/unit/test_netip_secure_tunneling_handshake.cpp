// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_tunneling_client.hpp"

#if KNX_SECURE_ENABLED

#include "knx/netip/ip_secure/secure_session.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/util/hex.hpp"
#include "knx/netip/netip_config.hpp"

#include "../common/vec_file.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using knx::netip::ip_secure::SecureSession;
using knx::netip::ip_secure::SecureTunnelingClient;
using knx::netip::ip_secure::SecureWrapper;
using knx::NetIpPort;
using knx::SessionId;
using knx::UserId;

namespace {

static bool splitFrames(std::span<const uint8_t> stream, std::vector<std::vector<uint8_t>>& out)
{
    out.clear();
    size_t off = 0;
    while (off + 6 <= stream.size()) {
        const uint16_t totalLen = static_cast<uint16_t>((static_cast<uint16_t>(stream[off + 4]) << 8) | stream[off + 5]);
        if (totalLen < 6) return false;
        if (off + totalLen > stream.size()) return false;
        out.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(off),
                         stream.begin() + static_cast<std::ptrdiff_t>(off + totalLen));
        off += totalLen;
    }
    return off == stream.size();
}

static uint16_t readU16BE(std::span<const uint8_t, 2> p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

struct SharedTcpState {
    std::vector<uint8_t> inbound;  // bytes to be received by client
    std::vector<uint8_t> outbound; // bytes sent by client
};

class ScriptedTcpSocket final : public knx::platform::TcpSocket {
public:
    explicit ScriptedTcpSocket(std::shared_ptr<SharedTcpState> st) : st_(std::move(st)) {}

    knx::util::Result<void> connect(knx::IpAddress /*destAddr*/, uint16_t /*destPort*/) override {
        open_ = true;
        return knx::util::Result<void>::ok();
    }

    void close() override { open_ = false; }

    bool isOpen() const override { return open_; }

    int send(std::span<const uint8_t> data) override {
        if (!open_) return -1;
        if (data.data() == nullptr && !data.empty()) return -1;
        st_->outbound.insert(st_->outbound.end(), data.begin(), data.end());
        return static_cast<int>(data.size());
    }

    int receive(std::span<uint8_t> buffer) override {
        if (!open_) return -1;
        if (buffer.data() == nullptr && !buffer.empty()) return -1;
        if (st_->inbound.empty()) return 0;

        const size_t n = (buffer.size() < st_->inbound.size()) ? buffer.size() : st_->inbound.size();
        for (size_t i = 0; i < n; ++i) buffer[i] = st_->inbound[i];
        st_->inbound.erase(st_->inbound.begin(), st_->inbound.begin() + static_cast<std::ptrdiff_t>(n));
        return static_cast<int>(n);
    }

    size_t available() const override {
        return st_->inbound.size();
    }

    uint16_t localPort() const override { return 12345; }
    knx::IpAddress localAddress() const override { return knx::IpAddress::fromString("127.0.0.1"); }

private:
    std::shared_ptr<SharedTcpState> st_;
    bool open_{false};
};

class MockNetworkInterface final : public knx::platform::NetworkInterface {
public:
    explicit MockNetworkInterface(std::shared_ptr<SharedTcpState> st) : st_(std::move(st)) {}

    knx::util::Result<void> init() override { return knx::util::Result<void>::ok(); }
    bool isConnected() const override { return true; }
    knx::IpAddress ipAddress() const override { return knx::IpAddress(0); }
    knx::IpAddress subnetMask() const override { return knx::IpAddress(0); }
    knx::IpAddress gateway() const override { return knx::IpAddress(0); }
    void macAddress(std::span<uint8_t> mac) const override {
        if (mac.data() == nullptr || mac.size() < 6) return;
        for (int i = 0; i < 6; ++i) mac[i] = 0;
    }

    std::unique_ptr<knx::platform::UdpSocket> createUdpSocket() override { return nullptr; }
    std::unique_ptr<knx::platform::TcpSocket> createTcpSocket() override {
        return std::make_unique<ScriptedTcpSocket>(st_);
    }

private:
    std::shared_ptr<SharedTcpState> st_;
};

static bool toFixed16(std::span<const uint8_t> in, std::array<uint8_t, 16>& out)
{
    if (in.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) out[i] = in[i];
    return true;
}

static bool toFixed6(std::span<const uint8_t> in, std::array<uint8_t, 6>& out)
{
    if (in.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) out[i] = in[i];
    return true;
}

static uint64_t seq48FromBytes(std::span<const uint8_t> seq)
{
    if (seq.size() != 6) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < 6; ++i) {
        v = (v << 8) | static_cast<uint64_t>(seq[i]);
    }
    return v & 0xFFFFFFFFFFFFULL;
}

} // namespace

int main()
{
    // Load vectors.
    std::string reqText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0951_request.vec", reqText)) return 1;
    std::map<std::string, std::string> reqKv;
    if (!knx_test::vec::parseVec(reqText, reqKv)) return 2;

    std::string respText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0952_response.vec", respText)) return 3;
    std::map<std::string, std::string> respKv;
    if (!knx_test::vec::parseVec(respText, respKv)) return 4;

    std::string authPlainText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_plain.vec", authPlainText)) return 5;
    std::map<std::string, std::string> authPlainKv;
    if (!knx_test::vec::parseVec(authPlainText, authPlainKv)) return 6;

    std::string authWrappedText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_wrapped.vec", authWrappedText)) return 7;
    std::map<std::string, std::string> authWrappedKv;
    if (!knx_test::vec::parseVec(authWrappedText, authWrappedKv)) return 8;

    std::vector<uint8_t> reqFrameExpected;
    if (!knx_test::vec::getHex(reqKv, "frame", reqFrameExpected)) return 9;

    std::vector<uint8_t> clientPrivBytes;
    if (!knx_test::vec::getHex(reqKv, "client_priv", clientPrivBytes)) return 10;
    if (clientPrivBytes.size() != 32) return 11;

    std::vector<uint8_t> respFrame;
    if (!knx_test::vec::getHex(respKv, "frame", respFrame)) return 12;

    std::vector<uint8_t> userIdBytes;
    std::vector<uint8_t> passwordLatin1;
    if (!knx_test::vec::getHex(authPlainKv, "user_id", userIdBytes)) return 13;
    if (!knx_test::vec::getHex(authPlainKv, "password_latin1", passwordLatin1)) return 14;
    if (userIdBytes.size() != 1) return 15;

    std::vector<uint8_t> authWrappedExpected;
    if (!knx_test::vec::getHex(authWrappedKv, "frame", authWrappedExpected)) return 16;

    std::vector<uint8_t> sessionKeyBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "session_key", sessionKeyBytes)) return 17;
    std::array<uint8_t, 16> sessionKey{};
    if (!toFixed16(sessionKeyBytes, sessionKey)) return 18;

    std::vector<uint8_t> sidBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "sid", sidBytes)) return 19;
    if (sidBytes.size() != 2) return 20;
    const SessionId sid(static_cast<uint16_t>((static_cast<uint16_t>(sidBytes[0]) << 8) | sidBytes[1]));

    std::vector<uint8_t> seqBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "seq", seqBytes)) return 21;
    const uint64_t initialSeq = seq48FromBytes(seqBytes);
    if (initialSeq == 0) return 22;

    std::vector<uint8_t> serialBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "serial", serialBytes)) return 23;
    std::array<uint8_t, 6> clientSerial{};
    if (!toFixed6(serialBytes, clientSerial)) return 24;

    // Build a wrapped status response as the scripted "server".
    SecureSession::UserPasswordKey userPasswordKey{};
    if (SecureSession::deriveUserPasswordKeyLatin1(passwordLatin1, userPasswordKey).isError()) return 25;

    std::array<uint8_t, SecureSession::kSessionStatusFrameLen> statusPlain{};
    if (SecureSession::encodeSessionStatus(/*status=*/0x00, statusPlain).isError()) return 26;

    SecureWrapper::Key swKey{};
    for (size_t i = 0; i < 16; ++i) swKey[i] = sessionKey[i];

    const std::array<uint8_t, SecureWrapper::kTagLen> tag{0x00, 0x00};
    const std::array<uint8_t, 4> counterSuffix{0x00, 0x00, 0xFF, 0x00};
    const std::array<uint8_t, 6> serverSerial{0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const std::array<uint8_t, 6> serverSeq{0x00, 0x00, 0x00, 0x00, 0x00, 0x02};

    std::vector<uint8_t> statusWrapped(SecureWrapper::kOverhead + statusPlain.size());
    auto statusWrapRes = SecureWrapper::wrap(swKey, sid, serverSeq, serverSerial, tag, counterSuffix, statusPlain, statusWrapped);
    if (statusWrapRes.isError()) return 27;
    statusWrapped.resize(statusWrapRes.value());

    // Script the server to send: response(0952) then wrapped status(0950->0954)
    auto shared = std::make_shared<SharedTcpState>();
    shared->inbound.insert(shared->inbound.end(), respFrame.begin(), respFrame.end());
    shared->inbound.insert(shared->inbound.end(), statusWrapped.begin(), statusWrapped.end());

    MockNetworkInterface net(shared);

    SecureTunnelingClient::Options opt;
    opt.host = knx::IpAddress::fromOctets(127, 0, 0, 1);
    opt.port = NetIpPort(knx::netip::config::kDefaultPort);
    opt.userId = UserId(userIdBytes[0]);
    opt.passwordLatin1 = passwordLatin1;
    for (size_t i = 0; i < 32; ++i) opt.clientPrivateKey[i] = clientPrivBytes[i];
    opt.clientSerial = clientSerial;
    opt.initialSeq = initialSeq;

    SecureTunnelingClient client;
    auto openRes = client.beginOpen(net, opt, 200);
    if (openRes.isError()) {
        std::cout << "handshake_failed\n";
        return 40;
    }
    bool sawOpenPending = false;
    bool openCompleted = false;
    for (int attempt = 0; attempt < 20 && !openCompleted; ++attempt) {
        auto openProgress = client.pollOpen();
        if (openProgress.isError()) return 140;
        if (openProgress.value() == knx::util::OperationProgressState::Pending) {
            sawOpenPending = true;
        }
        if (openProgress.value() == knx::util::OperationProgressState::Success) {
            openCompleted = true;
        }
    }
    if (!openCompleted) return 141;
    if (!sawOpenPending) return 142;
    if (!client.isOpen()) return 41;
    if (!client.security()) return 42;

    // Validate what was sent: request then wrapped authenticate.
    std::vector<std::vector<uint8_t>> frames;
    if (!splitFrames(shared->outbound, frames)) return 43;
    if (frames.size() != 2) {
        std::cout << "sent_frames_count=" << frames.size() << "\n";
        return 44;
    }

    if (frames[0] != reqFrameExpected) {
        std::cout << "request_mismatch\n";
        std::cout << "got=" << knx::util::toHex(frames[0]) << "\n";
        std::cout << "expected=" << knx::util::toHex(reqFrameExpected) << "\n";
        return 45;
    }

    if (frames[1] != authWrappedExpected) {
        std::cout << "authenticate_wrapped_mismatch\n";
        std::cout << "got=" << knx::util::toHex(frames[1]) << "\n";
        std::cout << "expected=" << knx::util::toHex(authWrappedExpected) << "\n";
        return 46;
    }

    // ===== Tunnelling connect over secure wrapper =====
    // Prepare a plaintext CONNECT_RESPONSE and wrap it as the server.
    std::vector<uint8_t> connectRespPlain;
    connectRespPlain.reserve(16);
    connectRespPlain.push_back(0x06);
    connectRespPlain.push_back(0x10);
    connectRespPlain.push_back(0x02);
    connectRespPlain.push_back(0x06);
    connectRespPlain.push_back(0x00);
    connectRespPlain.push_back(0x10);
    connectRespPlain.push_back(0x11); // channel id
    connectRespPlain.push_back(0x00); // status ok
    connectRespPlain.push_back(0x08);
    connectRespPlain.push_back(0x02); // HPAI proto (TCP)
    connectRespPlain.push_back(127);
    connectRespPlain.push_back(0);
    connectRespPlain.push_back(0);
    connectRespPlain.push_back(1);
    connectRespPlain.push_back(0x1F);
    connectRespPlain.push_back(0x90);

    std::vector<uint8_t> connectRespWrapped(SecureWrapper::kOverhead + connectRespPlain.size());
    auto connectRespWrapRes = SecureWrapper::wrap(swKey,
                             sid,
                             /*seq=*/{0x00, 0x00, 0x00, 0x00, 0x00, 0x02},
                             /*serial=*/{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
                             tag,
                             counterSuffix,
                             connectRespPlain,
                             connectRespWrapped);
    if (connectRespWrapRes.isError()) return 60;
    connectRespWrapped.resize(connectRespWrapRes.value());
    shared->inbound.insert(shared->inbound.end(), connectRespWrapped.begin(), connectRespWrapped.end());

    auto connectRes = client.beginConnectTunneling(200);
    if (connectRes.isError()) return 61;
    bool connectCompleted = false;
    for (int attempt = 0; attempt < 20 && !connectCompleted; ++attempt) {
        auto connectProgress = client.pollConnectTunneling();
        if (connectProgress.isError()) return 143;
        if (connectProgress.value() == knx::util::OperationProgressState::Success) {
            connectCompleted = true;
        }
    }
    if (!connectCompleted) return 144;
    if (!client.isTunnelingConnected()) return 62;
    if (client.channelId().value() != 0x11) return 63;

    // Validate that the next sent frame is a wrapped CONNECT_REQUEST.
    std::vector<std::vector<uint8_t>> framesAfterConnect;
    if (!splitFrames(shared->outbound, framesAfterConnect)) return 64;
    if (framesAfterConnect.size() < 3) return 65;

    std::vector<uint8_t> crPlain(framesAfterConnect[2].size() - 22);
    auto crUnwrapRes = SecureWrapper::unwrapAndVerify(swKey, sid, framesAfterConnect[2], crPlain);
    if (crUnwrapRes.isError()) return 66;
    if (crUnwrapRes.value() < 6) return 67;
    if (readU16BE(std::span<const uint8_t, 2>(crPlain.data() + 2, 2)) != 0x0205) return 68;

    // ===== Connection-state request over secure wrapper =====
    std::vector<uint8_t> stateRespPlain;
    stateRespPlain.reserve(8);
    stateRespPlain.push_back(0x06);
    stateRespPlain.push_back(0x10);
    stateRespPlain.push_back(0x02);
    stateRespPlain.push_back(0x08);
    stateRespPlain.push_back(0x00);
    stateRespPlain.push_back(0x08);
    stateRespPlain.push_back(0x11);
    stateRespPlain.push_back(0x00);

    std::vector<uint8_t> stateRespWrapped(SecureWrapper::kOverhead + stateRespPlain.size());
    auto stateRespWrapRes = SecureWrapper::wrap(swKey,
                           sid,
                           /*seq=*/{0x00, 0x00, 0x00, 0x00, 0x00, 0x03},
                           /*serial=*/{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
                           tag,
                           counterSuffix,
                           stateRespPlain,
                           stateRespWrapped);
    if (stateRespWrapRes.isError()) return 145;
    stateRespWrapped.resize(stateRespWrapRes.value());
    shared->inbound.insert(shared->inbound.end(), stateRespWrapped.begin(), stateRespWrapped.end());

    auto stateReqRes = client.beginConnectionStateRequest(200);
    if (stateReqRes.isError()) return 146;
    bool stateCompleted = false;
    for (int attempt = 0; attempt < 20 && !stateCompleted; ++attempt) {
        auto stateProgress = client.pollConnectionStateRequest();
        if (stateProgress.isError()) return 147;
        if (stateProgress.value() == knx::util::OperationProgressState::Success) {
            stateCompleted = true;
        }
    }
    if (!stateCompleted) return 148;

    std::vector<std::vector<uint8_t>> framesAfterState;
    if (!splitFrames(shared->outbound, framesAfterState)) return 149;
    if (framesAfterState.size() < 4) return 150;

    std::vector<uint8_t> statePlain(framesAfterState[3].size() - 22);
    auto stateUnwrapRes = SecureWrapper::unwrapAndVerify(swKey, sid, framesAfterState[3], statePlain);
    if (stateUnwrapRes.isError()) return 151;
    if (stateUnwrapRes.value() < 6) return 152;
    if (readU16BE(std::span<const uint8_t, 2>(statePlain.data() + 2, 2)) != 0x0207) return 153;

    // ===== One secured tunnelling request + ack =====
    // Prepare an ACK for the first tunnelling request (seq 0).
    std::vector<uint8_t> ackPlain;
    ackPlain.reserve(10);
    ackPlain.push_back(0x06);
    ackPlain.push_back(0x10);
    ackPlain.push_back(0x04);
    ackPlain.push_back(0x21);
    ackPlain.push_back(0x00);
    ackPlain.push_back(0x0A);
    ackPlain.push_back(0x04);
    ackPlain.push_back(0x11);
    ackPlain.push_back(0x00);
    ackPlain.push_back(0x00);

    std::vector<uint8_t> ackWrapped(SecureWrapper::kOverhead + ackPlain.size());
    auto ackWrapRes = SecureWrapper::wrap(swKey,
                             sid,
                             /*seq=*/{0x00, 0x00, 0x00, 0x00, 0x00, 0x04},
                             /*serial=*/{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
                             tag,
                             counterSuffix,
                             ackPlain,
                             ackWrapped);
    if (ackWrapRes.isError()) return 70;
    ackWrapped.resize(ackWrapRes.value());
    shared->inbound.insert(shared->inbound.end(), ackWrapped.begin(), ackWrapped.end());

    std::vector<uint8_t> cemi = {0x11, 0x00, 0xBC, 0xE0, 0x00, 0x00};
    auto sendRes = client.sendCemi(cemi, true, 200);
    if (sendRes.isError()) return 71;

    // Ensure the last outbound frame is a wrapped tunnelling request.
    std::vector<std::vector<uint8_t>> framesAfterCemi;
    if (!splitFrames(shared->outbound, framesAfterCemi)) return 72;
    if (framesAfterCemi.size() < 5) return 73;
    std::vector<uint8_t> trPlain(framesAfterCemi[4].size() - 22);
    auto trUnwrapRes = SecureWrapper::unwrapAndVerify(swKey, sid, framesAfterCemi[4], trPlain);
    if (trUnwrapRes.isError()) return 74;
    if (trUnwrapRes.value() < 10) return 75;
    if (readU16BE(std::span<const uint8_t, 2>(trPlain.data() + 2, 2)) != 0x0420) return 76;

    // ===== Tunnelling disconnect over secure wrapper =====
    std::vector<uint8_t> disconnectRespPlain;
    disconnectRespPlain.reserve(8);
    disconnectRespPlain.push_back(0x06);
    disconnectRespPlain.push_back(0x10);
    disconnectRespPlain.push_back(0x02);
    disconnectRespPlain.push_back(0x0A);
    disconnectRespPlain.push_back(0x00);
    disconnectRespPlain.push_back(0x08);
    disconnectRespPlain.push_back(0x11);
    disconnectRespPlain.push_back(0x00);

    std::vector<uint8_t> disconnectRespWrapped(SecureWrapper::kOverhead + disconnectRespPlain.size());
    auto disconnectRespWrapRes = SecureWrapper::wrap(swKey,
                                sid,
                                /*seq=*/{0x00, 0x00, 0x00, 0x00, 0x00, 0x05},
                                /*serial=*/{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
                                tag,
                                counterSuffix,
                                disconnectRespPlain,
                                disconnectRespWrapped);
    if (disconnectRespWrapRes.isError()) return 154;
    disconnectRespWrapped.resize(disconnectRespWrapRes.value());
    shared->inbound.insert(shared->inbound.end(), disconnectRespWrapped.begin(), disconnectRespWrapped.end());

    auto disconnectRes = client.beginDisconnectTunneling(200);
    if (disconnectRes.isError()) return 155;
    bool disconnectCompleted = false;
    for (int attempt = 0; attempt < 20 && !disconnectCompleted; ++attempt) {
        auto disconnectProgress = client.pollDisconnectTunneling();
        if (disconnectProgress.isError()) return 156;
        if (disconnectProgress.value() == knx::util::OperationProgressState::Success) {
            disconnectCompleted = true;
        }
    }
    if (!disconnectCompleted) return 157;
    if (client.isTunnelingConnected()) return 158;

    std::vector<std::vector<uint8_t>> framesAfterDisconnect;
    if (!splitFrames(shared->outbound, framesAfterDisconnect)) return 159;
    if (framesAfterDisconnect.size() < 6) return 160;

    std::vector<uint8_t> disconnectPlain(framesAfterDisconnect[5].size() - 22);
    auto disconnectUnwrapRes = SecureWrapper::unwrapAndVerify(swKey, sid, framesAfterDisconnect[5], disconnectPlain);
    if (disconnectUnwrapRes.isError()) return 161;
    if (disconnectUnwrapRes.value() < 6) return 162;
    if (readU16BE(std::span<const uint8_t, 2>(disconnectPlain.data() + 2, 2)) != 0x0209) return 163;

    return 0;
}

#else

int main() { return 0; }

#endif // KNX_SECURE_ENABLED
