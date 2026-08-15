// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#if KNX_SECURE_ENABLED

#include "knx/netip/device_management_codec.hpp"
#include "knx/netip/device_management_connection_source.hpp"
#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/netip/ip_secure/secure_session.hpp"
#include "knx/netip/ip_secure/secure_session_bootstrap.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/netip_security.hpp"
#include "knx/netip/secure_tcp_frame_channel.hpp"
#include "knx/netip/tunneling_orchestration.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/security/x25519.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/types.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace knx {
namespace netip {
namespace ip_secure {

class SecureTunnelingSecurity final : public knx::netip::NetIpSecurity {
public:
    using Key = SecureWrapper::Key;

    SecureTunnelingSecurity(const Key& sessionKey,
                            SessionId sessionId,
                            const std::array<uint8_t, SecureWrapper::kSerialLen>& localSerial,
                            uint64_t initialSeq = 1);

    util::Result<size_t> protect(std::span<const uint8_t> in, std::span<uint8_t> out) override;
    util::Result<size_t> unprotect(std::span<const uint8_t> in, std::span<uint8_t> out) override;

    SessionId sessionId() const { return sessionId_; }

private:
    static inline constexpr void encodeSeq48BE_(uint64_t seq48, std::span<uint8_t, SecureWrapper::kSeqLen> out) noexcept;

    Key sessionKey_{};
    SessionId sessionId_{SessionId::invalid()};
    std::array<uint8_t, SecureWrapper::kSerialLen> serial_{};
    uint64_t seq48_{1};
};

class SecureTunnelingClient {
public:
    using ReceiveCallback = util::InplaceFunction<void(std::span<const uint8_t>), 64>;

    enum class SessionOperationType : uint8_t {
        None = 0,
        OpenSecureSession,
        ConnectTunneling,
        ConnectionStateRequest,
        DisconnectTunneling,
    };

    struct Options {
        IpAddress host;
        NetIpPort port{NetIpPort(knx::netip::config::kDefaultPort)};

        UserId userId{UserId(1)};
        std::vector<uint8_t> passwordLatin1;

        // Caller-provided Curve25519 private key (32 bytes) for deterministic behavior.
        // (If all zeros, open() will fail rather than guessing entropy sources.)
        knx::security::X25519::Scalar clientPrivateKey{};

        // 6-byte KNX device serial number used in SecureWrapper.
        std::array<uint8_t, SecureWrapper::kSerialLen> clientSerial{};

        // Initial 48-bit sequence counter for SecureWrapper.
        uint64_t initialSeq{1};
    };

    SecureTunnelingClient();
    ~SecureTunnelingClient();

    SecureTunnelingClient(const SecureTunnelingClient&) = delete;
    SecureTunnelingClient& operator=(const SecureTunnelingClient&) = delete;

    util::Result<void> beginOpen(platform::NetworkInterface& network, const Options& options, int timeoutMs = 1000);
    util::Result<util::OperationProgressState> pollOpen();
    util::Result<void> open(platform::NetworkInterface& network, const Options& options);
    void close();
    bool isOpen() const noexcept;
    void setTimingPlatform(platform::TimingPlatform* timingPlatform) noexcept { timingPlatform_ = timingPlatform; }
    platform::TimingPlatform* timingPlatform() const noexcept { return timingPlatform_; }

    // ===== KNXnet/IP tunnelling over the secured TCP stream =====
    // These mirror the UDP tunneling-session client behavior but operate on this TCP session.
    util::Result<void> beginConnectTunneling(int timeoutMs = 1000);
    util::Result<util::OperationProgressState> pollConnectTunneling();
    util::Result<void> connectTunneling(int timeoutMs = 1000);
    util::Result<void> beginConnectionStateRequest(int timeoutMs = 1000);
    util::Result<util::OperationProgressState> pollConnectionStateRequest();
    util::Result<void> disconnectTunneling(int timeoutMs = 200);
    util::Result<void> beginDisconnectTunneling(int timeoutMs = 200);
    util::Result<util::OperationProgressState> pollDisconnectTunneling();
    util::Result<void> sendConnectionStateRequest(int timeoutMs = 1000);

    util::Result<void> sendCemi(std::span<const uint8_t> cemi, bool waitAck = true, int timeoutMs = 500);
    void setReceiveCallback(ReceiveCallback cb) { rxCb_ = std::move(cb); }
    util::Result<bool> poll(int timeoutMs = 0);
    util::Result<knx::netip::device_management::ConnectionHeader> acquireDeviceManagementConnectionHeader();
    DeviceManagementConnectionProvider deviceManagementConnectionProvider() noexcept
    {
        return DeviceManagementConnectionProvider::from<SecureTunnelingClient,
                                                       &SecureTunnelingClient::acquireDeviceManagementConnectionHeader>(*this);
    }

    bool isTunnelingConnected() const noexcept { return isOpen() && channelId_.isValid(); }
    SessionOperationType activeSessionOperation() const noexcept;
    ChannelId channelId() const noexcept { return channelId_; }
    TunnelingSequence sequence() const noexcept { return seq_; }

    // After open(), returns a ready-to-use SecureWrapper-based security context for
    // tunnelling (tag=0x0000, counterSuffix=0000ff00).
    knx::netip::NetIpSecurity* security() noexcept { return security_.get(); }

private:
    enum class OpenStage : uint8_t {
        Idle = 0,
        WaitingSessionResponse,
        WaitingSessionStatus,
    };

    struct OpenOperation {
        bool active{false};
        OpenStage stage{OpenStage::Idle};
        uint32_t startTimeMs{0};
        int timeoutMs{0};
        Options options{};
        SecureSession::PublicKey clientPublicKey{};
        SecureSessionBootstrap::NegotiatedSession negotiatedSession{};

        void reset() noexcept
        {
            active = false;
            stage = OpenStage::Idle;
            startTimeMs = 0;
            timeoutMs = 0;
            options = Options{};
            clientPublicKey = {};
            negotiatedSession = {};
        }
    };

    struct ConnectOperation {
        bool active{false};
        uint32_t startTimeMs{0};
        int timeoutMs{0};

        constexpr void reset() noexcept
        {
            active = false;
            startTimeMs = 0;
            timeoutMs = 0;
        }
    };

    struct ConnectionStateOperation {
        bool active{false};
        uint32_t startTimeMs{0};
        int timeoutMs{0};

        constexpr void reset() noexcept
        {
            active = false;
            startTimeMs = 0;
            timeoutMs = 0;
        }
    };

    struct DisconnectOperation {
        bool active{false};
        uint32_t startTimeMs{0};
        int timeoutMs{0};

        constexpr void reset() noexcept
        {
            active = false;
            startTimeMs = 0;
            timeoutMs = 0;
        }
    };

    util::Result<void> sendFrameUnlocked_(std::span<const uint8_t> frame);
    util::Result<size_t> receiveFrameUnlocked_(std::span<uint8_t> outFrame);
    util::Result<void> sendFrame(std::span<const uint8_t> frame);
    util::Result<size_t> receiveFrame(std::span<uint8_t> outFrame);

    util::Result<bool> receiveOnce_(int timeoutMs);
    util::Result<void> sendDisconnect_();
    util::Result<void> sendTunnelingRequest_(std::span<const uint8_t> cemi);
    uint32_t getCurrentTimeMs_() const;
    bool hasPendingControlOperation_() const noexcept;

    static constexpr size_t kMaxTcpFrameLen = knx::netip::config::kTcpBufferSize;

    std::unique_ptr<platform::TcpSocket> sock_;
    std::unique_ptr<SecureTunnelingSecurity> security_;
    std::unique_ptr<SecureTcpFrameChannel> secureChannel_;

    // Serializes all socket I/O between poll(), keepalive, and sendCemi() ACK waits.
    mutable std::mutex ioMutex_;

    TunnelingAckTracker ackTracker_{};
    OpenOperation openOperation_{};
    ConnectOperation connectOperation_{};
    ConnectionStateOperation connectionStateOperation_{};
    DisconnectOperation disconnectOperation_{};

    ChannelId channelId_{ChannelId::invalid()};
    TunnelingSequence seq_{TunnelingSequence(0)};
    uint32_t lastActivityTimeMs_{0};
    platform::TimingPlatform* timingPlatform_{nullptr};
    ReceiveCallback rxCb_{};
    DatagramBuffer<kMaxTcpFrameLen> frameBuffer_{};
    DatagramBuffer<kMaxTcpFrameLen> scratchBuffer_{};
    SecureDatagramBuffer<kMaxTcpFrameLen, SecureWrapper::kOverhead> secureBuffer_{};
};

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
