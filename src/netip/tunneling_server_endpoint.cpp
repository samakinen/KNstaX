// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/tunneling_server_endpoint.hpp"

#include "knx/netip/detail/polling.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/util/log.hpp"

#include <array>

namespace knx {
namespace netip {

static const char* TAG = "KNX.NETIP.TunSrv";

TunnelingServerEndpoint::TunnelingServerEndpoint() = default;

TunnelingServerEndpoint::~TunnelingServerEndpoint()
{
    close();
}

util::Result<void> TunnelingServerEndpoint::open(platform::NetworkInterface& network, const Options& options)
{
    close();

    if (!options.port.isValid() || options.maxChannels == 0) {
        return util::ErrorCode::InvalidParameter;
    }

    options_ = options;
    channels_.assign(options_.maxChannels, ChannelSlot{});

    socket_ = network.createUdpSocket();
    if (!socket_) {
        return util::ErrorCode::ResourceUnavailable;
    }

    auto openResult = socket_->open(options_.port.value());
    if (openResult.isError()) {
        socket_.reset();
        return openResult.error();
    }

    KNX_LOGI(TAG, "Tunneling server open on UDP %u (maxChannels=%u)",
             static_cast<unsigned>(options_.port.value()),
             static_cast<unsigned>(options_.maxChannels));
    return util::Result<void>::ok();
}

void TunnelingServerEndpoint::close()
{
    if (socket_) {
        socket_->close();
        socket_.reset();
    }
    channels_.clear();
}

bool TunnelingServerEndpoint::isOpen() const
{
    return socket_ && socket_->isOpen();
}

void TunnelingServerEndpoint::setReceiveCallback(ReceiveCallback callback)
{
    receiveCallback_ = std::move(callback);
}

uint32_t TunnelingServerEndpoint::nowMs() const
{
    return detail::nowMs(timingPlatform_);
}

void TunnelingServerEndpoint::markChannelActivity(ChannelSlot& slot)
{
    slot.info.lastActivityMs = nowMs();
}

void TunnelingServerEndpoint::pruneIdleChannels()
{
    if (options_.channelIdleTimeoutMs == 0) {
        return;
    }

    const uint32_t now = nowMs();
    for (auto& slot : channels_) {
        if (!slot.active) {
            continue;
        }

        if ((now - slot.info.lastActivityMs) >= options_.channelIdleTimeoutMs) {
            KNX_LOGI(TAG, "Channel %u timed out", static_cast<unsigned>(slot.info.channelId.value()));
            slot = ChannelSlot{};
        }
    }
}

TunnelingServerEndpoint::ChannelSlot* TunnelingServerEndpoint::findChannelById(ChannelId channelId)
{
    for (auto& slot : channels_) {
        if (slot.active && slot.info.channelId == channelId) {
            return &slot;
        }
    }
    return nullptr;
}

const TunnelingServerEndpoint::ChannelSlot* TunnelingServerEndpoint::findChannelById(ChannelId channelId) const
{
    for (const auto& slot : channels_) {
        if (slot.active && slot.info.channelId == channelId) {
            return &slot;
        }
    }
    return nullptr;
}

TunnelingServerEndpoint::ChannelSlot* TunnelingServerEndpoint::findChannelByEndpoint(IpAddress address, NetIpPort port)
{
    for (auto& slot : channels_) {
        if (!slot.active) {
            continue;
        }
        if (slot.info.remoteAddress == address && slot.info.remotePort == port) {
            return &slot;
        }
    }
    return nullptr;
}

TunnelingServerEndpoint::ChannelSlot* TunnelingServerEndpoint::allocateChannel(IpAddress address, NetIpPort port)
{
    for (auto& slot : channels_) {
        if (slot.active) {
            continue;
        }

        slot.active = true;
        slot.info.channelId = ChannelId(static_cast<uint8_t>(&slot - channels_.data() + 1u));
        slot.info.remoteAddress = address;
        slot.info.remotePort = port;
        slot.info.expectedRxSequence = 0;
        slot.info.nextTxSequence = 0;
        slot.info.lastActivityMs = nowMs();

        return &slot;
    }

    return nullptr;
}

util::Result<void> TunnelingServerEndpoint::sendChannelStatusResponse(NetIpServiceType serviceType,
                                                                      IpAddress destination,
                                                                      NetIpPort destinationPort,
                                                                      uint8_t channelId,
                                                                      uint8_t status)
{
    if (!isOpen()) {
        return util::ErrorCode::NotInitialized;
    }

    PacketWriter packet(txBuffer_.span());
    auto resetResult = packet.reset(KnxNetIpCodec::kHeaderLen);
    if (resetResult.isError()) {
        return resetResult.error();
    }

    const std::array<uint8_t, 2> payload{channelId, status};
    auto payloadResult = packet.write(payload);
    if (payloadResult.isError()) {
        return payloadResult.error();
    }

    auto headerResult = KnxNetIpCodec::encodeHeader(
        serviceType,
        packet.size() - KnxNetIpCodec::kHeaderLen,
        std::span<uint8_t, KnxNetIpCodec::kHeaderLen>(packet.span().data(), KnxNetIpCodec::kHeaderLen));
    if (headerResult.isError()) {
        return headerResult.error();
    }

    const int sent = socket_->send(destination, destinationPort.value(), packet.span());
    if (sent != static_cast<int>(packet.size())) {
        return util::ErrorCode::TransmissionFailed;
    }

    return util::Result<void>::ok();
}

util::Result<void> TunnelingServerEndpoint::handleConnectionRequest(std::span<const uint8_t> frame,
                                                                    IpAddress sourceAddress,
                                                                    NetIpPort sourcePort)
{
    if (frame.size() < 26) {
        return util::ErrorCode::DecodeFailed;
    }

    ChannelSlot* slot = findChannelByEndpoint(sourceAddress, sourcePort);
    if (slot == nullptr) {
        slot = allocateChannel(sourceAddress, sourcePort);
    }

    if (slot == nullptr) {
        return sendChannelStatusResponse(control_packet::kServiceConnectionResponse,
                                         sourceAddress,
                                         sourcePort,
                                         0,
                                         kStatusNoMoreConnections);
    }

    markChannelActivity(*slot);

    KNX_LOGI(TAG, "CONNECT_REQUEST accepted channel=%u", static_cast<unsigned>(slot->info.channelId.value()));
    return sendChannelStatusResponse(control_packet::kServiceConnectionResponse,
                                     sourceAddress,
                                     sourcePort,
                                     slot->info.channelId.value(),
                                     kStatusNoError);
}

util::Result<void> TunnelingServerEndpoint::handleConnectionStateRequest(std::span<const uint8_t> frame,
                                                                         IpAddress sourceAddress,
                                                                         NetIpPort sourcePort)
{
    if (frame.size() < 16) {
        return util::ErrorCode::DecodeFailed;
    }

    const ChannelId channelId(frame[6]);
    auto* slot = findChannelById(channelId);
    if (slot == nullptr || slot->info.remoteAddress != sourceAddress || slot->info.remotePort != sourcePort) {
        return sendChannelStatusResponse(control_packet::kServiceConnectionStateResponse,
                                         sourceAddress,
                                         sourcePort,
                                         channelId.value(),
                                         kStatusInvalidConnection);
    }

    markChannelActivity(*slot);
    return sendChannelStatusResponse(control_packet::kServiceConnectionStateResponse,
                                     sourceAddress,
                                     sourcePort,
                                     channelId.value(),
                                     kStatusNoError);
}

util::Result<void> TunnelingServerEndpoint::handleDisconnectRequest(std::span<const uint8_t> frame,
                                                                    IpAddress sourceAddress,
                                                                    NetIpPort sourcePort)
{
    if (frame.size() < 16) {
        return util::ErrorCode::DecodeFailed;
    }

    const ChannelId channelId(frame[6]);
    auto* slot = findChannelById(channelId);
    if (slot == nullptr || slot->info.remoteAddress != sourceAddress || slot->info.remotePort != sourcePort) {
        return sendChannelStatusResponse(control_packet::kServiceDisconnectResponse,
                                         sourceAddress,
                                         sourcePort,
                                         channelId.value(),
                                         kStatusInvalidConnection);
    }

    KNX_LOGI(TAG, "DISCONNECT_REQUEST channel=%u", static_cast<unsigned>(channelId.value()));
    *slot = ChannelSlot{};

    return sendChannelStatusResponse(control_packet::kServiceDisconnectResponse,
                                     sourceAddress,
                                     sourcePort,
                                     channelId.value(),
                                     kStatusNoError);
}

util::Result<void> TunnelingServerEndpoint::handleTunnelingRequest(std::span<const uint8_t> frame,
                                                                   IpAddress sourceAddress,
                                                                   NetIpPort sourcePort)
{
    auto decodeResult = control_packet::Codec::decodeTunnelingRequest(frame);
    if (decodeResult.isError()) {
        return decodeResult.error();
    }

    const auto request = decodeResult.value();
    const ChannelId channelId(request.channelId);
    auto* slot = findChannelById(channelId);
    if (slot == nullptr || slot->info.remoteAddress != sourceAddress || slot->info.remotePort != sourcePort) {
        PacketWriter ack(txBuffer_.span());
        auto ackResult = control_packet::Codec::encodeTunnelingAck(ack, request.channelId, request.sequence, kStatusInvalidConnection);
        if (ackResult.isError()) {
            return ackResult.error();
        }
        const int sent = socket_->send(sourceAddress, sourcePort.value(), ack.span());
        if (sent != static_cast<int>(ack.size())) {
            return util::ErrorCode::TransmissionFailed;
        }
        return util::Result<void>::ok();
    }

    uint8_t ackStatus = kStatusNoError;
    if (request.sequence != slot->info.expectedRxSequence) {
        ackStatus = kStatusSequenceError;
    }

    PacketWriter ack(txBuffer_.span());
    auto ackResult = control_packet::Codec::encodeTunnelingAck(ack, request.channelId, request.sequence, ackStatus);
    if (ackResult.isError()) {
        return ackResult.error();
    }

    const int sent = socket_->send(sourceAddress, sourcePort.value(), ack.span());
    if (sent != static_cast<int>(ack.size())) {
        return util::ErrorCode::TransmissionFailed;
    }

    markChannelActivity(*slot);

    if (ackStatus != kStatusNoError) {
        KNX_LOGW(TAG,
                 "Sequence mismatch on channel=%u expected=%u received=%u",
                 static_cast<unsigned>(slot->info.channelId.value()),
                 static_cast<unsigned>(slot->info.expectedRxSequence),
                 static_cast<unsigned>(request.sequence));
        return util::Result<void>::ok();
    }

    slot->info.expectedRxSequence = static_cast<uint8_t>((request.sequence + 1u) & 0xFFu);

    if (receiveCallback_) {
        receiveCallback_(slot->info.channelId, request.cemi);
    }

    return util::Result<void>::ok();
}

util::Result<bool> TunnelingServerEndpoint::poll(int timeoutMs)
{
    if (!isOpen()) {
        return util::ErrorCode::NotInitialized;
    }

    pruneIdleChannels();

    auto waitResult = detail::waitUntilReadable(timingPlatform_, timeoutMs, [this]() {
        return socket_ && socket_->available() > 0;
    });
    if (waitResult.isError()) {
        return waitResult.error();
    }
    if (!waitResult.value()) {
        return false;
    }

    IpAddress sourceAddress(0);
    uint16_t sourcePortRaw = 0;
    const int received = socket_->receive(rxBuffer_.span(), sourceAddress, sourcePortRaw);
    if (received <= 0) {
        return util::ErrorCode::ResourceUnavailable;
    }

    const auto frame = std::span<const uint8_t>(rxBuffer_.span().data(), static_cast<size_t>(received));

    KnxNetIpHeader header{};
    auto headerResult = KnxNetIpCodec::decodeHeader(frame, header);
    if (headerResult.isError()) {
        return headerResult.error();
    }

    const NetIpPort sourcePort(sourcePortRaw);

    switch (header.serviceType.value()) {
        case control_packet::kServiceConnectionRequest.value():
            return handleConnectionRequest(frame, sourceAddress, sourcePort).isOk();
        case control_packet::kServiceConnectionStateRequest.value():
            return handleConnectionStateRequest(frame, sourceAddress, sourcePort).isOk();
        case control_packet::kServiceDisconnectRequest.value():
            return handleDisconnectRequest(frame, sourceAddress, sourcePort).isOk();
        case control_packet::kServiceTunnelingRequest.value():
            return handleTunnelingRequest(frame, sourceAddress, sourcePort).isOk();
        default:
            return true;
    }
}

util::Result<void> TunnelingServerEndpoint::sendCemi(ChannelId channelId, std::span<const uint8_t> cemi)
{
    if (!isOpen()) {
        return util::ErrorCode::NotInitialized;
    }

    auto* slot = findChannelById(channelId);
    if (slot == nullptr) {
        return util::ErrorCode::InvalidParameter;
    }

    PacketWriter packet(txBuffer_.span());
    auto encodeResult = control_packet::Codec::encodeTunnelingRequest(
        packet,
        channelId.value(),
        slot->info.nextTxSequence,
        cemi);
    if (encodeResult.isError()) {
        return encodeResult.error();
    }

    const int sent = socket_->send(slot->info.remoteAddress, slot->info.remotePort.value(), packet.span());
    if (sent != static_cast<int>(packet.size())) {
        return util::ErrorCode::TransmissionFailed;
    }

    slot->info.nextTxSequence = static_cast<uint8_t>((slot->info.nextTxSequence + 1u) & 0xFFu);
    markChannelActivity(*slot);
    return util::Result<void>::ok();
}

util::Result<size_t> TunnelingServerEndpoint::sendCemiToAll(std::span<const uint8_t> cemi)
{
    size_t delivered = 0;
    for (const auto& slot : channels_) {
        if (!slot.active) {
            continue;
        }
        auto sendResult = sendCemi(slot.info.channelId, cemi);
        if (sendResult.isOk()) {
            ++delivered;
        }
    }
    return delivered;
}

size_t TunnelingServerEndpoint::activeChannelCount() const
{
    size_t active = 0;
    for (const auto& slot : channels_) {
        if (slot.active) {
            ++active;
        }
    }
    return active;
}

std::vector<TunnelingServerEndpoint::ChannelInfo> TunnelingServerEndpoint::activeChannels() const
{
    std::vector<ChannelInfo> out;
    out.reserve(channels_.size());
    for (const auto& slot : channels_) {
        if (slot.active) {
            out.push_back(slot.info);
        }
    }
    return out;
}

} // namespace netip
} // namespace knx
