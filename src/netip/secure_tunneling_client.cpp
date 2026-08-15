// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_tunneling_client.hpp"
#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/netip/detail/polling.hpp"
#include "knx/netip/tunneling_control_builder.hpp"

#if KNX_SECURE_ENABLED

#include <mutex>
#include <span>

namespace knx {
namespace netip {
namespace ip_secure {

using ControlPacketCodec = control_packet::Codec;

static constexpr bool scalarAllZero(const knx::security::X25519::Scalar& s) noexcept
{
    for (auto b : s) {
        if (b != 0) return false;
    }
    return true;
}


SecureTunnelingSecurity::SecureTunnelingSecurity(
    const Key& sessionKey,
    SessionId sessionId,
    const std::array<uint8_t, SecureWrapper::kSerialLen>& localSerial,
    uint64_t initialSeq)
    : sessionKey_(sessionKey)
    , sessionId_(sessionId)
    , serial_(localSerial)
    , seq48_(initialSeq & 0xFFFFFFFFFFFFULL)
{
}

inline constexpr void SecureTunnelingSecurity::encodeSeq48BE_(uint64_t seq48, std::span<uint8_t, SecureWrapper::kSeqLen> out) noexcept
{
    seq48 &= 0xFFFFFFFFFFFFULL;
    out[0] = static_cast<uint8_t>((seq48 >> 40) & 0xFF);
    out[1] = static_cast<uint8_t>((seq48 >> 32) & 0xFF);
    out[2] = static_cast<uint8_t>((seq48 >> 24) & 0xFF);
    out[3] = static_cast<uint8_t>((seq48 >> 16) & 0xFF);
    out[4] = static_cast<uint8_t>((seq48 >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>(seq48 & 0xFF);
}

util::Result<size_t> SecureTunnelingSecurity::protect(std::span<const uint8_t> in, std::span<uint8_t> out)
{
    if (!sessionId_.isValid()) return util::ErrorCode::NotInitialized;

    const uint64_t seq = (seq48_++ & 0xFFFFFFFFFFFFULL);
    std::array<uint8_t, SecureWrapper::kSeqLen> seqBytes{};
    encodeSeq48BE_(seq, seqBytes);

    const std::array<uint8_t, SecureWrapper::kTagLen> tag{0x00, 0x00};
    const std::array<uint8_t, 4> counterSuffix{0x00, 0x00, 0xFF, 0x00};

    return SecureWrapper::wrap(sessionKey_, sessionId_, seqBytes, serial_, tag, counterSuffix, in, out);
}

util::Result<size_t> SecureTunnelingSecurity::unprotect(std::span<const uint8_t> in, std::span<uint8_t> out)
{
    if (!sessionId_.isValid()) return util::ErrorCode::NotInitialized;

    auto unwrapRes = SecureWrapper::unwrapAndVerify(sessionKey_, sessionId_, in, out);
    if (unwrapRes.isError()) return unwrapRes.error();

    return unwrapRes;
}

SecureTunnelingClient::SecureTunnelingClient() = default;
SecureTunnelingClient::~SecureTunnelingClient() { close(); }

bool SecureTunnelingClient::isOpen() const noexcept
{
    return sock_ && sock_->isOpen() && secureChannel_;
}

bool SecureTunnelingClient::hasPendingControlOperation_() const noexcept
{
    return connectOperation_.active ||
           connectionStateOperation_.active ||
           disconnectOperation_.active;
}

SecureTunnelingClient::SessionOperationType SecureTunnelingClient::activeSessionOperation() const noexcept
{
    if (openOperation_.active) return SessionOperationType::OpenSecureSession;
    if (connectOperation_.active) return SessionOperationType::ConnectTunneling;
    if (connectionStateOperation_.active) return SessionOperationType::ConnectionStateRequest;
    if (disconnectOperation_.active) return SessionOperationType::DisconnectTunneling;
    return SessionOperationType::None;
}

void SecureTunnelingClient::close()
{
    if (sock_) {
        (void)disconnectTunneling();
        sock_->close();
        sock_.reset();
    }
    secureChannel_.reset();
    security_.reset();
    openOperation_.reset();
    connectOperation_.reset();
    connectionStateOperation_.reset();
    disconnectOperation_.reset();
    TunnelingOrchestration::resetSession(channelId_, seq_);
    TunnelingOrchestration::clearActivity(lastActivityTimeMs_);
}

uint32_t SecureTunnelingClient::getCurrentTimeMs_() const
{
    return detail::nowMs(timingPlatform_);
}

util::Result<void> SecureTunnelingClient::sendFrame(std::span<const uint8_t> frame)
{
    std::lock_guard<std::mutex> lock(ioMutex_);
    return sendFrameUnlocked_(frame);
}

util::Result<void> SecureTunnelingClient::sendFrameUnlocked_(std::span<const uint8_t> frame)
{
    if (!secureChannel_) return util::ErrorCode::NotInitialized;
    return secureChannel_->sendFrame(frame);
}

util::Result<size_t> SecureTunnelingClient::receiveFrame(std::span<uint8_t> outFrame)
{
    std::lock_guard<std::mutex> lock(ioMutex_);
    return receiveFrameUnlocked_(outFrame);
}

util::Result<size_t> SecureTunnelingClient::receiveFrameUnlocked_(std::span<uint8_t> outFrame)
{
    if (!secureChannel_) return util::ErrorCode::NotInitialized;
    return secureChannel_->receiveFrame(outFrame);
}

util::Result<void> SecureTunnelingClient::beginConnectTunneling(int timeoutMs)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;
    if (!security_) return util::ErrorCode::NotInitialized;
    if (channelId_.isValid()) return util::Result<void>::ok();
    if (hasPendingControlOperation_()) return util::ErrorCode::Busy;

    const auto controlEndpoint = TunnelingControlBuilder::makeTcpControlEndpoint(
        sock_->localAddress().isZero() ? IpAddress(0) : sock_->localAddress(),
        sock_->localPort());

    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = TunnelingControlBuilder::encodeConnectionRequest(packet, controlEndpoint);
    if (encodeResult.isError()) return encodeResult.error();

    auto sendResult = sendFrame(packet.span());
    if (sendResult.isError()) return sendResult.error();

    connectOperation_.active = true;
    connectOperation_.startTimeMs = getCurrentTimeMs_();
    connectOperation_.timeoutMs = timeoutMs;
    return util::Result<void>::ok();
}

util::Result<util::OperationProgressState> SecureTunnelingClient::pollConnectTunneling()
{
    if (!connectOperation_.active) {
        if (isTunnelingConnected()) {
            return util::OperationProgressState::Success;
        }
        return util::ErrorCode::OperationNotReady;
    }

    auto waitRes = secureChannel_->waitReadable(0, timingPlatform_);
    if (waitRes.isError()) {
        connectOperation_.reset();
        return waitRes.error();
    }
    if (!waitRes.value()) {
        if (detail::remainingTimeoutMs(timingPlatform_, connectOperation_.startTimeMs, connectOperation_.timeoutMs) <= 0) {
            connectOperation_.reset();
            return util::OperationProgressState::Timeout;
        }
        return util::OperationProgressState::Pending;
    }

    auto recvRes = receiveFrame(frameBuffer_.span());
    if (recvRes.isError()) {
        connectOperation_.reset();
        return recvRes.error();
    }

    auto channelResult = ControlPacketCodec::decodeChannelStatusResponse(
        std::span<const uint8_t>(frameBuffer_.span()).first(recvRes.value()),
        control_packet::kServiceConnectionResponse);
    if (channelResult.isError()) {
        connectOperation_.reset();
        return channelResult.error();
    }
    if (channelResult.value().status != 0x00 || channelResult.value().channelId == 0x00) {
        connectOperation_.reset();
        return util::ErrorCode::OperationFailed;
    }

    TunnelingOrchestration::markConnected(channelId_, seq_, lastActivityTimeMs_, ChannelId(channelResult.value().channelId), [this]() {
        return getCurrentTimeMs_();
    });
    connectOperation_.reset();
    return util::OperationProgressState::Success;
}

util::Result<void> SecureTunnelingClient::connectTunneling(int timeoutMs)
{
    auto beginResult = beginConnectTunneling(timeoutMs);
    if (beginResult.isError()) return beginResult.error();

    auto terminal = detail::waitForTerminalProgress(timingPlatform_, [this]() { return pollConnectTunneling(); });
    if (terminal.isError()) return terminal.error();
    return detail::completionResult(terminal.value());
}

util::Result<void> SecureTunnelingClient::beginConnectionStateRequest(int timeoutMs)
{
    if (!isTunnelingConnected()) return util::ErrorCode::NotInitialized;
    if (hasPendingControlOperation_()) return util::ErrorCode::Busy;

    const auto controlEndpoint = TunnelingControlBuilder::makeTcpControlEndpoint(
        sock_->localAddress().isZero() ? IpAddress(0) : sock_->localAddress(),
        sock_->localPort());

    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = TunnelingControlBuilder::encodeConnectionStateRequest(packet, channelId_.value(), controlEndpoint);
    if (encodeResult.isError()) return encodeResult.error();

    auto sendResult = sendFrame(packet.span());
    if (sendResult.isError()) return sendResult.error();

    connectionStateOperation_.active = true;
    connectionStateOperation_.startTimeMs = getCurrentTimeMs_();
    connectionStateOperation_.timeoutMs = timeoutMs;

    return util::Result<void>::ok();
}

util::Result<util::OperationProgressState> SecureTunnelingClient::pollConnectionStateRequest()
{
    if (!connectionStateOperation_.active) {
        return util::ErrorCode::OperationNotReady;
    }

    auto waitRes = secureChannel_->waitReadable(0, timingPlatform_);
    if (waitRes.isError()) {
        connectionStateOperation_.reset();
        return waitRes.error();
    }
    if (!waitRes.value()) {
        if (detail::remainingTimeoutMs(timingPlatform_, connectionStateOperation_.startTimeMs, connectionStateOperation_.timeoutMs) <= 0) {
            connectionStateOperation_.reset();
            return util::OperationProgressState::Timeout;
        }
        return util::OperationProgressState::Pending;
    }

    auto recvRes = receiveFrame(frameBuffer_.span());
    if (recvRes.isError()) {
        connectionStateOperation_.reset();
        return recvRes.error();
    }

    auto channelResult = ControlPacketCodec::decodeChannelStatusResponse(
        std::span<const uint8_t>(frameBuffer_.span()).first(recvRes.value()),
        control_packet::kServiceConnectionStateResponse);
    if (channelResult.isError()) {
        connectionStateOperation_.reset();
        return channelResult.error();
    }
    if (channelResult.value().status != 0x00 || channelResult.value().channelId != channelId_.value()) {
        connectionStateOperation_.reset();
        return util::ErrorCode::OperationFailed;
    }

    TunnelingOrchestration::markActivity(lastActivityTimeMs_, [this]() { return getCurrentTimeMs_(); });
    connectionStateOperation_.reset();
    return util::OperationProgressState::Success;
}

util::Result<void> SecureTunnelingClient::sendConnectionStateRequest(int timeoutMs)
{
    auto beginResult = beginConnectionStateRequest(timeoutMs);
    if (beginResult.isError()) return beginResult.error();

    auto terminal = detail::waitForTerminalProgress(timingPlatform_, [this]() { return pollConnectionStateRequest(); });
    if (terminal.isError()) return terminal.error();
    return detail::completionResult(terminal.value());
}

util::Result<void> SecureTunnelingClient::sendDisconnect_()
{
    if (!isTunnelingConnected()) return util::ErrorCode::NotInitialized;

    const auto controlEndpoint = TunnelingControlBuilder::makeTcpControlEndpoint(
        sock_->localAddress().isZero() ? IpAddress(0) : sock_->localAddress(),
        sock_->localPort());

    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = TunnelingControlBuilder::encodeDisconnectRequest(packet, channelId_.value(), controlEndpoint);
    if (encodeResult.isError()) return encodeResult.error();

    return sendFrame(packet.span());
}

util::Result<void> SecureTunnelingClient::beginDisconnectTunneling(int timeoutMs)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;
    if (!channelId_.isValid()) return util::Result<void>::ok();
    if (hasPendingControlOperation_()) return util::ErrorCode::Busy;

    auto sendResult = sendDisconnect_();
    if (sendResult.isError()) return sendResult.error();

    if (timeoutMs <= 0) {
        TunnelingOrchestration::resetSession(channelId_, seq_);
        return util::Result<void>::ok();
    }

    disconnectOperation_.active = true;
    disconnectOperation_.startTimeMs = getCurrentTimeMs_();
    disconnectOperation_.timeoutMs = timeoutMs;

    return util::Result<void>::ok();
}

util::Result<util::OperationProgressState> SecureTunnelingClient::pollDisconnectTunneling()
{
    if (!disconnectOperation_.active) {
        if (!channelId_.isValid()) {
            return util::OperationProgressState::Success;
        }
        return util::ErrorCode::OperationNotReady;
    }

    auto waitRes = secureChannel_->waitReadable(0, timingPlatform_);
    if (waitRes.isError()) {
        disconnectOperation_.reset();
        TunnelingOrchestration::resetSession(channelId_, seq_);
        return waitRes.error();
    }
    if (!waitRes.value()) {
        if (detail::remainingTimeoutMs(timingPlatform_, disconnectOperation_.startTimeMs, disconnectOperation_.timeoutMs) <= 0) {
            disconnectOperation_.reset();
            TunnelingOrchestration::resetSession(channelId_, seq_);
            return util::OperationProgressState::Timeout;
        }
        return util::OperationProgressState::Pending;
    }

    auto recvRes = receiveFrame(frameBuffer_.span());
    if (recvRes.isError()) {
        disconnectOperation_.reset();
        TunnelingOrchestration::resetSession(channelId_, seq_);
        return recvRes.error();
    }

    const auto expectedChannelId = channelId_;
    auto channelResult = ControlPacketCodec::decodeChannelStatusResponse(
        std::span<const uint8_t>(frameBuffer_.span()).first(recvRes.value()),
        control_packet::kServiceDisconnectResponse);
    disconnectOperation_.reset();
    if (channelResult.isError()) {
        TunnelingOrchestration::resetSession(channelId_, seq_);
        return channelResult.error();
    }
    if (channelResult.value().status != 0x00 || channelResult.value().channelId != expectedChannelId.value()) {
        TunnelingOrchestration::resetSession(channelId_, seq_);
        return util::ErrorCode::OperationFailed;
    }

    TunnelingOrchestration::resetSession(channelId_, seq_);
    return util::OperationProgressState::Success;
}

util::Result<void> SecureTunnelingClient::disconnectTunneling(int timeoutMs)
{
    auto beginResult = beginDisconnectTunneling(timeoutMs);
    if (beginResult.isError()) return beginResult.error();
    if (timeoutMs <= 0 || !disconnectOperation_.active) return util::Result<void>::ok();

    auto terminal = detail::waitForTerminalProgress(timingPlatform_, [this]() { return pollDisconnectTunneling(); });
    if (terminal.isError()) return terminal.error();
    if (terminal.value() == util::OperationProgressState::Timeout) {
        return util::Result<void>::ok();
    }
    return detail::completionResult(terminal.value());
}

util::Result<void> SecureTunnelingClient::sendTunnelingRequest_(std::span<const uint8_t> cemi)
{
    if (!isTunnelingConnected()) return util::ErrorCode::NotInitialized;

    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = ControlPacketCodec::encodeTunnelingRequest(packet, channelId_.value(), seq_.value(), cemi);
    if (encodeResult.isError()) return encodeResult.error();

    auto sendRes = sendFrame(packet.span());
    if (sendRes.isError()) return sendRes.error();
    seq_ = TunnelingSequence(static_cast<uint8_t>((seq_.value() + 1) & 0xFF));
    TunnelingOrchestration::markActivity(lastActivityTimeMs_, [this]() { return getCurrentTimeMs_(); });
    return util::Result<void>::ok();
}

util::Result<bool> SecureTunnelingClient::receiveOnce_(int timeoutMs)
{
    if (!isTunnelingConnected()) return util::ErrorCode::NotInitialized;
    auto waitRes = secureChannel_->waitReadable(timeoutMs, timingPlatform_);
    if (waitRes.isError()) return waitRes.error();
    if (!waitRes.value()) return false;

    std::unique_lock<std::mutex> ioLock(ioMutex_);
    auto recvRes = receiveFrameUnlocked_(frameBuffer_.span());
    if (recvRes.isError()) return recvRes.error();
    const size_t inSize = recvRes.value();
    KnxNetIpHeader header;
    auto headerResult = KnxNetIpCodec::decodeHeader(frameBuffer_.span().first(inSize), header);
    if (headerResult.isError()) return headerResult.error();
    if (TunnelingOrchestration::isActivityServiceType(header.serviceType)) {
        TunnelingOrchestration::markActivity(lastActivityTimeMs_, [this]() { return getCurrentTimeMs_(); });
    }

    return TunnelingOrchestration::handleInboundFrame(
        frameBuffer_.span().first(inSize),
        header.serviceType,
        channelId_,
        ackTracker_,
        [&](uint8_t sequence) -> util::Result<void> {
            PacketWriter ackPacket(scratchBuffer_.span());
            auto ackHeaderResult = ControlPacketCodec::encodeTunnelingAck(
                ackPacket,
                channelId_.value(),
                sequence,
                0x00);
            if (ackHeaderResult.isOk()) {
                (void)sendFrameUnlocked_(ackPacket.span());
            }
            return util::Result<void>::ok();
        },
        [&](std::span<const uint8_t> cemi) {
            auto cb = rxCb_;
            ioLock.unlock();
            if (cb) cb(cemi);
        });
}

util::Result<device_management::ConnectionHeader> SecureTunnelingClient::acquireDeviceManagementConnectionHeader()
{
    if (!isTunnelingConnected()) return util::ErrorCode::NotInitialized;

    const auto header = device_management::ConnectionHeader{channelId_, seq_.value()};
    seq_ = TunnelingSequence(static_cast<uint8_t>((seq_.value() + 1) & 0xFF));
    return header;
}

util::Result<bool> SecureTunnelingClient::poll(int timeoutMs)
{
    if (!isTunnelingConnected()) return util::ErrorCode::NotInitialized;
    return receiveOnce_(timeoutMs);
}

util::Result<void> SecureTunnelingClient::sendCemi(std::span<const uint8_t> cemi, bool waitAck, int timeoutMs)
{
    if (!isTunnelingConnected()) return util::ErrorCode::NotInitialized;
    const uint8_t expectedAckSeq = seq_.value();
    auto sendRes = sendTunnelingRequest_(cemi);
    if (sendRes.isError()) return sendRes.error();
    if (!waitAck) return util::Result<void>::ok();

    return TunnelingOrchestration::waitForAck(
        ackTracker_,
        expectedAckSeq,
        timeoutMs,
        [this](int remainingMs) { return receiveOnce_(remainingMs); },
        [this]() { return getCurrentTimeMs_(); });
}

util::Result<void> SecureTunnelingClient::beginOpen(platform::NetworkInterface& network,
                                                    const Options& options,
                                                    int timeoutMs)
{
    close();

    if (options.host.isZero()) return util::ErrorCode::InvalidParameter;
    if (!options.port.isValid()) return util::ErrorCode::InvalidParameter;
    if (options.passwordLatin1.empty()) return util::ErrorCode::InvalidParameter;
    if (scalarAllZero(options.clientPrivateKey)) return util::ErrorCode::InvalidParameter;

    const IpAddress addr = options.host;

    auto sock = network.createTcpSocket();
    if (!sock) return util::ErrorCode::ResourceUnavailable;
    auto connectRes = sock->connect(addr, options.port.value());
    if (connectRes.isError()) return connectRes.error();

    sock_ = std::move(sock);

    SecureSession::PublicKey clientPub{};
    auto clientPubResult = SecureSessionBootstrap::deriveClientPublicKey(options.clientPrivateKey, clientPub);
    if (clientPubResult.isError()) {
        close();
        return clientPubResult.error();
    }

    std::array<uint8_t, SecureSession::kSessionRequestFrameLen> req{};
    auto reqResult = SecureSession::encodeSessionRequest(clientPub, req);
    if (reqResult.isError()) {
        close();
        return util::ErrorCode::EncodeFailed;
    }
    auto writeReq = TcpFrameChannel::sendFrame(*sock_, req);
    if (writeReq.isError()) {
        close();
        return writeReq.error();
    }


        openOperation_.active = true;
        openOperation_.stage = OpenStage::WaitingSessionResponse;
        openOperation_.startTimeMs = getCurrentTimeMs_();
        openOperation_.timeoutMs = timeoutMs;
        openOperation_.options = options;
        openOperation_.clientPublicKey = clientPub;
        openOperation_.negotiatedSession = {};

        return util::Result<void>::ok();
    }

    util::Result<util::OperationProgressState> SecureTunnelingClient::pollOpen()
    {
        if (!openOperation_.active) {
            if (isOpen()) {
                return util::OperationProgressState::Success;
            }
            return util::ErrorCode::OperationNotReady;
        }

        auto waitRes = TcpFrameChannel::waitReadable(*sock_, 0, timingPlatform_);
        if (waitRes.isError()) {
            close();
            return waitRes.error();
        }
        if (!waitRes.value()) {
            if (detail::remainingTimeoutMs(timingPlatform_, openOperation_.startTimeMs, openOperation_.timeoutMs) <= 0) {
                close();
                return util::OperationProgressState::Timeout;
            }
            return util::OperationProgressState::Pending;
        }

        if (openOperation_.stage == OpenStage::WaitingSessionResponse) {
            auto readResp = TcpFrameChannel::receiveFrame(*sock_, frameBuffer_.span());
            if (readResp.isError()) {
                close();
                return readResp.error();
            }

            const size_t respSize = readResp.value();
            auto authRequestResult = SecureSessionBootstrap::buildAuthenticateRequest(
                openOperation_.options.clientPrivateKey,
                openOperation_.options.passwordLatin1,
                openOperation_.options.userId,
                openOperation_.options.clientSerial,
                openOperation_.options.initialSeq,
                openOperation_.clientPublicKey,
                frameBuffer_.span().first(respSize),
                secureBuffer_.span());
            if (authRequestResult.isError()) {
                close();
                return authRequestResult.error();
            }

            openOperation_.negotiatedSession = authRequestResult.value().session;
            auto writeAuth = TcpFrameChannel::sendFrame(
                *sock_,
                secureBuffer_.span().first(authRequestResult.value().wrappedFrameLength));
            if (writeAuth.isError()) {
                close();
                return writeAuth.error();
            }

            openOperation_.stage = OpenStage::WaitingSessionStatus;
            return util::OperationProgressState::Pending;
        }

        auto readStatus = TcpFrameChannel::receiveFrame(*sock_, frameBuffer_.span());
        if (readStatus.isError()) {
            close();
            return readStatus.error();
        }
        const size_t statusSize = readStatus.value();

        auto statusResult = SecureSessionBootstrap::decodeSessionStatus(
            openOperation_.negotiatedSession.wrapperKey,
            openOperation_.negotiatedSession.sessionId,
            frameBuffer_.span().first(statusSize),
            scratchBuffer_.span());
        if (statusResult.isError()) {
            close();
            return statusResult.error();
        }
        if (statusResult.value() != 0x00) {
            close();
            return util::ErrorCode::OperationFailed;
        }

        security_ = std::make_unique<SecureTunnelingSecurity>(openOperation_.negotiatedSession.wrapperKey,
                                                              openOperation_.negotiatedSession.sessionId,
                                                              openOperation_.options.clientSerial,
                                                              openOperation_.negotiatedSession.nextSequence);
        secureChannel_ = std::make_unique<SecureTcpFrameChannel>(*sock_,
                                                                 *security_,
                                                                 secureBuffer_.span(),
                                                                 scratchBuffer_.span());

        TunnelingOrchestration::resetSession(channelId_, seq_);
        TunnelingOrchestration::markActivity(lastActivityTimeMs_, [this]() { return getCurrentTimeMs_(); });
        openOperation_.reset();
        return util::OperationProgressState::Success;
    }

    util::Result<void> SecureTunnelingClient::open(platform::NetworkInterface& network, const Options& options)
    {
        auto beginResult = beginOpen(network, options, 1000);
        if (beginResult.isError()) return beginResult.error();

        auto terminal = detail::waitForTerminalProgress(timingPlatform_, [this]() { return pollOpen(); });
        if (terminal.isError()) return terminal.error();
        return detail::completionResult(terminal.value());
}

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
