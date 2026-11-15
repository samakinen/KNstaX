// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/config.hpp"
#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/netip/random_source.hpp"
#include "knx/netip/routing_security.hpp"
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
#include <mutex>
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
        RoutingSecurity* security{nullptr};
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
        util::InplaceFunction<void(std::span<const uint8_t> datagram, IpAddress source, uint16_t sourcePort), ::knx::config::callbackBytes(64)>;

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
     * The context is used under a lock by whichever task is sending or
     * receiving, and this takes the same lock: once it returns, no datagram is
     * still being processed with the previous context, and the caller may
     * destroy it. The caller owns the object.
     */
    void setSecurity(RoutingSecurity* security);
    [[nodiscard]] bool hasSecurity() const;

    /// Randomness for the ROUTING_BUSY back-off (03_08_05 §2.3.5). Without one
    /// the fallback in random_source.hpp is used, never a fixed value.
    void setRandomSource(RandomBytesSource source) { random_ = std::move(source); }

    util::Result<void> sendRoutingIndication(std::span<const uint8_t> cemi);

    /// The system-broadcast domain. Never wrapped, even under Secure Routing.
    util::Result<void> sendRoutingSystemBroadcast(std::span<const uint8_t> cemi);

    util::Result<void> sendRoutingLostMessage(const RoutingLostMessageInfo& info);

    /// Unicast escape hatch for the control services that share this socket.
    util::Result<void> sendDatagram(IpAddress address, NetIpPort port, std::span<const uint8_t> datagram);

    /// Receive and classify one datagram. Control-service datagrams are
    /// dispatched to the handler and reported as `InboundKind::None`, so a
    /// caller draining frames does not have to know they exist.
    ///
    /// Under Secure Routing this is also where the multicast timer is kept in
    /// step: the wait is cut short when a TIMER_NOTIFY falls due, a due one is
    /// sent before returning, and a received one is handed to the context.
    /// A caller that keeps receiving therefore keeps the timer synchronised.
    util::Result<Inbound> receive(std::span<uint8_t> cemiOut, uint32_t timeoutMs);

    /// True while an outbound datagram would breach the egress budget or a
    /// ROUTING_BUSY back-off. The caller decides whether to wait or to drop.
    bool isSendBlocked() const;
    uint32_t millisecondsUntilSendable() const;

    /**
     * Re-issue the IGMP membership for the routing group.
     *
     * The join is attempted at open(), and on a device whose interface is not
     * up yet there is nothing to join on: the endpoint opens anyway, because a
     * KNXnet/IP stack that refuses to start until the network arrives is a
     * device that boots with default parameters instead of its commissioned
     * ones. This is how the membership is completed afterwards — the host calls
     * it when the interface gains an address.
     *
     * Also the repair for an address that changed underneath a membership that
     * was already established (a DHCP renewal into a different subnet, a roam
     * between access points): the group is left and rejoined, so the membership
     * follows the interface rather than pointing at one that no longer exists.
     *
     * Idempotent, and safe to call on an endpoint that is already correctly
     * joined. Returns NotInitialized when the socket is not open.
     */
    util::Result<void> refreshMulticastMembership();

    /// True once the routing group has actually been joined. False between an
    /// open() that found no interface and the refresh that completes it.
    [[nodiscard]] bool multicastJoined() const { return joined_; }

    /**
     * Move the open endpoint to another routing multicast group.
     *
     * Leaves the current group, makes `multicastGroup` the destination of
     * every send from now on, and joins it. A join that fails (no interface)
     * still leaves the new group in force; refreshMulticastMembership()
     * completes it, exactly as after an open() without an interface.
     *
     * Safe against the receive task, which refreshes the membership on its
     * own, and against sends in progress on the stack task, which read the
     * group once per datagram. Returns NotInitialized when the socket is not
     * open and InvalidAddress for 0.0.0.0.
     */
    util::Result<void> setMulticastGroup(IpAddress multicastGroup);

    /// The routing multicast group sends go to; 0.0.0.0 when closed.
    [[nodiscard]] IpAddress multicastGroup() const;

    uint64_t routingLostMessagesSeen() const { return lostMessagesSeen_.load(); }
    uint64_t routingLostCountTotal() const { return lostCountTotal_.load(); }
    uint64_t routingBusySeen() const { return busySeen_.load(); }
    uint64_t txDeferred() const { return txDeferred_.load(); }
    /// SECURE_WRAPPER datagrams discarded because no backbone key is loaded.
    /// Non-zero means a secured installation is talking to an unsecured device.
    uint64_t secureWrappersUnreadable() const { return secureWrappersUnreadable_.load(); }
    /// Datagrams of the routing family that a secured group only accepts
    /// wrapped (ROUTING_INDICATION, _BUSY, _LOST_MESSAGE), dropped because
    /// they arrived in the clear.
    uint64_t plainOnSecuredGroupDropped() const { return plainOnSecuredDropped_.load(); }
    uint64_t timerNotifiesSent() const { return timerNotifiesSent_.load(); }
    uint64_t timerNotifiesAccepted() const { return timerNotifiesAccepted_.load(); }

private:
    util::Result<void> sendCemiCarrier(NetIpServiceType serviceType, std::span<const uint8_t> cemi, bool allowSecure);
    util::Result<void> sendToGroup(std::span<const uint8_t> datagram, bool allowSecure);
    uint32_t nowMs() const;
    /// Send a TIMER_NOTIFY if one has fallen due. Called with securityMutex_ held.
    void sendDueTimerNotify(RoutingSecurity& security);

    std::unique_ptr<platform::UdpSocket> sock_;
    Options options_;
    IpAddress group() const { return IpAddress(multicastRaw_.load()); }

    /// The routing group, as IpAddress::raw. Read by every send (stack task)
    /// and TIMER_NOTIFY (receive task) and moved by setMulticastGroup().
    std::atomic<uint32_t> multicastRaw_{0};
    /// Serialises the join/leave pairs of open(), close(),
    /// refreshMulticastMembership() and setMulticastGroup(), which run on the
    /// stack task and, for the idle-time refresh, on the receive task. Guards
    /// joined_ and interfaceAddr_ for the same reason.
    mutable std::mutex membershipMutex_;
    IpAddress interfaceAddr_{IpAddress(0)};
    IpAddress localAddr_{IpAddress(0)};
    /// Kept so a membership refresh can re-read the address the interface holds
    /// now. The caller owns it and outlives the endpoint.
    platform::NetworkInterface* network_{nullptr};
    std::atomic<bool> joined_{false};

    DatagramBuffer<config::kUdpBufferSize> rxBuffer_{};
    DatagramBuffer<config::kUdpBufferSize> txBuffer_{};
    DatagramBuffer<config::kUdpBufferSize> securePlainBuffer_{};
    SecureDatagramBuffer<config::kUdpBufferSize, ip_secure::SecureWrapper::kOverhead> secureWrapBuffer_{};
    platform::TimingPlatform* timingPlatform_{nullptr};

    ControlDatagramHandler controlHandler_{};
    /// Read on both the send and receive paths, which are different tasks;
    /// held for as long as a datagram is processed with the context it points to.
    mutable std::mutex securityMutex_;
    RoutingSecurity* security_{nullptr};
    RandomBytesSource random_{};
    /// The send path (stack task) checks and records sends; the receive task
    /// records ROUTING_BUSY. Both go through flowMutex_.
    mutable std::mutex flowMutex_;
    RoutingFlowControl flowControl_{};

    std::atomic<uint64_t> lostMessagesSeen_{0};
    std::atomic<uint64_t> lostCountTotal_{0};
    std::atomic<uint64_t> busySeen_{0};
    std::atomic<uint64_t> txDeferred_{0};
    std::atomic<uint64_t> secureWrappersUnreadable_{0};
    std::atomic<uint64_t> plainOnSecuredDropped_{0};
    std::atomic<uint64_t> timerNotifiesSent_{0};
    std::atomic<uint64_t> timerNotifiesAccepted_{0};
    /// Throttle for the warning above. Touched only on the receive task.
    uint32_t secureDropLoggedMs_{0};
    bool secureDropEverLogged_{false};
};

} // namespace netip
} // namespace knx
