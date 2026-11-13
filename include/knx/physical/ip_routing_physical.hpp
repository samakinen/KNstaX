// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file ip_routing_physical.hpp
 * @brief TP1 physical layer adapter backed by KNXnet/IP Routing (multicast)
 */

#pragma once

#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/netip/core_discovery_codec.hpp"
#include "knx/netip/core_server_responder.hpp"
#include "knx/netip/random_source.hpp"
#include "knx/netip/routing_flow_control.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/types.hpp"

#include <atomic>
#include <cstdint>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <span>
#include <vector>

#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

namespace knx {
namespace netip {
class RoutingEndpoint;
namespace ip_secure {
class McTimerStore;
class SecureRoutingSecurity;
}
}
namespace physical {

class IpRoutingPhysical {
public:
    using ProgressState = util::OperationProgressState;

    IpRoutingPhysical();
    ~IpRoutingPhysical();

    IpRoutingPhysical(const IpRoutingPhysical&) = delete;
    IpRoutingPhysical& operator=(const IpRoutingPhysical&) = delete;

    // Must be called before init().
    void setMulticast(IpAddress multicastGroup,
                      NetIpPort port,
                      IpAddress interfaceAddress = IpAddress(0));

    // Must be called before init(). Required for cross-platform builds.
    void setNetworkInterface(platform::NetworkInterface* network);
    void setTimingPlatform(platform::TimingPlatform* timingPlatform);

    // Optional multicast socket behavior; must be set before init().
    void setMulticastSocketOptions(uint8_t ttl, Toggle loopback);

    /**
     * Answer KNXnet/IP discovery and self-description on this socket.
     *
     * Core is mandatory for a KNXnet/IP device (03/08/01 Table 2), and a device
     * that does not answer it is one ETS cannot list even though it is sitting
     * on the multicast group. The provider is called per request rather than
     * sampled once: programming mode is the field that decides whether a
     * "select by programming mode" search finds this device, and it changes
     * while the device runs.
     *
     * Must be called before init().
     */
    void setCoreDiscovery(netip::CoreServerResponder::DescriptionProvider provider,
                          std::span<const netip::ServiceFamilySupport> serviceFamilies);

    /**
     * Source of randomness for the ROUTING_BUSY back-off (03_08_05 §2.3.5) and,
     * under Secure Routing, for message tags and TIMER_NOTIFY delays.
     *
     * The product passes the platform RNG. Without it a clock-seeded fallback
     * is used (see random_source.hpp), so the back-off is never the fixed tw
     * that resumes every device on the group in the same millisecond.
     * Must be called before init().
     */
    void setRandomSource(netip::RandomBytesSource source);

#if KNX_SECURE_ENABLED
    /// How a backbone key reached applySecureRouting(); decides when the first
    /// TIMER_NOTIFY goes out (03_08_09 §2.2.2.3.2.8).
    enum class KeySource : uint8_t {
        /// Downloaded now, by ETS: a new domain (E11), notify at once.
        Download,
        /// Restored from storage at start-up: a power-up, notify after
        /// random(0, 10 s) so a site's worth of devices do not all notify at once.
        Restored,
    };

    /// The device's KNX serial number, which goes into every SECURE_WRAPPER and
    /// own TIMER_NOTIFY. Set before the first key is applied: a context built
    /// without it shares its nonce space with every device that also left it
    /// at zero.
    void setSecureRoutingSerial(const std::array<uint8_t, 6>& serial);

    /// Where the multicast timer is persisted (03_08_09 §2.2.4.2). The store
    /// must outlive this object. Set before the first key is applied.
    void setMcTimerStore(netip::ip_secure::McTimerStore* store);

    /// PID_MULTICAST_LATENCY_TOLERANCE (ms) and PID_SYNC_LATENCY_FRACTION
    /// (PDT_SCALING), as the product reads them from the KNXnet/IP Parameter
    /// Object.
    struct LatencySettings {
        uint16_t latencyToleranceMs{2000};
        uint8_t syncLatencyFraction{0x1A};
        bool operator==(const LatencySettings&) const = default;
    };
    using LatencySettingsSource = util::InplaceFunction<LatencySettings(), 32>;

    /**
     * Ask `source` for the latency settings once a second on the receive path.
     *
     * ETS writes them as plain properties, and §2.3.1.6.3 gives the device one
     * second to act on a new value — so asking once a second meets the
     * contract without a write hook in the property layer for two rarely
     * written values. Must be called before init().
     */
    void setLatencySettingsSource(LatencySettingsSource source);

    /**
     * Turn Secure Routing on, off, or over to a different key while running.
     *
     * ETS downloads the backbone key as an ordinary property write and expects
     * it to take effect at once — a device that only honours a key supplied
     * before init() would need a restart it is never told to perform, and in
     * the meantime multicasts in the clear inside a project that believes it is
     * encrypted.
     *
     * An all-zero key means "no secure routing", which is how ETS turns it off.
     * A different key is a new domain: the multicast timer restarts from 0
     * (03_08_09 §2.2.2.2.2, E11). The key already in use, written again, is not
     * an event and changes nothing — which is what keeps a reboot, or ETS
     * repeating a download, from resetting the timer.
     */
    util::Result<void> applySecureRouting(std::span<const uint8_t> groupKey,
                                          KeySource source = KeySource::Download);

    bool isSecureRoutingActive() const;

    /**
     * Restart the multicast timer synchronisation for the key in force (A7).
     *
     * 03_08_09 §2.2.2.3.2.5 E11 is "PID_BACKBONE_KEY and/or
     * PID_MULTICAST_ADDRESS is set to a new and different value", and the
     * transition table answers E11 with A7 from either state. A new key is
     * handled by applySecureRouting(); this is the address half, for a key
     * that stays the same. The synchronisation restarts in SCHED_PERIODIC
     * with notify_timer = 0, and mc_timer carries on from its current value:
     * it is at least the persisted one §2.2.2.3.2.8 restarts from, and it is
     * valid for this key. No-op without Secure Routing.
     */
    void restartMulticastTimerSync();

    /// Current mc_timer, or 0 without Secure Routing. Diagnostic.
    uint64_t secureRoutingTimer() const;
#endif

    /// Inbound frames held between the socket and the data link layer. A fixed
    /// ring, so the depth is bounded by this; 03_08_05 §2.3.5 recommends room
    /// for at least 30.
    static constexpr size_t kRxQueueCapacity = 32;
    /// Largest encoded TP1 frame a slot holds: an L_Data_Extended frame with a
    /// full APDU, as FrameCodec produces it.
    static constexpr size_t kRxSlotSize = 264;

    // Bounds memory under multicast storms. Clamped to [1, kRxQueueCapacity].
    void setMaxRxQueueDepth(size_t maxFrames);

    util::Result<void> init();
    void close();
    bool isOpen() const;

    /**
     * Re-issue the routing group membership on the open socket.
     *
     * init() does not fail when the join does — see
     * netip::RoutingEndpoint::refreshMulticastMembership() — so a device that
     * started before its interface was up runs with a socket that is bound but
     * deaf to the group. The host calls this when the interface gains or
     * changes an address; until it does, that device hears nothing.
     *
     * Returns NotInitialized when the layer is not open.
     */
    util::Result<void> refreshMulticastMembership();

    /// True once the routing group has been joined. A device that is open but
    /// not joined can transmit and cannot receive.
    [[nodiscard]] bool multicastJoined() const;

    /**
     * Use a different routing multicast group, before or after init().
     *
     * Before init() this only replaces the group setMulticast() gave. After
     * it, the open endpoint leaves the old group, sends to the new one from
     * then on, and joins it (see netip::RoutingEndpoint::setMulticastGroup()).
     * The product calls it at start-up when the KNXnet/IP Parameter Object
     * holds a group ETS downloaded, which is only known once the stored
     * properties are back — after the socket has opened.
     */
    util::Result<void> setMulticastGroup(IpAddress multicastGroup);

    /// The routing multicast group in force.
    [[nodiscard]] IpAddress multicastGroup() const;

    /**
     * @brief Counters for the routing socket
     *
     * Exists to answer the one question a capture taken anywhere else cannot:
     * when a device stops answering, did the telegram never arrive, or did it
     * arrive and get dropped in here? `datagramsReceived` standing still while
     * a client is plainly transmitting is the first case, and no amount of
     * stack work fixes it.
     */
    struct LinkStatistics {
        /// Datagrams read off the socket, whatever they turned out to contain.
        uint64_t datagramsReceived{0};
        /// Datagrams discarded because the inbound queue was already full.
        uint64_t datagramsDropped{0};
        /// Group memberships re-issued after a long silence; see
        /// refreshMulticastMembership().
        uint64_t membershipRefreshes{0};
        /// Milliseconds since the last datagram. Grows without bound on a
        /// device that has gone deaf.
        uint32_t msSinceLastInbound{0};
    };

    [[nodiscard]] LinkStatistics linkStatistics() const;

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame);
    util::Result<uint32_t> beginTransmit(std::span<const uint8_t> frame);
    util::Result<ProgressState> pollTransmit(uint32_t sequence);
    /**
     * @brief This port waits for the medium inside beginReceive()/pollReceive()
     *
     * Read by the data link layer to decide whether its receive task should
     * sleep in the socket or poll on an interval. Declared as a member so the
     * capability is detected by its presence, the same way the queue-notify
     * hook is, rather than by a list of media kept somewhere else.
     */
    [[nodiscard]] static constexpr bool receiveWaitsForMedium() { return true; }

    util::Result<void> beginReceive(uint32_t timeoutMs);
    util::Result<ProgressState> pollReceive();
    util::Result<std::span<const uint8_t>> receivedFrameView();
    void setReceiveCallback(ReceiveCallback callback, void* context);
    PhysicalLayerState getState() const;
    util::Result<void> setBusMonitorMode(Toggle mode);

private:
    /**
     * Read what the socket already holds into the inbound queue.
     *
     * Waits up to `timeoutMs` for the first datagram, then keeps reading
     * without waiting until the socket is empty or `kMaxDrainPerPoll` is
     * reached.
     *
     * One datagram per call also emptied the socket, but only a whole
     * begin/poll round trip through the layer above at a time, and only while
     * that layer is running a receive pass. Between passes the socket buffer is
     * the only queue holding arrivals, and it is the one queue on this device
     * whose depth is the host's to decide — whatever overflows it is gone
     * before any KNX layer sees it. Taking everything that has arrived in one
     * go keeps that window as short as the medium allows.
     */
    void drainInbound(uint32_t timeoutMs);

    /// Bound on one drain so a multicast storm cannot hold the caller inside
    /// the socket. Comfortably above the burst a management client sends —
    /// ETS opens a connection with T_Connect and the first request back to
    /// back — and anything beyond it is taken by the next poll.
    static constexpr size_t kMaxDrainPerPoll = 16;
    void enqueueFrame(std::span<const uint8_t> cemi);
#if KNX_SECURE_ENABLED
    /// Pull the latency settings from the product at most once a second.
    void refreshLatencySettings();
    /// Build the context for the stored key and install it on the endpoint.
    /// Called with secureRoutingMutex_ held and an open endpoint.
    void installSecureRoutingContext(KeySource source);
#endif

    /// Re-join the group when nothing has arrived for a long time. See the
    /// definition for why silence, and not a plain interval, is the trigger.
    void refreshMembershipIfDeaf();

    platform::NetworkInterface* network_;

    /// IpAddress::raw of the group in force. Written by setMulticast() and
    /// setMulticastGroup() on the stack task, read by multicastGroup() from
    /// the discovery responder on the receive task.
    std::atomic<uint32_t> multicastGroup_;
    NetIpPort port_;
    IpAddress interfaceAddress_;

    uint8_t multicastTtl_;
    bool multicastLoopback_;

#if KNX_SECURE_ENABLED
    bool secureRoutingEnabled_{false};
    std::array<uint8_t, 16> secureRoutingGroupKey_{};
    std::array<uint8_t, 6> secureRoutingSerial_{};
    /// How the stored key arrived, for the context init() builds from it.
    KeySource pendingKeySource_{KeySource::Restored};
    netip::ip_secure::McTimerStore* mcTimerStore_{nullptr};
    LatencySettings latencySettings_{};
    LatencySettingsSource latencySettingsSource_{};
    /// Touched only on the receive path.
    uint32_t latencyPolledMs_{0};
    bool latencyPolled_{false};
#endif

    std::atomic<size_t> maxRxQueueDepth_;
    std::atomic<uint64_t> rxDropped_;
    std::atomic<uint64_t> rxDatagrams_{0};
    std::atomic<uint64_t> membershipRefreshes_{0};

    /// Both touched only from the receive path, which is single-threaded.
    uint32_t lastInboundMs_{0};
    bool lastInboundValid_{false};

    std::unique_ptr<netip::RoutingEndpoint> endpoint_;

#if KNX_SECURE_ENABLED
    /// The context for the key in force. A superseded one is destroyed as soon
    /// as RoutingEndpoint::setSecurity() has returned: the endpoint uses its
    /// context under the lock that call takes, so no datagram still in flight
    /// can be pointing at it. Nothing is retired, so nothing accumulates.
    std::unique_ptr<netip::ip_secure::SecureRoutingSecurity> routingSecurity_;
    mutable std::mutex secureRoutingMutex_;
#endif

    std::atomic<bool> initialized_;

    struct RxSlot {
        std::array<uint8_t, kRxSlotSize> bytes{};
        size_t length{0};
    };

    mutable std::mutex rxMutex_;
    /// Fixed ring, allocated once at init(): an inbound datagram costs a copy
    /// into a slot rather than a heap allocation.
    std::unique_ptr<std::array<RxSlot, kRxQueueCapacity>> rxRing_;
    size_t rxHead_{0};
    size_t rxCount_{0};
    RxSlot lastReceivedFrame_{};
    std::vector<uint8_t> pendingTxFrame_;
    bool txActive_{false};
    uint32_t txSequence_{0};
    ProgressState txState_{ProgressState::Success};
    bool rxActive_{false};
    uint64_t rxDeadlineMs_{0};
    platform::TimingPlatform* timingPlatform_{nullptr};

    ReceiveCallback rxCb_;
    void* rxCtx_;

    netip::CoreServerResponder coreResponder_;
    bool coreDiscoveryEnabled_{false};
    netip::RandomBytesSource randomSource_{};

    std::atomic<PhysicalLayerState> state_;
};

} // namespace physical
} // namespace knx
