// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/gateway_discovery_client.hpp"

#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/gateway_discovery_codec.hpp"
#include "knx/netip/detail/polling.hpp"
#include "knx/netip/udp_datagram_channel.hpp"
#include "knx/netip/netip_config.hpp"

namespace knx {
namespace netip {

using ControlPacketCodec = control_packet::Codec;

namespace {

static constexpr const char* kKnxNetIpMulticast = "224.0.23.12";
static constexpr uint16_t kKnxNetIpPort = knx::netip::config::kDefaultPort;

} // namespace

util::Result<void> GatewayDiscoveryClient::parseSearchResponsePacket(std::span<const uint8_t> data, GatewayInfo& info)
{
    return GatewayDiscoveryCodec::parseSearchResponse(data, info);
}

util::Result<void> GatewayDiscoveryClient::parseDescriptionResponsePacket(std::span<const uint8_t> data, GatewayInfo& info)
{
    return GatewayDiscoveryCodec::parseDescriptionResponse(data, info);
}

util::Result<void> GatewayDiscoveryClient::beginDiscover(platform::NetworkInterface& network, int timeoutMs, size_t maxGateways)
{
    finishDiscovery();

    auto sock = network.createUdpSocket();
    if (!sock) return util::ErrorCode::ResourceUnavailable;
    if (sock->open(0).isError()) return util::ErrorCode::ResourceUnavailable;

    auto sendResult = sendSearchRequest(*sock, network.ipAddress());
    if (sendResult.isError()) {
        sock->close();
        return sendResult.error();
    }

    discoverSocket_ = std::move(sock);
    discoveredGateways_.clear();
    if (maxGateways > 0) {
        discoveredGateways_.reserve(maxGateways);
    }
    discoverOperation_.active = true;
    discoverOperation_.startTimeMs = detail::nowMs(timingPlatform_);
    discoverOperation_.timeoutMs = timeoutMs;
    discoverOperation_.maxGateways = maxGateways;
    return util::Result<void>::ok();
}

util::Result<util::OperationProgressState> GatewayDiscoveryClient::pollDiscover()
{
    if (!discoverOperation_.active) {
        return util::ErrorCode::OperationNotReady;
    }
    if (!discoverSocket_ || !discoverSocket_->isOpen()) {
        finishDiscovery();
        return util::ErrorCode::NotInitialized;
    }

    while (discoverSocket_->available() > 0) {
        IpAddress srcAddr(0);
        uint16_t srcPort = 0;
        const int received = discoverSocket_->receive(scratchBuffer_.span(), srcAddr, srcPort);
        if (received < 6) {
            continue;
        }

        GatewayInfo info;
        if (parseSearchResponsePacket(scratchBuffer_.span().first(static_cast<size_t>(received)), info).isOk()) {
            discoveredGateways_.push_back(info);
            if (discoverOperation_.maxGateways > 0 &&
                discoveredGateways_.size() >= discoverOperation_.maxGateways) {
                finishDiscovery();
                return util::OperationProgressState::Success;
            }
        }
    }

    if (detail::remainingTimeoutMs(timingPlatform_, discoverOperation_.startTimeMs, discoverOperation_.timeoutMs) <= 0) {
        const bool foundGateways = !discoveredGateways_.empty();
        finishDiscovery();
        return foundGateways ? util::OperationProgressState::Success : util::OperationProgressState::Timeout;
    }

    return util::OperationProgressState::Pending;
}

std::vector<GatewayInfo> GatewayDiscoveryClient::discover(platform::NetworkInterface& network, int timeoutMs, size_t maxGateways)
{
    if (beginDiscover(network, timeoutMs, maxGateways).isError()) {
        return {};
    }

    auto terminal = detail::waitForTerminalProgress(timingPlatform_, [this]() { return pollDiscover(); });
    if (terminal.isError()) {
        finishDiscovery();
        return {};
    }

    if (terminal.value() == util::OperationProgressState::TransmissionFailed) {
        finishDiscovery();
        return {};
    }

    return takeDiscoveredGateways();
}

std::vector<GatewayInfo> GatewayDiscoveryClient::takeDiscoveredGateways()
{
    auto gateways = std::move(discoveredGateways_);
    discoveredGateways_.clear();
    return gateways;
}

util::Result<void> GatewayDiscoveryClient::getDescription(platform::NetworkInterface& network,
                                                          IpAddress host,
                                                          NetIpPort port,
                                                          int timeoutMs,
                                                          GatewayInfo& info)
{
    if (host.isZero() || !port.isValid()) return util::ErrorCode::InvalidParameter;

    auto sock = network.createUdpSocket();
    if (!sock) return util::ErrorCode::ResourceUnavailable;
    auto openRes = sock->open(0);
    if (openRes.isError()) return openRes.error();

    const IpAddress localAddr = network.ipAddress();
    const uint16_t localPort = sock->localPort();

    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = ControlPacketCodec::encodeDescriptionRequest(
        packet,
        control_packet::HpaiEndpoint{control_packet::HpaiProtocol::Udp, localAddr, localPort});
    if (encodeResult.isError()) {
        sock->close();
        return encodeResult.error();
    }

    auto exchangeResult = UdpDatagramChannel::exchange(*sock,
                                                       UdpDatagramEndpoint{host, port},
                                                       packet.span(),
                                                       scratchBuffer_.span(),
                                                       timeoutMs,
                                                       timingPlatform_);
    sock->close();
    if (exchangeResult.isError()) return exchangeResult.error();

    return parseDescriptionResponsePacket(scratchBuffer_.span().first(exchangeResult.value()), info);
}

util::Result<void> GatewayDiscoveryClient::sendSearchRequest(platform::UdpSocket& socket,
                                                             IpAddress localAddr)
{
    if (!socket.isOpen()) return util::ErrorCode::NotInitialized;

    const IpAddress multicastAddr = IpAddress::fromString(kKnxNetIpMulticast);
    if (multicastAddr.isZero()) return util::ErrorCode::InvalidAddress;

    PacketWriter packet(frameBuffer_.span());
    auto encodeResult = ControlPacketCodec::encodeSearchRequest(
        packet,
        control_packet::HpaiEndpoint{control_packet::HpaiProtocol::Udp, localAddr, socket.localPort()});
    if (encodeResult.isError()) return encodeResult.error();

    const int sent = socket.send(multicastAddr, kKnxNetIpPort, packet.span());
    if (sent != static_cast<int>(packet.size())) return util::ErrorCode::TransmissionFailed;

    return util::Result<void>::ok();
}

void GatewayDiscoveryClient::finishDiscovery() noexcept
{
    if (discoverSocket_) {
        discoverSocket_->close();
        discoverSocket_.reset();
    }
    discoverOperation_.reset();
}

} // namespace netip
} // namespace knx