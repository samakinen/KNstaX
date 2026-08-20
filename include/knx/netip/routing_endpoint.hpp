// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/netip/netip_security.hpp"
#include "knx/netip/routing.hpp"
#include "knx/netip/routing_flow_control.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/types.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace knx {
namespace netip {

/**
 * @brief The device's socket on the KNX IP medium.
 *
 * One UDP socket carries everything a KNX IP device says and hears: routing
 * indications, the system-broadcast domain, flow control, and — because they
 * arrive on the same group and port — the Core discovery and self-description
 * requests. Splitting those across sockets would mean two binds on 3671.
 *
 * Inbound datagrams are classified from the KNXnet/IP header before anything
 * else happens to them, which is what makes Secure Routing work rather than
 * break discovery: only a SECURE_WRAPPER is decrypted, and the services that
 * are specified to travel in the clear even in a secure installation —
 * ROUTING_SYSTEM_BROADCAST (03/02/06 §4.1.3) and discovery — are recognised
 * instead of failing an unwrap they were never meant to pass.
 */
class RoutingEndpoint {
public:
    struct Options {
        IpAddress multicastGroup{IpAddress::fromOctets(224, 0, 23, 12)};
        NetIpPort port{NetIpPort(config::kDefaultPort)};
        IpAddress interfaceAddress{IpAddress(0)};
        uint8_t ttl{1};
        bool loopback{true};
        NetIpSecurity* security{nullptr};
        /// Egress budget and ROUTING_BUSY back-off; see RoutingFlowControl.
        RoutingFlowControl::Config flowControl{};
    };

    /// What a received datagram turned out to be, once classified.
    enum class InboundKind : uint8_t {
        None = 0,
        /// An ordinary ROUTING_INDICATION.
        RoutingIndication,
        /// A ROUTING_SYSTEM_BROADCAST — same cEMI, different domain.
        SystemBroadcast,
    };

    struct Inbound {
        InboundKind kind{InboundKind::None};
        size_t cemiLength{0};
    };

    /// Datagrams belonging to a service this endpoint does not implement
    /// itself, handed over with the source they came from so the handler can
    /// answer through `sendDatagram()`.
    using ControlDatagramHandler =
        util::InplaceFunction<void(std::span<const uint8_t> datagram, IpAddress source, uint16_t sourcePort), 64>;

    RoutingEndpoint();
    ~RoutingEndpoint();

    RoutingEndpoint(const RoutingEndpoint&) = delete;
    RoutingEndpoint& operator=(const RoutingEndpoint&) = delete;

    util::Result<void> open(platform::NetworkInterface& network, const Options& options);

    void setTimingPlatform(platform::TimingPlatform* timingPlatform) noexcept { timingPlatform_ = timingPlatform; }
    platform::TimingPlatform* timingPlatform() const noexcept { return timingPlatform_; }

    void setControlDatagramHandler(ControlDatagramHandler handler) { controlHandler_ = std::move(handler); }

    void close();
    bool isOpen() const;

    /// The address this socket sends from, once bound. Reported to a client in
    /// the control endpoint of a search response.
    IpAddress localAddress() const;

    /**
     * Swap the secure-routing context while the socket stays open.
     *
     * ETS turns Secure Routing on by downloading a backbone key, and expects it
     * to apply without a restart. `nullptr` returns the group to plain
     * multicast, which is how it turns it back off.
     *
     * The pointer is read once per datagram from whichever task is sending or
     * receiving, so it is stored atomically; the caller owns the object and
     * must keep any previously installed one alive for as long as this endpoint
     * is open.
     */
    void setSecurity(NetIpSecurity* security) noexcept;
    [[nodiscard]] bool hasSecurity() const noexcept;

    util::Result<void> sendRoutingIndication(std::span<const uint8_t> cemi);

    /// The system-broadcast domain. Never wrapped, even under Secure Routing.
    util::Result<void> sendRoutingSystemBroadcast(std::span<const uint8_t> cemi);

    util::Result<void> sendRoutingLostMessage(const RoutingLostMessageInfo& info);

    /// Unicast escape hatch for the control services that share this socket.
    util::Result<void> sendDatagram(IpAddress address, NetIpPort port, std::span<const uint8_t> datagram);

    /// Receive and classify one datagram. Control-service datagrams are
    /// dispatched to the handler and reported as `InboundKind::None`, so a
    /// caller draining frames does not have to know they exist.
    util::Result<Inbound> receive(std::span<uint8_t> cemiOut, uint32_t timeoutMs);

    /// True while an outbound datagram would breach the egress budget or a
    /// ROUTING_BUSY back-off. The caller decides whether to wait or to drop.
    bool isSendBlocked() const;
    uint32_t millisecondsUntilSendable() const;

    uint64_t routingLostMessagesSeen() const { return lostMessagesSeen_.load(); }
    uint64_t routingLostCountTotal() const { return lostCountTotal_.load(); }
    uint64_t routingBusySeen() const { return busySeen_.load(); }
    uint64_t txDeferred() const { return txDeferred_.load(); }

private:
    util::Result<void> sendCemiCarrier(NetIpServiceType serviceType, std::span<const uint8_t> cemi, bool allowSecure);
    util::Result<void> sendToGroup(std::span<const uint8_t> datagram, bool allowSecure);
    uint32_t nowMs() const;

    std::unique_ptr<platform::UdpSocket> sock_;
    Options options_;
    IpAddress multicastAddr_{IpAddress(0)};
    IpAddress interfaceAddr_{IpAddress(0)};
    IpAddress localAddr_{IpAddress(0)};

    DatagramBuffer<config::kUdpBufferSize> rxBuffer_{};
    DatagramBuffer<config::kUdpBufferSize> txBuffer_{};
    DatagramBuffer<config::kUdpBufferSize> securePlainBuffer_{};
    SecureDatagramBuffer<config::kUdpBufferSize, ip_secure::SecureWrapper::kOverhead> secureWrapBuffer_{};
    platform::TimingPlatform* timingPlatform_{nullptr};

    ControlDatagramHandler controlHandler_{};
    /// Read on both the send and receive paths, which are different tasks.
    std::atomic<NetIpSecurity*> security_{nullptr};
    RoutingFlowControl flowControl_{};

    std::atomic<uint64_t> lostMessagesSeen_{0};
    std::atomic<uint64_t> lostCountTotal_{0};
    std::atomic<uint64_t> busySeen_{0};
    std::atomic<uint64_t> txDeferred_{0};
};

} // namespace netip
} // namespace knx
