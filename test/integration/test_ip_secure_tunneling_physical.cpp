// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#if KNX_SECURE_ENABLED

#include "knx/physical/ip_secure_tunneling_physical.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/netip/ip_secure/secure_session.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/platform/linux_platform.hpp"

#include "../common/vec_file.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace knx;
using namespace knx::datalink;
using namespace knx::physical;
using knx::netip::ip_secure::SecureSession;
using knx::netip::ip_secure::SecureWrapper;

namespace {

static Tp1DataLinkConfig polledRxConfig()
{
    auto config = Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;
    return config;
}

static uint16_t readU16BE(std::span<const uint8_t, 2> p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

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
    for (size_t i = 0; i < 6; ++i) v = (v << 8) | static_cast<uint64_t>(seq[i]);
    return v & 0xFFFFFFFFFFFFULL;
}

struct SharedTcpState {
    mutable std::mutex m;
    std::vector<uint8_t> inbound;
    std::vector<uint8_t> outbound;
};

static void pushInbound(const std::shared_ptr<SharedTcpState>& st, std::span<const uint8_t> frame)
{
    std::lock_guard<std::mutex> lock(st->m);
    st->inbound.insert(st->inbound.end(), frame.begin(), frame.end());
}

static std::vector<uint8_t> snapshotOutbound(const std::shared_ptr<SharedTcpState>& st)
{
    std::lock_guard<std::mutex> lock(st->m);
    return st->outbound;
}

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
        std::lock_guard<std::mutex> lock(st_->m);
        st_->outbound.insert(st_->outbound.end(), data.begin(), data.end());
        return static_cast<int>(data.size());
    }

    int receive(std::span<uint8_t> buffer) override {
        if (!open_) return -1;
        if (buffer.data() == nullptr && !buffer.empty()) return -1;

        std::lock_guard<std::mutex> lock(st_->m);
        if (st_->inbound.empty()) return 0;

        const size_t n = (buffer.size() < st_->inbound.size()) ? buffer.size() : st_->inbound.size();
        for (size_t i = 0; i < n; ++i) buffer[i] = st_->inbound[i];
        st_->inbound.erase(st_->inbound.begin(), st_->inbound.begin() + static_cast<std::ptrdiff_t>(n));
        return static_cast<int>(n);
    }

    size_t available() const override {
        std::lock_guard<std::mutex> lock(st_->m);
        return st_->inbound.size();
    }

    uint16_t localPort() const override { return 12345; }
    IpAddress localAddress() const override { return knx::IpAddress::fromString("127.0.0.1"); }

private:
    std::shared_ptr<SharedTcpState> st_;
    bool open_{false};
};

class MockNetworkInterface final : public knx::platform::NetworkInterface {
public:
    explicit MockNetworkInterface(std::shared_ptr<SharedTcpState> st) : st_(std::move(st)) {}

    knx::util::Result<void> init() override { return knx::util::Result<void>::ok(); }
    bool isConnected() const override { return true; }
    IpAddress ipAddress() const override { return IpAddress(0); }
    IpAddress subnetMask() const override { return IpAddress(0); }
    IpAddress gateway() const override { return IpAddress(0); }
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

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_ip_secure_tunneling_physical_send_receive(void)
{
    // Load vectors.
    std::string reqText;
    TEST_ASSERT_TRUE(knx_test::vec::readTextFile(
        "test/vectors/knxnetip_secure_session/secure_session_0951_request.vec", reqText));
    std::map<std::string, std::string> reqKv;
    TEST_ASSERT_TRUE(knx_test::vec::parseVec(reqText, reqKv));

    std::string respText;
    TEST_ASSERT_TRUE(knx_test::vec::readTextFile(
        "test/vectors/knxnetip_secure_session/secure_session_0952_response.vec", respText));
    std::map<std::string, std::string> respKv;
    TEST_ASSERT_TRUE(knx_test::vec::parseVec(respText, respKv));

    std::string authPlainText;
    TEST_ASSERT_TRUE(knx_test::vec::readTextFile(
        "test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_plain.vec", authPlainText));
    std::map<std::string, std::string> authPlainKv;
    TEST_ASSERT_TRUE(knx_test::vec::parseVec(authPlainText, authPlainKv));

    std::string authWrappedText;
    TEST_ASSERT_TRUE(knx_test::vec::readTextFile(
        "test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_wrapped.vec", authWrappedText));
    std::map<std::string, std::string> authWrappedKv;
    TEST_ASSERT_TRUE(knx_test::vec::parseVec(authWrappedText, authWrappedKv));

    std::vector<uint8_t> respFrame;
    TEST_ASSERT_TRUE(knx_test::vec::getHex(respKv, "frame", respFrame));

    std::vector<uint8_t> clientPrivBytes;
    TEST_ASSERT_TRUE(knx_test::vec::getHex(reqKv, "client_priv", clientPrivBytes));
    TEST_ASSERT_EQUAL_UINT32(32, clientPrivBytes.size());

    std::vector<uint8_t> userIdBytes;
    std::vector<uint8_t> passwordLatin1;
    TEST_ASSERT_TRUE(knx_test::vec::getHex(authPlainKv, "user_id", userIdBytes));
    TEST_ASSERT_TRUE(knx_test::vec::getHex(authPlainKv, "password_latin1", passwordLatin1));
    TEST_ASSERT_EQUAL_UINT32(1, userIdBytes.size());

    std::vector<uint8_t> sessionKeyBytes;
    TEST_ASSERT_TRUE(knx_test::vec::getHex(authWrappedKv, "session_key", sessionKeyBytes));
    std::array<uint8_t, 16> sessionKey{};
    TEST_ASSERT_TRUE(toFixed16(sessionKeyBytes, sessionKey));

    std::vector<uint8_t> sidBytes;
    TEST_ASSERT_TRUE(knx_test::vec::getHex(authWrappedKv, "sid", sidBytes));
    TEST_ASSERT_EQUAL_UINT32(2, sidBytes.size());
    const SessionId sid(static_cast<uint16_t>((static_cast<uint16_t>(sidBytes[0]) << 8) | sidBytes[1]));

    std::vector<uint8_t> seqBytes;
    TEST_ASSERT_TRUE(knx_test::vec::getHex(authWrappedKv, "seq", seqBytes));
    const uint64_t initialSeq = seq48FromBytes(seqBytes);
    TEST_ASSERT_TRUE(initialSeq != 0);

    std::vector<uint8_t> serialBytes;
    TEST_ASSERT_TRUE(knx_test::vec::getHex(authWrappedKv, "serial", serialBytes));
    std::array<uint8_t, 6> clientSerial{};
    TEST_ASSERT_TRUE(toFixed6(serialBytes, clientSerial));

    SecureWrapper::Key swKey{};
    for (size_t i = 0; i < 16; ++i) swKey[i] = sessionKey[i];

    // Common secure wrapper fields for tunnelling.
    const std::array<uint8_t, SecureWrapper::kTagLen> tag{0x00, 0x00};
    const std::array<uint8_t, 4> counterSuffix{0x00, 0x00, 0xFF, 0x00};
    const std::array<uint8_t, 6> serverSerial{0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

    // Server frames: 0952 response, then wrapped 0954 status (OK).
    std::array<uint8_t, SecureSession::kSessionStatusFrameLen> statusPlain{};
    TEST_ASSERT_TRUE(SecureSession::encodeSessionStatus(/*status=*/0x00, statusPlain).isOk());

    std::vector<uint8_t> statusWrapped(SecureWrapper::kOverhead + statusPlain.size());
    auto statusWrapRes = SecureWrapper::wrap(swKey,
                                             sid,
                                             /*seq=*/{0x00, 0x00, 0x00, 0x00, 0x00, 0x02},
                                             serverSerial,
                                             tag,
                                             counterSuffix,
                                             statusPlain,
                                             statusWrapped);
    TEST_ASSERT_TRUE(statusWrapRes.isOk());
    statusWrapped.resize(statusWrapRes.value());

    // Wrapped CONNECT_RESPONSE (channel id 0x11, status OK)
    std::vector<uint8_t> connectRespPlain;
    connectRespPlain.reserve(16);
    connectRespPlain.push_back(0x06);
    connectRespPlain.push_back(0x10);
    connectRespPlain.push_back(0x02);
    connectRespPlain.push_back(0x06);
    connectRespPlain.push_back(0x00);
    connectRespPlain.push_back(0x10);
    connectRespPlain.push_back(0x11);
    connectRespPlain.push_back(0x00);
    connectRespPlain.push_back(0x08);
    connectRespPlain.push_back(0x02);
    connectRespPlain.push_back(127);
    connectRespPlain.push_back(0);
    connectRespPlain.push_back(0);
    connectRespPlain.push_back(1);
    connectRespPlain.push_back(0x1F);
    connectRespPlain.push_back(0x90);

    std::vector<uint8_t> connectRespWrapped(SecureWrapper::kOverhead + connectRespPlain.size());
    auto connectWrapRes = SecureWrapper::wrap(swKey,
                                              sid,
                                              /*seq=*/{0x00, 0x00, 0x00, 0x00, 0x00, 0x03},
                                              serverSerial,
                                              tag,
                                              counterSuffix,
                                              connectRespPlain,
                                              connectRespWrapped);
    TEST_ASSERT_TRUE(connectWrapRes.isOk());
    connectRespWrapped.resize(connectWrapRes.value());

    // Pre-script an ACK for the first outbound tunnelling request (seq 0).
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
                                          serverSerial,
                                          tag,
                                          counterSuffix,
                                          ackPlain,
                                          ackWrapped);
    TEST_ASSERT_TRUE(ackWrapRes.isOk());
    ackWrapped.resize(ackWrapRes.value());

    auto shared = std::make_shared<SharedTcpState>();
    pushInbound(shared, respFrame);
    pushInbound(shared, statusWrapped);
    pushInbound(shared, connectRespWrapped);
    pushInbound(shared, ackWrapped);

    MockNetworkInterface net(shared);
    knx::platform::LinuxPlatform platform;

    IpSecureTunnelingPhysical phys;
    phys.setNetworkInterface(&net);
    phys.setGateway(knx::IpAddress::fromOctets(127, 0, 0, 1), NetIpPort(knx::netip::config::kDefaultPort));

    std::array<uint8_t, 32> clientPriv{};
    for (size_t i = 0; i < 32; ++i) clientPriv[i] = clientPrivBytes[i];

    phys.setCredentials(UserId(userIdBytes[0]), passwordLatin1, clientPriv, clientSerial, initialSeq);

    Tp1DataLinkLayer dl(platform, phys, nullptr, polledRxConfig());
    TEST_ASSERT_TRUE(dl.init(IndividualAddress(0x1111)).isOk());
    TEST_ASSERT_TRUE(dl.addGroupAddress(GroupAddress(0x0100)).isOk());

    // TX: send one frame through data link (expects secure tunnelling request + ACK).
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

    // Verify last outbound secured frame wraps a TUNNELING_REQUEST.
    const std::vector<uint8_t> outbound = snapshotOutbound(shared);
    std::vector<std::vector<uint8_t>> frames;
    TEST_ASSERT_TRUE(splitFrames(outbound, frames));
    TEST_ASSERT_TRUE(frames.size() >= 4);

    std::vector<uint8_t> parsed(frames.back().size() - 22);
    auto unwrapRes = SecureWrapper::unwrapAndVerify(swKey, sid, frames.back(), parsed);
    TEST_ASSERT_TRUE(unwrapRes.isOk());
    TEST_ASSERT_TRUE(unwrapRes.value() >= 6);
    TEST_ASSERT_EQUAL_HEX16(0x0420, readU16BE(std::span<const uint8_t, 2>(parsed.data() + 2, 2)));

    // RX: inject a secured incoming tunnelling request with cEMI L_Data.ind.
    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto cemiResult = netip::encodeCemiLData(out, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult.isOk());

    std::vector<uint8_t> treq;
    treq.reserve(10 + cemiResult.value());
    treq.push_back(0x06);
    treq.push_back(0x10);
    treq.push_back(0x04);
    treq.push_back(0x20);
    const uint16_t treqLen = static_cast<uint16_t>(10 + cemiResult.value());
    treq.push_back(static_cast<uint8_t>((treqLen >> 8) & 0xFF));
    treq.push_back(static_cast<uint8_t>(treqLen & 0xFF));
    treq.push_back(0x04);
    treq.push_back(0x11);
    treq.push_back(0x05);
    treq.push_back(0x00);
    treq.insert(treq.end(), cemi.begin(), cemi.begin() + cemiResult.value());

    std::vector<uint8_t> treqWrapped(SecureWrapper::kOverhead + treq.size());
    auto treqWrapRes = SecureWrapper::wrap(swKey,
                                           sid,
                                           /*seq=*/{0x00, 0x00, 0x00, 0x00, 0x00, 0x05},
                                           serverSerial,
                                           tag,
                                           counterSuffix,
                                           treq,
                                           treqWrapped);
    TEST_ASSERT_TRUE(treqWrapRes.isOk());
    treqWrapped.resize(treqWrapRes.value());
    pushInbound(shared, treqWrapped);

    std::atomic<bool> got{false};
    dl.setReceiveCallback([&](const LDataFrame& in) {
        if (in.destination.raw == out.destination.raw && in.source.raw == out.source.raw) {
            got.store(true);
        }
    });

    for (int attempt = 0; attempt < 30 && !got.load(); ++attempt) {
        (void)dl.processRxAvailable(0);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    TEST_ASSERT_TRUE(got.load());

    dl.close();
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_ip_secure_tunneling_physical_send_receive);
    return UNITY_END();
}

#else

void setUp(void) {}
void tearDown(void) {}
int main() {
    UNITY_BEGIN();
    return UNITY_END();
}

#endif // KNX_SECURE_ENABLED
