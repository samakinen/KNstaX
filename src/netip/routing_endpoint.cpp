// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/routing_endpoint.hpp"

#include "knx/netip/core_server_responder.hpp"
#include "knx/netip/detail/polling.hpp"
#include "knx/netip/secure_udp_datagram_channel.hpp"

#include "knx/util/log.hpp"
#include "knx/util/result.hpp"

#include <algorithm>

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

uint32_t RoutingEndpoint::nowMs() const
{
    return detail::nowMs(timingPlatform_);
}

util::Result<void> RoutingEndpoint::open(platform::NetworkInterface& network, const Options& options)
{
    close();
    options_ = options;
    flowControl_ = RoutingFlowControl(options_.flowControl);
    security_.store(options_.security, std::memory_order_relaxed);

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

    // Preferred over the interface address in the options because that one is
    // allowed to be 0.0.0.0 ("system default"), which is not an address a
    // client can send a search response to.
    localAddr_ = interfaceAddr_.isZero() ? network.ipAddress() : interfaceAddr_;

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
    localAddr_ = IpAddress(0);
    security_.store(nullptr, std::memory_order_relaxed);
    flowControl_.reset();
}

bool RoutingEndpoint::isOpen() const
{
    return sock_ && sock_->isOpen();
}

void RoutingEndpoint::setSecurity(NetIpSecurity* security) noexcept
{
    security_.store(security, std::memory_order_release);
}

bool RoutingEndpoint::hasSecurity() const noexcept
{
    return security_.load(std::memory_order_acquire) != nullptr;
}

IpAddress RoutingEndpoint::localAddress() const
{
    return localAddr_;
}

bool RoutingEndpoint::isSendBlocked() const
{
    return !flowControl_.canSend(nowMs());
}

uint32_t RoutingEndpoint::millisecondsUntilSendable() const
{
    return flowControl_.millisecondsUntilSendable(nowMs());
}

util::Result<void> RoutingEndpoint::sendToGroup(std::span<const uint8_t> datagram, bool allowSecure)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;

    const uint32_t now = nowMs();
    if (!flowControl_.canSend(now)) {
        txDeferred_.fetch_add(1);
        return util::ErrorCode::Busy;
    }

    NetIpSecurity* const security = security_.load(std::memory_order_acquire);

    util::Result<void> result = util::Result<void>::ok();
    if (allowSecure && security) {
        SecureUdpDatagramChannel secureChannel(*sock_,
                                               *security,
                                               secureWrapBuffer_.span(),
                                               securePlainBuffer_.span());
        result = secureChannel.send(UdpDatagramEndpoint{multicastAddr_, options_.port}, datagram);
    } else {
        const int sent = sock_->send(multicastAddr_, options_.port.value(), datagram);
        if (sent != static_cast<int>(datagram.size())) {
            result = util::Result<void>::err(util::ErrorCode::TransmissionFailed);
        }
    }

    if (result.isOk()) {
        flowControl_.noteSent(now);
    }
    return result;
}

util::Result<void> RoutingEndpoint::sendCemiCarrier(NetIpServiceType serviceType,
                                                    std::span<const uint8_t> cemi,
                                                    bool allowSecure)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;

    auto pktResult = (serviceType == RoutingCodec::ST_ROUTING_SYSTEM_BROADCAST)
        ? RoutingCodec::encodeRoutingSystemBroadcast(cemi, txBuffer_.span())
        : RoutingCodec::encodeRoutingIndication(cemi, txBuffer_.span());
    if (pktResult.isError()) {
        return pktResult.error();
    }

    return sendToGroup(txBuffer_.span().first(pktResult.value()), allowSecure);
}

util::Result<void> RoutingEndpoint::sendRoutingIndication(std::span<const uint8_t> cemi)
{
    return sendCemiCarrier(RoutingCodec::ST_ROUTING_INDICATION, cemi, /*allowSecure=*/true);
}

util::Result<void> RoutingEndpoint::sendRoutingSystemBroadcast(std::span<const uint8_t> cemi)
{
    // "Even if secure routing is enabled, a ROUTING_SYSTEM_BROADCAST shall not
    // be wrapped in a SECURE_WRAPPER" (03/02/06 §4.1.3). The payload carries
    // its own protection through the Security Control Field instead.
    return sendCemiCarrier(RoutingCodec::ST_ROUTING_SYSTEM_BROADCAST, cemi, /*allowSecure=*/false);
}

util::Result<void> RoutingEndpoint::sendRoutingLostMessage(const RoutingLostMessageInfo& info)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;

    std::array<uint8_t, RoutingCodec::ROUTING_LOST_MESSAGE_FRAME_LEN> pkt{};
    auto pktResult = RoutingCodec::encodeRoutingLostMessage(info, pkt);
    if (pktResult.isError()) {
        return pktResult.error();
    }

    return sendToGroup(pkt, /*allowSecure=*/true);
}

util::Result<void> RoutingEndpoint::sendDatagram(IpAddress address,
                                                 NetIpPort port,
                                                 std::span<const uint8_t> datagram)
{
    if (!sock_ || !sock_->isOpen()) return util::ErrorCode::NotInitialized;
    if (address.isZero() || !port.isValid()) return util::ErrorCode::InvalidAddress;

    // Deliberately outside the routing budget: that budget exists to protect
    // media couplers on the multicast group, and a unicast search response
    // reaches none of them.
    const int sent = sock_->send(address, port.value(), datagram);
    if (sent != static_cast<int>(datagram.size())) return util::ErrorCode::TransmissionFailed;
    return util::Result<void>::ok();
}

util::Result<RoutingEndpoint::Inbound> RoutingEndpoint::receive(std::span<uint8_t> cemiOut, uint32_t timeoutMs)
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
    const int received = sock_->receive(inData, srcAddr, srcPort);
    if (received <= 0) {
        return util::ErrorCode::ResourceUnavailable;
    }
    size_t inLen = static_cast<size_t>(received);

    KnxNetIpHeader hdr;
    if (RoutingCodec::decodeHeader(inData.first(inLen), hdr).isError()) {
        return util::ErrorCode::DecodeFailed;
    }

    // Decrypt only what was actually encrypted. Classifying first is what lets
    // discovery and the system-broadcast domain keep working on a secured
    // group: both are specified to travel in the clear there.
    NetIpSecurity* const security = security_.load(std::memory_order_acquire);

    if (hdr.serviceType == ip_secure::SecureWrapper::kServiceTypeSecureWrapper) {
        if (!security) {
            KNX_LOGD(TAG, "SECURE_WRAPPER received but secure routing is not configured");
            return util::ErrorCode::OperationNotSupported;
        }
        SecurePacketCodec codec(*security, secureWrapBuffer_.span(), securePlainBuffer_.span());
        auto unwrapped = codec.unwrapInPlace(inData.first(inLen), inData);
        if (unwrapped.isError()) {
            return unwrapped.error();
        }
        inLen = unwrapped.value();
        if (RoutingCodec::decodeHeader(inData.first(inLen), hdr).isError()) {
            return util::ErrorCode::DecodeFailed;
        }
    } else if (security && hdr.serviceType == RoutingCodec::ST_ROUTING_INDICATION) {
        // A secured installation does not accept plain process data; taking it
        // would make the encryption advisory.
        KNX_LOGD(TAG, "Dropping unsecured ROUTING_INDICATION on a secured group");
        return util::ErrorCode::AccessDenied;
    }

    const auto frame = inData.first(inLen);

    if (hdr.serviceType == RoutingCodec::ST_ROUTING_INDICATION ||
        hdr.serviceType == RoutingCodec::ST_ROUTING_SYSTEM_BROADCAST) {
        const bool systemBroadcast = (hdr.serviceType == RoutingCodec::ST_ROUTING_SYSTEM_BROADCAST);
        auto cemiResult = systemBroadcast ? RoutingCodec::decodeRoutingSystemBroadcast(frame)
                                          : RoutingCodec::decodeRoutingIndication(frame);
        if (cemiResult.isError()) {
            return util::ErrorCode::DecodeFailed;
        }
        const auto cemi = cemiResult.value();
        if (cemi.size() > cemiOut.size()) {
            return util::ErrorCode::BufferTooSmall;
        }
        std::copy(cemi.begin(), cemi.end(), cemiOut.begin());
        return Inbound{
            .kind = systemBroadcast ? InboundKind::SystemBroadcast : InboundKind::RoutingIndication,
            .cemiLength = cemi.size(),
        };
    }

    if (hdr.serviceType == RoutingCodec::ST_ROUTING_LOST_MESSAGE) {
        RoutingLostMessageInfo info{};
        if (RoutingCodec::decodeRoutingLostMessage(frame, info).isOk()) {
            lostMessagesSeen_.fetch_add(1);
            lostCountTotal_.fetch_add(info.lostMessages);
            KNX_LOGW(TAG, "ROUTING_LOST_MESSAGE: lost=%u state=0x%02X",
                     static_cast<unsigned>(info.lostMessages),
                     static_cast<unsigned>(info.deviceState));
        }
        return Inbound{};
    }

    if (hdr.serviceType == RoutingCodec::ST_ROUTING_BUSY) {
        RoutingBusyInfo info{};
        if (RoutingCodec::decodeRoutingBusy(frame, info).isOk()) {
            busySeen_.fetch_add(1);
            // The random component needs a source of randomness the endpoint
            // does not own; the deterministic wait is the part the spec makes
            // mandatory, and IpRoutingPhysical supplies the rest.
            flowControl_.noteBusyReceived(info, nowMs());
            KNX_LOGD(TAG, "ROUTING_BUSY: wait=%u ms control=0x%04X",
                     static_cast<unsigned>(info.waitTimeMs),
                     static_cast<unsigned>(info.controlField));
        }
        return Inbound{};
    }

    if (controlHandler_ && CoreServerResponder::handlesServiceType(hdr.serviceType)) {
        controlHandler_(frame, srcAddr, srcPort);
        return Inbound{};
    }

    return Inbound{};
}

} // namespace netip
} // namespace knx
