// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/tunneling_session_client.hpp"

#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/detail/polling.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/netip/secure_udp_datagram_channel.hpp"
#include "knx/netip/tunneling_control_builder.hpp"
#include "knx/netip/udp_datagram_channel.hpp"

#include "knx/util/log.hpp"

static const char* TAG_TUN = "KNX.NETIP.Tunnel";

namespace knx {
namespace netip {

using ControlPacketCodec = control_packet::Codec;

TunnelingSessionClient::TunnelingSessionClient()
    : keepalive_([this]() {
        if (isOpen()) {
            (void)sendConnectionStateRequest(1000);
        }
    })
{
}

TunnelingSessionClient::~TunnelingSessionClient()
{
    stopKeepalive();
    close();
}

util::Result<void> TunnelingSessionClient::beginOpen(platform::NetworkInterface& network,
                                                     IpAddress host,
                                                     NetIpPort port,
                                                     int timeoutMs)
{
    close();
    if (host.isZero() || !port.isValid()) return util::ErrorCode::InvalidParameter;

    auto sock = network.createUdpSocket();
    if (!sock) return util::ErrorCode::ResourceUnavailable;
    auto openRes = sock->open(0);
    if (openRes.isError()) return openRes.error();

    sock_ = std::move(sock);
    remote_.addr = host;
    remote_.port = port;
    localAddr_ = network.ipAddress();

    auto connResult = sendConnectionRequest(host, port);
    if (connResult.isError()) {
        close();
        return connResult.error();
    }

    openOperation_.active = true;
    openOperation_.startTimeMs = detail::nowMs(timingPlatform_);
    openOperation_.timeoutMs = timeoutMs;
    return util::Result<void>::ok();
}

util::Result<util::OperationProgressState> TunnelingSessionClient::pollOpen()
{
    if (!openOperation_.active) {
        if (isOpen()) {
            return util::OperationProgressState::Success;
        }
        return util::ErrorCode::OperationNotReady;
    }

    if (isOpen()) {
        openOperation_.reset();
        return util::OperationProgressState::Success;
    }

    auto responseResult = receiveConnectionResponseOnce();
    if (responseResult.isError()) {
        close();
        return responseResult.error();
    }
    if (responseResult.value()) {
        openOperation_.reset();
        return util::OperationProgressState::Success;
    }

    if (detail::remainingTimeoutMs(timingPlatform_, openOperation_.startTimeMs, openOperation_.timeoutMs) <= 0) {
        close();
        return util::OperationProgressState::Timeout;
    }

    return util::OperationProgressState::Pending;
}

util::Result<void> TunnelingSessionClient::open(platform::NetworkInterface& network, IpAddress host, NetIpPort port, int timeoutMs)
{
    auto beginResult = beginOpen(network, host, port, timeoutMs);
    if (beginResult.isError()) {
        uint8_t ha = 0, hb = 0, hc = 0, hd = 0;
        host.toOctets(ha, hb, hc, hd);
        KNX_LOGE(TAG_TUN, "ConnectionRequest failed to %u.%u.%u.%u:%u",
                 static_cast<unsigned>(ha), static_cast<unsigned>(hb),
                 static_cast<unsigned>(hc), static_cast<unsigned>(hd),
                 static_cast<unsigned>(port.value()));
        return beginResult.error();
    }

    auto terminal = detail::waitForTerminalProgress(timingPlatform_, [this]() { return pollOpen(); });
    if (terminal.isError()) {
        return terminal.error();
    }

    return detail::completionResult(terminal.value());
}

void TunnelingSessionClient::close()
{
    stopKeepalive();
    if (sock_) {
        (void)sendDisconnect();
        sock_->close();
        sock_.reset();
    }
    TunnelingOrchestration::resetSession(channelId_, seq_);
    TunnelingOrchestration::clearActivity(lastActivityTimeMs_);
    openOperation_.reset();
    remote_ = RemoteInfo{};
    localAddr_ = IpAddress(0);
}

bool TunnelingSessionClient::isOpen() const noexcept
{
    return sock_ && sock_->isOpen() && channelId_.isValid();
}

TunnelingSessionClient::SessionOperationType TunnelingSessionClient::activeSessionOperation() const noexcept
{
    return openOperation_.active ? SessionOperationType::Open : SessionOperationType::None;
}

ChannelId TunnelingSessionClient::channelId() const noexcept
{
    return channelId_;
}

TunnelingSequence TunnelingSessionClient::sequence() const noexcept
{
    return seq_;
}

uint32_t TunnelingSessionClient::getCurrentTimeMs() const noexcept
{
    return detail::nowMs(timingPlatform_);
}

uint32_t TunnelingSessionClient::getTimeSinceLastActivity() const
{
    if (lastActivityTimeMs_ == 0) return 0;
    return getCurrentTimeMs() - lastActivityTimeMs_;
}

void TunnelingSessionClient::setReceiveCallback(ReceiveCallback cb)
{
    rxCb_ = std::move(cb);
}

util::Result<bool> TunnelingSessionClient::poll(int timeoutMs)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;
    return receiveOnce(timeoutMs);
}

util::Result<device_management::ConnectionHeader> TunnelingSessionClient::acquireDeviceManagementConnectionHeader()
{
    if (!isOpen()) return util::ErrorCode::NotInitialized;

    const auto header = device_management::ConnectionHeader{channelId_, seq_.value()};
    seq_ = TunnelingSequence(static_cast<uint8_t>((seq_.value() + 1) & 0xFF));
    return header;
}

util::Result<void> TunnelingSessionClient::sendConnectionRequest(IpAddress host, NetIpPort port)
{
    uint8_t ha = 0, hb = 0, hc = 0, hd = 0;
    host.toOctets(ha, hb, hc, hd);

    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;

    uint8_t a = 0, b = 0, c = 0, d = 0;
    const auto controlEndpoint = TunnelingControlBuilder::makeUdpControlEndpoint(localAddr_, remote_.addr, sock_->localPort());
    const IpAddress advertisedAddr = controlEndpoint.address;
    advertisedAddr.toOctets(a, b, c, d);
    const uint16_t localPort = controlEndpoint.port;

    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = TunnelingControlBuilder::encodeConnectionRequest(packet, controlEndpoint);
    if (encodeResult.isError()) return encodeResult.error();

    KNX_LOGD(TAG_TUN, "Sent CONN_REQ to %u.%u.%u.%u:%u local=%u.%u.%u.%u:%u bytes=%d",
             static_cast<unsigned>(ha), static_cast<unsigned>(hb),
             static_cast<unsigned>(hc), static_cast<unsigned>(hd),
             static_cast<unsigned>(port.value()),
             static_cast<unsigned>(a), static_cast<unsigned>(b), static_cast<unsigned>(c), static_cast<unsigned>(d),
             static_cast<unsigned>(localPort), static_cast<int>(packet.size()));

    std::unique_lock<std::mutex> ioLock(ioMutex_);
    const int sent = sock_->send(remote_.addr, remote_.port.value(), packet.span());
    if (sent != static_cast<int>(packet.size())) return util::ErrorCode::TransmissionFailed;
    return util::Result<void>::ok();
}

util::Result<bool> TunnelingSessionClient::receiveConnectionResponseOnce()
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;
    if (sock_->available() == 0) return false;

    std::unique_lock<std::mutex> ioLock(ioMutex_);
    IpAddress srcAddr(0);
    uint16_t srcPort = 0;
    const int received = sock_->receive(scratchBuffer_.span(), srcAddr, srcPort);
    if (received <= 0) {
        return false;
    }
    if (received < 6) return util::ErrorCode::InvalidFrameSize;

    const auto frame = std::span<const uint8_t>(scratchBuffer_.span().data(), static_cast<size_t>(received));
    KnxNetIpHeader header;
    auto headerResult = KnxNetIpCodec::decodeHeader(frame, header);
    if (headerResult.isError()) return headerResult.error();
    if (header.serviceType != control_packet::kServiceConnectionResponse) {
        return false;
    }

    auto channelResult = ControlPacketCodec::decodeChannelStatusResponse(frame, control_packet::kServiceConnectionResponse);
    if (channelResult.isError()) return channelResult.error();

    if (channelResult.value().status != 0x00 || channelResult.value().channelId == 0x00) {
        return util::ErrorCode::OperationFailed;
    }

    TunnelingOrchestration::markConnected(channelId_, seq_, lastActivityTimeMs_, ChannelId(channelResult.value().channelId), [this]() {
        return getCurrentTimeMs();
    });
    KNX_LOGI(TAG_TUN, "Tunneling connected: channel=%u seq=%u", static_cast<unsigned>(channelId_.value()), static_cast<unsigned>(seq_.value()));
    return true;
}

util::Result<void> TunnelingSessionClient::sendDisconnect()
{
    if (!sock_ || !sock_->isOpen() || !channelId_.isValid()) return util::ErrorCode::NotInitialized;

    uint8_t a = 0, b = 0, c = 0, d = 0;
    const auto controlEndpoint = TunnelingControlBuilder::makeUdpControlEndpoint(localAddr_, remote_.addr, sock_->localPort());
    const IpAddress advertisedAddr = controlEndpoint.address;
    advertisedAddr.toOctets(a, b, c, d);
    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = TunnelingControlBuilder::encodeDisconnectRequest(packet, channelId_.value(), controlEndpoint);
    if (encodeResult.isError()) return encodeResult.error();

    if (remote_.addr.isZero() || !remote_.port.isValid()) return util::ErrorCode::InvalidAddress;

    std::unique_lock<std::mutex> ioLock(ioMutex_);
    size_t responseSize = 0;
    auto verifyResult = TunnelingOrchestration::verifyChannelStatus(
        control_packet::kServiceDisconnectResponse,
        channelId_,
        [&]() -> util::Result<void> {
            auto exchangeResult = UdpDatagramChannel::exchange(*sock_,
                                                               UdpDatagramEndpoint{remote_.addr, remote_.port},
                                                               packet.span(),
                                                               scratchBuffer_.span(),
                                                               200,
                                                               timingPlatform_);
            if (exchangeResult.isError()) return exchangeResult.error();
            responseSize = exchangeResult.value();
            return util::Result<void>::ok();
        },
        [&]() -> util::Result<std::span<const uint8_t>> {
            return std::span<const uint8_t>(scratchBuffer_.span().data(), responseSize);
        });
    if (verifyResult.isOk()) {
        TunnelingOrchestration::markActivity(lastActivityTimeMs_, [this]() { return getCurrentTimeMs(); });
    }
    return util::Result<void>::ok();
}

util::Result<void> TunnelingSessionClient::sendTunnelingRequest(std::span<const uint8_t> cemi)
{
    if (!sock_ || !sock_->isOpen() || !channelId_.isValid()) return util::ErrorCode::NotInitialized;

    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = ControlPacketCodec::encodeTunnelingRequest(packet, channelId_.value(), seq_.value(), cemi);
    if (encodeResult.isError()) return encodeResult.error();

    if (security_) {
        SecureUdpDatagramChannel secureChannel(*sock_, *security_, secureBuffer_.span(), scratchBuffer_.span());
        auto sendResult = secureChannel.send(UdpDatagramEndpoint{remote_.addr, remote_.port}, packet.span());
        if (sendResult.isError()) return sendResult.error();
    } else {
        const int sent = sock_->send(remote_.addr, remote_.port.value(), packet.span());
        if (sent != static_cast<int>(packet.size())) return util::ErrorCode::TransmissionFailed;
    }

    seq_ = TunnelingSequence(static_cast<uint8_t>((seq_.value() + 1) & 0xFF));
    return util::Result<void>::ok();
}

util::Result<void> TunnelingSessionClient::sendCemi(std::span<const uint8_t> cemi, bool waitAck, int timeoutMs)
{
    if (!isOpen()) return util::ErrorCode::NotInitialized;
    auto sendResult = sendTunnelingRequest(cemi);
    if (sendResult.isError()) return sendResult.error();
    if (!waitAck) return util::Result<void>::ok();
    const uint8_t expectedSeq = static_cast<uint8_t>((seq_.value() - 1) & 0xFF);
    return TunnelingOrchestration::waitForAck(
        ackTracker_,
        expectedSeq,
        timeoutMs,
        [this](int remainingMs) { return receiveOnce(remainingMs); },
        [this]() { return getCurrentTimeMs(); });
}

util::Result<bool> TunnelingSessionClient::receiveOnce(int timeoutMs)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;

    const uint32_t startMs = detail::nowMs(timingPlatform_);
    std::unique_lock<std::mutex> ioLock(ioMutex_, std::defer_lock);
    IpAddress srcAddr(0);
    uint16_t srcPort = 0;
    int received = 0;
    while (true) {
        const int remainingMs = detail::remainingTimeoutMs(timingPlatform_, startMs, timeoutMs);
        auto waitRes = detail::waitUntilReadable(timingPlatform_, remainingMs, [this]() {
            return sock_ && sock_->available() > 0;
        });
        if (waitRes.isError()) return waitRes.error();
        if (!waitRes.value()) return false;

        ioLock.lock();
        if (!sock_ || !sock_->isOpen()) {
            return util::ErrorCode::NotInitialized;
        }
        if (sock_->available() == 0) {
            ioLock.unlock();
            if (timeoutMs == 0 || remainingMs == 0) {
                return false;
            }
            continue;
        }

        if (security_) {
            SecureUdpDatagramChannel secureChannel(*sock_, *security_, secureBuffer_.span(), scratchBuffer_.span());
            auto receiveResult = secureChannel.receive(frameBuffer_.span(), srcAddr, srcPort);
            if (receiveResult.isError()) {
                ioLock.unlock();
                if (remainingMs == 0) return false;
                continue;
            }
            received = static_cast<int>(receiveResult.value());
        } else {
            received = sock_->receive(frameBuffer_.span(), srcAddr, srcPort);
            if (received <= 0) {
                ioLock.unlock();
                if (remainingMs == 0) return false;
                continue;
            }
        }
        break;
    }
    if (received < 6) return util::ErrorCode::InvalidFrameSize;

    const uint8_t* inData = frameBuffer_.bytes.data();
    size_t inLen = static_cast<size_t>(received);

    if (inLen < 6) return util::ErrorCode::InvalidFrameSize;

    KnxNetIpHeader header;
    auto headerResult = KnxNetIpCodec::decodeHeader(std::span<const uint8_t>(inData, inLen), header);
    if (headerResult.isError()) return false;

    inLen = header.totalLength;

    if (TunnelingOrchestration::isActivityServiceType(header.serviceType)) {
        TunnelingOrchestration::markActivity(lastActivityTimeMs_, [this]() { return getCurrentTimeMs(); });
    }

    return TunnelingOrchestration::handleInboundFrame(
        std::span<const uint8_t>(inData, inLen),
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
            if (ackHeaderResult.isError()) return ackHeaderResult.error();

            if (security_) {
                SecureUdpDatagramChannel secureChannel(*sock_, *security_, secureBuffer_.span(), frameBuffer_.span());
                auto sendResult = secureChannel.send(UdpDatagramEndpoint{srcAddr, NetIpPort(srcPort)}, ackPacket.span());
                if (sendResult.isError()) return sendResult.error();
            } else {
                (void)sock_->send(srcAddr, srcPort, ackPacket.span());
            }
            return util::Result<void>::ok();
        },
        [&](std::span<const uint8_t> cemi) {
            auto cb = rxCb_;
            ioLock.unlock();
            if (cb) cb(cemi);
        });
}

util::Result<void> TunnelingSessionClient::sendConnectionStateRequest(int timeoutMs)
{
    if (!sock_ || !sock_->isOpen() || !channelId_.isValid()) return util::ErrorCode::NotInitialized;

    uint8_t a = 0, b = 0, c = 0, d = 0;
    const auto controlEndpoint = TunnelingControlBuilder::makeUdpControlEndpoint(localAddr_, remote_.addr, sock_->localPort());
    const IpAddress advertisedAddr = controlEndpoint.address;
    advertisedAddr.toOctets(a, b, c, d);
    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = TunnelingControlBuilder::encodeConnectionStateRequest(packet, channelId_.value(), controlEndpoint);
    if (encodeResult.isError()) return encodeResult.error();

    std::unique_lock<std::mutex> ioLock(ioMutex_);
    size_t responseSize = 0;
    auto verifyResult = TunnelingOrchestration::verifyChannelStatus(
        control_packet::kServiceConnectionStateResponse,
        channelId_,
        [&]() -> util::Result<void> {
            auto exchangeResult = UdpDatagramChannel::exchange(*sock_,
                                                               UdpDatagramEndpoint{remote_.addr, remote_.port},
                                                               packet.span(),
                                                               scratchBuffer_.span(),
                                                               timeoutMs);
            if (exchangeResult.isError()) return exchangeResult.error();
            responseSize = exchangeResult.value();
            return util::Result<void>::ok();
        },
        [&]() -> util::Result<std::span<const uint8_t>> {
            return std::span<const uint8_t>(scratchBuffer_.span().data(), responseSize);
        });
    if (verifyResult.isOk()) {
        TunnelingOrchestration::markActivity(lastActivityTimeMs_, [this]() { return getCurrentTimeMs(); });
        return util::Result<void>::ok();
    }

    return verifyResult.error();
}

void TunnelingSessionClient::startKeepalive(uint32_t intervalMs)
{
    keepalive_.start(intervalMs);
}

void TunnelingSessionClient::stopKeepalive()
{
    keepalive_.stop();
}

bool TunnelingSessionClient::isKeepaliveActive() const
{
    return keepalive_.isActive();
}

} // namespace netip
} // namespace knx