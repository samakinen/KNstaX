// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/routing_endpoint.hpp"

#include "knx/netip/detail/polling.hpp"
#include "knx/netip/routing.hpp"
#include "knx/netip/secure_udp_datagram_channel.hpp"

#include "knx/util/log.hpp"
#include "knx/util/result.hpp"

#include <array>

namespace knx {
namespace netip {

static const char* TAG = "KNX.RoutingEndpoint";

RoutingEndpoint::RoutingEndpoint()
    : sock_(nullptr)
    , options_()
{
}

RoutingEndpoint::~RoutingEndpoint()
{
    close();
}

util::Result<void> RoutingEndpoint::open(platform::NetworkInterface& network, const Options& options)
{
    close();
    options_ = options;

    multicastAddr_ = options_.multicastGroup;
    interfaceAddr_ = options_.interfaceAddress;
    if (multicastAddr_.isZero()) {
        uint8_t a = 0, b = 0, c = 0, d = 0;
        options_.multicastGroup.toOctets(a, b, c, d);
        KNX_LOGE(TAG, "Invalid multicast group '%u.%u.%u.%u'",
                 static_cast<unsigned>(a), static_cast<unsigned>(b),
                 static_cast<unsigned>(c), static_cast<unsigned>(d));
        return util::ErrorCode::InvalidAddress;
    }

    sock_ = network.createUdpSocket();
    if (!sock_) return util::ErrorCode::ResourceUnavailable;
    auto openRes = sock_->open(options_.port.value());
    if (openRes.isError()) {
        sock_.reset();
        return openRes.error();
    }

    (void)sock_->setMulticastInterface(interfaceAddr_);
    (void)sock_->setMulticastLoopback(options_.loopback ? platform::MulticastLoopbackMode::Enable : platform::MulticastLoopbackMode::Disable);
    (void)sock_->setMulticastTtl(options_.ttl);

    auto joinRes = sock_->joinMulticast(multicastAddr_, interfaceAddr_);
    if (joinRes.isError()) {
        close();
        return joinRes.error();
    }

    return util::Result<void>::ok();
}

void RoutingEndpoint::close()
{
    if (sock_) {
        sock_->leaveMulticast(multicastAddr_, interfaceAddr_);
        sock_->close();
        sock_.reset();
    }
    multicastAddr_ = IpAddress(0);
    interfaceAddr_ = IpAddress(0);
}

bool RoutingEndpoint::isOpen() const
{
    return sock_ && sock_->isOpen();
}

util::Result<void> RoutingEndpoint::sendRoutingIndication(std::span<const uint8_t> cemi)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;

    std::array<uint8_t, 2048> pkt{};
    auto pktResult = RoutingCodec::encodeRoutingIndication(cemi, pkt);
    if (pktResult.isError()) {
        return pktResult.error();
    }

    if (options_.security) {
        SecureUdpDatagramChannel secureChannel(*sock_,
                                               *options_.security,
                                               secureWrapBuffer_.span(),
                                               securePlainBuffer_.span());
        return secureChannel.send(UdpDatagramEndpoint{multicastAddr_, options_.port},
                                  std::span<const uint8_t>(pkt).first(pktResult.value()));
    }

    const int sent = sock_->send(multicastAddr_, options_.port.value(), std::span<const uint8_t>(pkt).first(pktResult.value()));
    if (sent != static_cast<int>(pktResult.value())) {
        return util::ErrorCode::TransmissionFailed;
    }
    return util::Result<void>::ok();
}

util::Result<void> RoutingEndpoint::sendRoutingLostMessage(uint16_t lostCount)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;

    std::array<uint8_t, RoutingCodec::ROUTING_LOST_MESSAGE_FRAME_LEN> pkt{};
    auto pktResult = RoutingCodec::encodeRoutingLostMessage(lostCount, pkt);
    if (pktResult.isError()) {
        return pktResult.error();
    }

    if (options_.security) {
        SecureUdpDatagramChannel secureChannel(*sock_,
                                               *options_.security,
                                               secureWrapBuffer_.span(),
                                               securePlainBuffer_.span());
        return secureChannel.send(UdpDatagramEndpoint{multicastAddr_, options_.port}, pkt);
    }

    const int sent = sock_->send(multicastAddr_, options_.port.value(), pkt);
    if (sent != static_cast<int>(pkt.size())) {
        return util::ErrorCode::TransmissionFailed;
    }
    return util::Result<void>::ok();
}

util::Result<size_t> RoutingEndpoint::receiveRoutingIndication(std::span<uint8_t> cemiOut, uint32_t timeoutMs)
{
    if (!sock_ || !sock_->isOpen()) {
        return util::ErrorCode::NotInitialized;
    }

    auto waitResult = detail::waitUntilReadable(timingPlatform_, timeoutMs, [this]() {
        return sock_ && sock_->available() > 0;
    });
    if (waitResult.isError()) {
        return waitResult.error();
    }
    if (!waitResult.value()) {
        return util::ErrorCode::Timeout;
    }

    IpAddress srcAddr(0);
    uint16_t srcPort = 0;
    std::span<uint8_t> inData = rxBuffer_.span();
    size_t inLen = 0;
    if (options_.security) {
        SecureUdpDatagramChannel secureChannel(*sock_,
                                               *options_.security,
                                               secureWrapBuffer_.span(),
                                               securePlainBuffer_.span());
        auto receiveResult = secureChannel.receive(inData, srcAddr, srcPort);
        if (receiveResult.isError()) return receiveResult.error();
        inLen = receiveResult.value();
    } else {
        const int n = sock_->receive(inData, srcAddr, srcPort);
        if (n <= 0) {
            return util::ErrorCode::ResourceUnavailable;
        }
        inLen = static_cast<size_t>(n);
    }

    KnxNetIpHeader hdr;
    if (RoutingCodec::decodeHeader(inData.first(inLen), hdr).isError()) {
        return util::ErrorCode::DecodeFailed;
    }

    if (hdr.serviceType == RoutingCodec::ST_ROUTING_INDICATION) {
        auto cemiResult = RoutingCodec::decodeRoutingIndication(inData.first(inLen));
        if (cemiResult.isError()) {
            return util::ErrorCode::DecodeFailed;
        }
        const auto cemi = cemiResult.value();
        if (cemi.size() > cemiOut.size()) {
            return util::ErrorCode::BufferTooSmall;
        }
        std::copy(cemi.begin(), cemi.end(), cemiOut.begin());
        return cemi.size();
    }

    if (hdr.serviceType == RoutingCodec::ST_ROUTING_LOST_MESSAGE) {
        uint16_t lost = 0;
        if (RoutingCodec::decodeRoutingLostMessage(inData.first(inLen), lost).isOk()) {
            lostMessagesSeen_.fetch_add(1);
            lostCountTotal_.fetch_add(lost);
            KNX_LOGW(TAG, "ROUTING_LOST_MESSAGE: lost=%u", static_cast<unsigned>(lost));
        }
        return util::ErrorCode::ResourceUnavailable;
    }

    return util::ErrorCode::OperationNotSupported;
}

} // namespace netip
} // namespace knx
