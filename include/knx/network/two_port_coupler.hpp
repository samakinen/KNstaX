// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file two_port_coupler.hpp
 * @brief A KNX line or backbone coupler bridging two L_Data interfaces.
 *
 * The routing decision itself lives in @ref CouplerRoutingPolicy, which
 * implements 03/03/03 §2.4.2.4 as a pure function. This class owns the ports,
 * the echo suppression and the filter table, and applies what the policy
 * decides.
 */

#pragma once

#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/network/coupler_routing.hpp"
#include "knx/network/filter_table.hpp"
#include "knx/network/routing_table.hpp"
#include "knx/types.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace knx {
namespace network {

class TwoPortCoupler {
public:
    /// Which side of the coupler. Primary is upstream (main line for a line
    /// coupler, backbone for a backbone coupler); secondary is the subnetwork
    /// below it.
    using Port = CouplerPort;

    /**
     * @param primary   the upstream interface
     * @param secondary the downstream interface
     */
    TwoPortCoupler(datalink::Tp1DataLinkLayer& primary, datalink::Tp1DataLinkLayer& secondary);
    ~TwoPortCoupler();

    TwoPortCoupler(const TwoPortCoupler&) = delete;
    TwoPortCoupler& operator=(const TwoPortCoupler&) = delete;

    util::Result<void> init();
    void close();

    /**
     * @brief Set the coupler's own individual address.
     *
     * This is what makes the coupler a coupler: every point-to-point routing
     * decision is made relative to it, and the role (line or backbone) is
     * derived from it. Without one the coupler falls back to repeater
     * behaviour, forwarding group traffic but making no address-based decision.
     *
     * Also registers the address with both data link layers so each side
     * acknowledges frames addressed to the coupler itself.
     */
    util::Result<void> setOwnAddress(const IndividualAddress& address);
    IndividualAddress ownAddress() const { return policy_.ownAddress(); }
    CouplerRole role() const { return policy_.role(); }

    /// The routing policy, for configuring LCCONFIG / LCGRPCONFIG behaviour.
    CouplerRoutingPolicy& policy() { return policy_; }
    const CouplerRoutingPolicy& policy() const { return policy_; }

    /// Master switch. When off, nothing is forwarded in either direction;
    /// frames addressed to the coupler are still delivered locally, so a
    /// disabled coupler stays reachable from ETS.
    void setRoutingEnabled(Toggle enabled) { routingEnabled_.store(isEnabled(enabled)); }
    bool isRoutingEnabled() const { return routingEnabled_.load(); }

    /// PID_FILTER_TABLE_USE. When off, GROUP_ROUT passes everything rather than
    /// blocking it — see @ref CouplerRoutingPolicy::setFilterTableInUse.
    void setFilteringEnabled(Toggle enabled) { policy_.setFilterTableInUse(isEnabled(enabled)); }
    bool isFilteringEnabled() const { return policy_.isFilterTableInUse(); }

    RoutingTable& routingTable() { return routingTable_; }
    FilterTable& filterTable() { return filterTable_; }

    // -------------------------------------------------------------------------
    // Observability callbacks (optional, called on the receive thread)
    // -------------------------------------------------------------------------

    /// Observability callback signature. Inline-stored, so a capturing lambda
    /// larger than the capacity is a compile error rather than a silent heap
    /// allocation on the receive path.
    using PortCallback = util::InplaceFunction<void(Port origin), 32>;
    using FrameCallback = util::InplaceFunction<void(Port origin,
                                                     const datalink::LDataFrame& frame), 32>;
    using AckCallback = util::InplaceFunction<void(Port origin, AckPolicy policy), 32>;

    /// Called each time a frame is forwarded. @p origin is the source port.
    void setFrameForwardedCallback(PortCallback cb) { onFrameForwarded_ = std::move(cb); }

    /// Called when a frame is not forwarded because the routing condition was
    /// false — the filter table, a LOCK setting, or a destination on the wrong
    /// side. Corresponds to IGNORE_TOTALLY.
    void setFrameFilteredCallback(PortCallback cb) { onFrameFiltered_ = std::move(cb); }

    /// Called when a frame that *would* have been routed is dropped because its
    /// hop count is exhausted. Corresponds to IGNORE_ACKED. Distinct from the
    /// filtered callback: this one means a telegram is failing to reach its
    /// destination, which is a topology problem worth surfacing.
    void setFrameDroppedCallback(PortCallback cb) { onFrameDropped_ = std::move(cb); }

    /**
     * @brief Called for every frame this device's own stack may need to see.
     *
     * A coupler is also a device: ETS talks to its application layer for its
     * own commissioning and for its Router Object, and it may carry group
     * objects of its own. This fires for
     *
     * - frames individually addressed to the coupler (FORWARD_LOCALLY), and
     * - every group-addressed and broadcast frame, whatever the routing
     *   decision was.
     *
     * The second case matters: routing and local reception are independent.
     * A group telegram can be blocked by the filter table and still be one this
     * device subscribes to, so the receiver — not the coupler — decides local
     * relevance, using its own address table.
     */
    void setLocalDeliveryCallback(FrameCallback cb) { onLocalDelivery_ = std::move(cb); }

    /**
     * @brief Called with the layer-2 acknowledge the specification requires.
     *
     * Exposed because the TP1 backends decide L_ACK inside the receive ISR,
     * from a published address table, and cannot consult a per-frame decision
     * made up here. A backend that *can* — a TPUART in a mode that defers the
     * acknowledge, or a test harness — can wire this to get the full
     * GROUP_IACK_ROUT / PHYS_IACK behaviour. Purely informational otherwise.
     */
    void setAckPolicyCallback(AckCallback cb) { onAckPolicy_ = std::move(cb); }

private:
    datalink::Tp1DataLinkLayer& primary_;
    datalink::Tp1DataLinkLayer& secondary_;

    std::atomic<bool> initialized_;
    std::atomic<bool> routingEnabled_;

    CouplerRoutingPolicy policy_;
    RoutingTable routingTable_;
    FilterTable filterTable_;

    PortCallback onFrameForwarded_;
    PortCallback onFrameFiltered_;
    PortCallback onFrameDropped_;
    FrameCallback onLocalDelivery_;
    AckCallback onAckPolicy_;

    /// How many recently forwarded frames are remembered per port for echo
    /// suppression. Small on purpose: an echo arrives within a frame time or
    /// two, so a longer memory would only risk suppressing a genuine repeat of
    /// the same telegram.
    static constexpr size_t kRecentHashCapacity = 16;

    /// Fixed-capacity ring. A std::deque here would put a heap allocation on
    /// the receive path of every forwarded frame, on the medium least able to
    /// afford one.
    struct RecentHashes {
        std::mutex mutex;
        std::array<uint64_t, kRecentHashCapacity> values{};
        size_t count{0};
        size_t next{0};

        bool contains(uint64_t h) const {
            for (size_t i = 0; i < count; ++i) {
                if (values[i] == h) return true;
            }
            return false;
        }

        void remember(uint64_t h) {
            values[next] = h;
            next = (next + 1) % kRecentHashCapacity;
            if (count < kRecentHashCapacity) ++count;
        }
    };

    RecentHashes recentPrimary_;
    RecentHashes recentSecondary_;

    datalink::Tp1DataLinkLayer& linkFor(Port port);
    RecentHashes& recentFor(Port port);

    static uint64_t hashFrame(const datalink::LDataFrame& frame);

    bool shouldDropDueToRecent(Port port, uint64_t h);
    void rememberSentTo(Port port, uint64_t h);

    void handleRx(Port origin, const datalink::LDataFrame& frame);
    void applyAction(Port origin, const datalink::LDataFrame& frame, RoutingAction action);
    void forward(Port origin, const datalink::LDataFrame& frame, RoutingAction action);
};

} // namespace network
} // namespace knx
