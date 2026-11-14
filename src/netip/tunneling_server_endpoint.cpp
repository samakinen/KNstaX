// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/tunneling_server_endpoint.hpp"

#include "knx/netip/detail/polling.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/util/log.hpp"

#include <algorithm>
#include <array>

namespace knx {
namespace netip {

namespace {

const char* TAG = "KNX.NETIP.TunSrv";

using control_packet::Codec;
using control_packet::HpaiEndpoint;

}  // namespace

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
    localAddress_ = options_.dataEndpointAddress.isZero() ? network.ipAddress() : options_.dataEndpointAddress;
    // One connection per unique Individual Address (03/08/04 §2.2.2).
    channels_.assign(std::min(options_.maxChannels, options_.tunnelAddresses.size()), ChannelSlot{});
    for (size_t i = 0; i < channels_.size(); ++i) {
        channels_[i].info.tunnelAddress = options_.tunnelAddresses[i];
    }

    socket_ = network.createUdpSocket();
    if (!socket_) {
        return util::ErrorCode::ResourceUnavailable;
    }

    auto openResult = socket_->open(options_.port.value());
    if (openResult.isError()) {
        socket_.reset();
        return openResult.error();
    }

    KNX_LOGI(TAG, "Tunneling server open on UDP %u (%u connections)",
             static_cast<unsigned>(options_.port.value()),
             static_cast<unsigned>(channels_.size()));
    return util::Result<void>::ok();
}

void TunnelingServerEndpoint::close()
{
    if (socket_) {
        for (auto& slot : channels_) {
            if (slot.active) {
                terminate(slot, "server closing");
            }
        }
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

TunnelingServerEndpoint::ChannelSlot* TunnelingServerEndpoint::allocateChannel(
    std::span<const IndividualAddress> candidates)
{
    for (auto& slot : channels_) {
        if (slot.active) {
            continue;
        }
        if (!candidates.empty()
            && std::find(candidates.begin(), candidates.end(), slot.info.tunnelAddress) == candidates.end()) {
            continue;
        }
        const IndividualAddress address = slot.info.tunnelAddress;
        slot = ChannelSlot{};
        slot.active = true;
        slot.info.channelId = ChannelId(static_cast<uint8_t>(&slot - channels_.data() + 1u));
        slot.info.tunnelAddress = address;
        slot.info.lastActivityMs = nowMs();
        return &slot;
    }
    return nullptr;
}

void TunnelingServerEndpoint::resolveEndpoint(HpaiEndpoint& endpoint, IpAddress sourceAddress, NetIpPort sourcePort)
{
    // "Route Back" (03/08/02 §8.6.2.2): reply to the source of the packet.
    if (Codec::isRouteBack(endpoint)) {
        endpoint.address = sourceAddress;
        endpoint.port = sourcePort.value();
    }
}

util::Result<void> TunnelingServerEndpoint::sendTo(const HpaiEndpoint& endpoint, std::span<const uint8_t> datagram)
{
    if (!isOpen()) {
        return util::ErrorCode::NotInitialized;
    }
    const int sent = socket_->send(endpoint.address, endpoint.port, datagram);
    if (sent != static_cast<int>(datagram.size())) {
        return util::ErrorCode::TransmissionFailed;
    }
    return util::Result<void>::ok();
}

util::Result<void> TunnelingServerEndpoint::sendChannelStatusResponse(NetIpServiceType serviceType,
                                                                      const HpaiEndpoint& destination,
                                                                      uint8_t channelId,
                                                                      uint8_t status)
{
    PacketWriter packet(txBuffer_.span());
    auto encodeResult = Codec::encodeChannelStatusResponse(packet, serviceType, channelId, status);
    if (encodeResult.isError()) {
        return encodeResult.error();
    }
    return sendTo(destination, packet.span());
}

util::Result<void> TunnelingServerEndpoint::sendConnectResponse(const HpaiEndpoint& destination,
                                                                uint8_t channelId,
                                                                uint8_t status,
                                                                IndividualAddress tunnelAddress)
{
    // 03/08/02 §7.8.2 / Figure 49: channel ID, status, the data endpoint
    // HPAI of this server and the CRD with the tunnel's Individual Address.
    const HpaiEndpoint dataEndpoint{control_packet::HpaiProtocol::Udp, localAddress_, options_.port.value()};
    PacketWriter packet(txBuffer_.span());
    auto encodeResult = Codec::encodeConnectResponse(packet, channelId, status, dataEndpoint, tunnelAddress.raw);
    if (encodeResult.isError()) {
        return encodeResult.error();
    }
    return sendTo(destination, packet.span());
}

void TunnelingServerEndpoint::terminate(ChannelSlot& slot, const char* reason)
{
    // The server terminates with a DISCONNECT_REQUEST to the client's control
    // endpoint (03/08/02 §5.4, §5.5; 03/08/04 §2.6.1).
    KNX_LOGI(TAG, "Closing channel %u: %s", static_cast<unsigned>(slot.info.channelId.value()), reason);
    if (isOpen()) {
        PacketWriter packet(txBuffer_.span());
        const HpaiEndpoint ownControl{control_packet::HpaiProtocol::Udp, localAddress_, options_.port.value()};
        if (Codec::encodeDisconnectRequest(packet, slot.info.channelId.value(), ownControl).isOk()) {
            (void)sendTo(slot.info.controlEndpoint, packet.span());
        }
    }
    const IndividualAddress address = slot.info.tunnelAddress;
    slot = ChannelSlot{};
    slot.info.tunnelAddress = address;
}

util::Result<void> TunnelingServerEndpoint::handleConnectionRequest(std::span<const uint8_t> frame,
                                                                    IpAddress sourceAddress,
                                                                    NetIpPort sourcePort)
{
    auto requestResult = Codec::decodeConnectRequest(frame);
    if (requestResult.isError()) {
        return requestResult.error();
    }
    auto request = requestResult.value();
    resolveEndpoint(request.controlEndpoint, sourceAddress, sourcePort);
    resolveEndpoint(request.dataEndpoint, sourceAddress, sourcePort);

    const auto refuse = [&](uint8_t status) {
        KNX_LOGW(TAG, "CONNECT_REQUEST refused: status 0x%02X", static_cast<unsigned>(status));
        return sendChannelStatusResponse(control_packet::kServiceConnectionResponse, request.controlEndpoint, 0,
                                         status);
    };

    // Only tunnelling on the data link layer is served here.
    if (request.connectionType != Codec::kConnectionTypeTunnel) {
        return refuse(control_packet::kStatusConnectionType);
    }
    if (request.knxLayer != Codec::kTunnelLayerTp1) {
        return refuse(control_packet::kStatusTunnellingLayer);
    }

    ChannelSlot* slot = nullptr;
    if (request.hasRequestedAddress) {
        // Extended CRI: the client asks for one particular address
        // (03/08/04 §5.4.3).
        const IndividualAddress requested(request.requestedAddress);
        const auto known = std::find(options_.tunnelAddresses.begin(), options_.tunnelAddresses.end(), requested);
        if (known == options_.tunnelAddresses.end()
            || static_cast<size_t>(known - options_.tunnelAddresses.begin()) >= channels_.size()) {
            return refuse(control_packet::kStatusNoTunnellingAddress);
        }
        const std::array<IndividualAddress, 1> wanted{requested};
        slot = allocateChannel(wanted);
        if (slot == nullptr) {
            return refuse(control_packet::kStatusConnectionInUse);
        }
    } else {
        slot = allocateChannel({});
        if (slot == nullptr) {
            return refuse(control_packet::kStatusNoMoreConnections);
        }
    }

    slot->info.remoteAddress = sourceAddress;
    slot->info.remotePort = sourcePort;
    slot->info.controlEndpoint = request.controlEndpoint;
    slot->info.dataEndpoint = request.dataEndpoint;
    markChannelActivity(*slot);

    KNX_LOGI(TAG, "CONNECT_REQUEST accepted channel=%u IA=0x%04X",
             static_cast<unsigned>(slot->info.channelId.value()),
             static_cast<unsigned>(slot->info.tunnelAddress.raw));
    return sendConnectResponse(request.controlEndpoint, slot->info.channelId.value(), control_packet::kStatusNoError,
                               slot->info.tunnelAddress);
}

util::Result<void> TunnelingServerEndpoint::handleConnectionStateRequest(std::span<const uint8_t> frame,
                                                                         IpAddress sourceAddress,
                                                                         NetIpPort sourcePort)
{
    auto requestResult = Codec::decodeChannelEndpointRequest(frame, control_packet::kServiceConnectionStateRequest);
    if (requestResult.isError()) {
        return requestResult.error();
    }
    auto request = requestResult.value();
    resolveEndpoint(request.controlEndpoint, sourceAddress, sourcePort);

    auto* slot = findChannelById(ChannelId(request.channelId));
    if (slot == nullptr || slot->info.remoteAddress != sourceAddress) {
        return sendChannelStatusResponse(control_packet::kServiceConnectionStateResponse, request.controlEndpoint,
                                         request.channelId, control_packet::kStatusConnectionId);
    }

    markChannelActivity(*slot);
    return sendChannelStatusResponse(control_packet::kServiceConnectionStateResponse, request.controlEndpoint,
                                     request.channelId, control_packet::kStatusNoError);
}

util::Result<void> TunnelingServerEndpoint::handleDisconnectRequest(std::span<const uint8_t> frame,
                                                                    IpAddress sourceAddress,
                                                                    NetIpPort sourcePort)
{
    auto requestResult = Codec::decodeChannelEndpointRequest(frame, control_packet::kServiceDisconnectRequest);
    if (requestResult.isError()) {
        return requestResult.error();
    }
    auto request = requestResult.value();
    resolveEndpoint(request.controlEndpoint, sourceAddress, sourcePort);

    auto* slot = findChannelById(ChannelId(request.channelId));
    if (slot == nullptr || slot->info.remoteAddress != sourceAddress) {
        return sendChannelStatusResponse(control_packet::kServiceDisconnectResponse, request.controlEndpoint,
                                         request.channelId, control_packet::kStatusConnectionId);
    }

    KNX_LOGI(TAG, "DISCONNECT_REQUEST channel=%u", static_cast<unsigned>(request.channelId));
    const IndividualAddress address = slot->info.tunnelAddress;
    *slot = ChannelSlot{};
    slot->info.tunnelAddress = address;

    return sendChannelStatusResponse(control_packet::kServiceDisconnectResponse, request.controlEndpoint,
                                     request.channelId, control_packet::kStatusNoError);
}

util::Result<void> TunnelingServerEndpoint::handleTunnelingRequest(std::span<const uint8_t> frame,
                                                                   IpAddress sourceAddress,
                                                                   NetIpPort /*sourcePort*/)
{
    auto decodeResult = Codec::decodeTunnelingRequest(frame);
    if (decodeResult.isError()) {
        return decodeResult.error();
    }

    const auto request = decodeResult.value();
    auto* slot = findChannelById(ChannelId(request.channelId));
    if (slot == nullptr || slot->info.remoteAddress != sourceAddress) {
        // Not a connection of ours: there is no data endpoint to answer.
        return util::Result<void>::ok();
    }

    const auto sendAck = [&]() {
        PacketWriter ack(txBuffer_.span());
        auto ackResult = Codec::encodeTunnelingAck(ack, request.channelId, request.sequence,
                                                   control_packet::kStatusNoError);
        if (ackResult.isError()) {
            return util::Result<void>(ackResult.error());
        }
        return sendTo(slot->info.dataEndpoint, ack.span());
    };

    // 03/08/04 §2.6.1, server side.
    const uint8_t expected = slot->info.expectedRxSequence;
    if (request.sequence == expected) {
        // Expected: acknowledge and process.
        auto ackResult = sendAck();
        markChannelActivity(*slot);
        slot->info.expectedRxSequence = static_cast<uint8_t>(expected + 1u);
        if (receiveCallback_) {
            receiveCallback_(slot->info.channelId, request.cemi);
        }
        return ackResult;
    }
    if (request.sequence == static_cast<uint8_t>(expected - 1u)) {
        // One less: our ACK was lost. Acknowledge again and discard.
        markChannelActivity(*slot);
        return sendAck();
    }
    // Anything else: no reply, discard. "The server shall not retrigger the
    // timeout after messages received with unexpected sequence number"
    // (03/08/02 §5.4).
    KNX_LOGW(TAG, "Sequence %u on channel %u, expected %u: discarded",
             static_cast<unsigned>(request.sequence), static_cast<unsigned>(request.channelId),
             static_cast<unsigned>(expected));
    return util::Result<void>::ok();
}

util::Result<void> TunnelingServerEndpoint::handleTunnelingAck(std::span<const uint8_t> frame)
{
    auto ackResult = Codec::decodeTunnelingAck(frame);
    if (ackResult.isError()) {
        return ackResult.error();
    }
    const auto ack = ackResult.value();
    auto* slot = findChannelById(ChannelId(ack.channelId));
    if (slot == nullptr || slot->inFlight.empty() || ack.sequence != slot->info.nextTxSequence) {
        return util::Result<void>::ok();
    }
    if (ack.status != control_packet::kStatusNoError) {
        // "the status of a received TUNNELLING_ACK frame signals any kind of
        // error condition": repeat once, then terminate (03/08/04 §2.6.1).
        if (slot->inFlightRepeated) {
            terminate(*slot, "TUNNELLING_REQUEST refused twice");
            return util::Result<void>::ok();
        }
        slot->inFlightRepeated = true;
        return transmitInFlight(*slot);
    }

    markChannelActivity(*slot);
    slot->info.nextTxSequence = static_cast<uint8_t>(slot->info.nextTxSequence + 1u);
    slot->inFlight.clear();
    slot->inFlightRepeated = false;
    startNextQueued(*slot);
    return util::Result<void>::ok();
}

util::Result<void> TunnelingServerEndpoint::transmitInFlight(ChannelSlot& slot)
{
    PacketWriter packet(txBuffer_.span());
    auto encodeResult = Codec::encodeTunnelingRequest(packet, slot.info.channelId.value(), slot.info.nextTxSequence,
                                                      slot.inFlight);
    if (encodeResult.isError()) {
        return encodeResult.error();
    }
    slot.inFlightSentMs = nowMs();
    return sendTo(slot.info.dataEndpoint, packet.span());
}

void TunnelingServerEndpoint::startNextQueued(ChannelSlot& slot)
{
    while (slot.active && slot.inFlight.empty() && !slot.queue.empty()) {
        slot.inFlight = std::move(slot.queue.front());
        slot.queue.pop_front();
        slot.inFlightRepeated = false;
        if (transmitInFlight(slot).isError()) {
            slot.inFlight.clear();
        }
    }
}

void TunnelingServerEndpoint::runTimers()
{
    const uint32_t now = nowMs();
    for (auto& slot : channels_) {
        if (!slot.active) {
            continue;
        }

        if (options_.channelIdleTimeoutMs != 0 && (now - slot.info.lastActivityMs) >= options_.channelIdleTimeoutMs) {
            terminate(slot, "no heartbeat");
            continue;
        }

        if (!slot.inFlight.empty() && (now - slot.inFlightSentMs) >= kTunnelingRequestTimeoutMs) {
            if (slot.inFlightRepeated) {
                terminate(slot, "TUNNELLING_REQUEST not acknowledged twice");
                continue;
            }
            slot.inFlightRepeated = true;
            (void)transmitInFlight(slot);
        }
    }
}

util::Result<bool> TunnelingServerEndpoint::poll(int timeoutMs)
{
    if (!isOpen()) {
        return util::ErrorCode::NotInitialized;
    }

    runTimers();

    auto waitResult = detail::waitUntilReadable(timingPlatform_, timeoutMs, *socket_);
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
        case control_packet::kServiceTunnelingAck.value():
            return handleTunnelingAck(frame).isOk();
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

    if (!slot->inFlight.empty()) {
        if (slot->queue.size() >= kMaxQueuedFrames) {
            return util::ErrorCode::QueueFull;
        }
        slot->queue.emplace_back(cemi.begin(), cemi.end());
        return util::Result<void>::ok();
    }

    slot->inFlight.assign(cemi.begin(), cemi.end());
    slot->inFlightRepeated = false;
    auto sendResult = transmitInFlight(*slot);
    if (sendResult.isError()) {
        slot->inFlight.clear();
    }
    return sendResult;
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
