// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file coupler_routing.hpp
 * @brief The KNX coupler routing algorithm (03/03/03 §2.4.2.4) as a pure decision.
 *
 * This header contains no I/O and owns no ports. It answers exactly one
 * question — "what should a coupler do with this frame?" — so the normative
 * algorithm can be tested row by row against the specification tables without
 * standing up two data link layers.
 *
 * @ref TwoPortCoupler applies the answer; this decides it.
 *
 * ## Which configuration governs which direction
 *
 * The `MAIN`/`SUB` property pairs are named after the subnetwork a frame
 * arrives *from*, not the one it leaves towards (03/05/01 §4.4.4, and the
 * conformance rows in §"PID_MAIN_LCGRPCONFIG = XXXXXX11 → Primary to Secondary
 * shall be routed in function of the Filter Table"). So:
 *
 * - `PID_MAIN_LCCONFIG` / `PID_MAIN_LCGRPCONFIG` govern primary → secondary
 * - `PID_SUB_LCCONFIG`  / `PID_SUB_LCGRPCONFIG`  govern secondary → primary
 *
 * The accessors here are therefore indexed by the *origin* port.
 */

#pragma once

#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/network/filter_table.hpp"
#include "knx/types.hpp"

#include <cstdint>

namespace knx {
namespace network {

/**
 * @brief The six routing actions of 03/03/03 §2.4.2.4.
 *
 * Every branch of every routing table in the specification terminates in one
 * of these. Keeping the enumeration faithful — rather than collapsing it to
 * "forward or drop" — is what makes the hop-count and acknowledge behaviour
 * expressible at all.
 */
enum class RoutingAction : uint8_t {
    /// Forward with the hop count left untouched. Hop count 7 means "unlimited"
    /// and must survive the coupler, otherwise a frame that is meant to cross
    /// any number of couplers would be aged as if it were a normal one.
    RouteUnmodified,

    /// Forward with the hop count decremented by one. The ordinary case.
    RouteDecremented,

    /// Forward with the hop count forced to 0, so the next coupler will not
    /// route it any further. Used for system broadcast across a media coupler.
    RouteLast,

    /// Do not forward: the frame is addressed to the coupler itself and belongs
    /// to its own application layer.
    ForwardLocally,

    /// Drop without acknowledging. The frame was not for this side of the
    /// coupler, so the sender must not be told that it arrived anywhere.
    IgnoreTotally,

    /// Drop, but acknowledge at layer 2. The frame *was* meant to be routed but
    /// its hop count is exhausted; acknowledging suppresses pointless
    /// retransmissions of a frame that can never make progress.
    IgnoreAcked,
};

/// True when the action means "put this frame on the other port".
constexpr bool isForwardingAction(RoutingAction action)
{
    return action == RoutingAction::RouteUnmodified || action == RoutingAction::RouteDecremented
           || action == RoutingAction::RouteLast;
}

/**
 * @brief Coupler role, which selects the addressing rules to apply.
 *
 * 03/03/03 §2.4.2.4 derives the role from the coupler's own individual address
 * rather than from configuration, and so does @ref CouplerRoutingPolicy::roleFor.
 * A device whose address does not describe a coupler cannot behave as one, and
 * silently guessing a role for it would route traffic to the wrong subnetwork.
 */
enum class CouplerRole : uint8_t {
    /// Not a coupler address (device part ≠ 0). Forwards everything that has
    /// hop count left, filtering group addresses only. No address-based rules.
    Repeater,

    /// x.y.0 with y ≠ 0. Couples a line to its main line.
    LineCoupler,

    /// x.0.0 with x ≠ 0. Couples a main line to the backbone.
    BackboneCoupler,
};

/**
 * @brief Which side of the coupler a frame arrived on.
 *
 * Primary is the upstream side (main line for a line coupler, backbone for a
 * backbone coupler); secondary is the downstream subnetwork.
 */
enum class CouplerPort : uint8_t {
    Primary = 0,
    Secondary = 1,
};

/// The opposite port.
constexpr CouplerPort otherPort(CouplerPort port)
{
    return port == CouplerPort::Primary ? CouplerPort::Secondary : CouplerPort::Primary;
}

/**
 * @brief How a frame is addressed, which selects the routing table to apply.
 */
enum class FrameClass : uint8_t {
    /// Individually addressed, connectionless or connection-oriented.
    PointToPoint,
    /// Group addressed with a non-zero group address.
    Multicast,
    /// Group address 0x0000.
    Broadcast,
    /**
     * Broadcast reserved for system management.
     *
     * TP1 cannot distinguish this from an ordinary broadcast — the distinction
     * is carried by the medium only where the medium has somewhere to carry it
     * (KNXnet/IP has a separate service type). @ref CouplerRoutingPolicy::classify
     * therefore never returns this; a media coupler that *can* tell the
     * difference passes it to the explicit @ref CouplerRoutingPolicy::decide
     * overload.
     */
    SystemBroadcast,
};

/// PHYS_FRAME field of PID_MAIN_LCCONFIG / PID_SUB_LCCONFIG (03/05/01 §4.4.4).
enum class PhysFrameHandling : uint8_t {
    /// Route every point-to-point frame regardless of address (bridge mode).
    Unlock = 1,
    /// Route no point-to-point frames at all.
    Lock = 2,
    /// Apply the normative address-based algorithm. Default.
    Route = 3,
};

/// PHYS_IACK field of PID_MAIN_LCCONFIG / PID_SUB_LCCONFIG (03/05/01 §4.4.4).
enum class PhysAckHandling : uint8_t {
    /// Acknowledge frames that are routed or addressed to the coupler. Default.
    Normal = 1,
    /// Acknowledge every point-to-point frame, routed or not.
    All = 2,
    /// Negatively acknowledge everything — protects a subnetwork from being
    /// parameterised from the other side.
    Nack = 3,
};

/// GROUP_6FFF / GROUP_7000 fields of PID_*_LCGRPCONFIG (03/05/01 §4.4.5).
enum class GroupHandling : uint8_t {
    /// Route every group frame in the range without consulting the filter table.
    Unlock = 1,
    /// Route none of them.
    Lock = 2,
    /// Consult the filter table.
    Route = 3,
};

/**
 * @brief PID_MAIN_LCCONFIG / PID_SUB_LCCONFIG (03/05/01 §4.4.4).
 *
 * Governs point-to-point and broadcast frames arriving on one side.
 */
struct LineCouplerConfig {
    PhysFrameHandling physFrame{PhysFrameHandling::Route};  ///< bits 0-1
    bool physRepeat{true};                                  ///< bit 2
    bool broadcastLock{false};                              ///< bit 3
    bool broadcastRepeat{true};                             ///< bit 4
    /// bit 5. False acknowledges *every* multicast frame, routed or not, which
    /// exists only to stop misrouted frames being retransmitted.
    bool groupAckRoutedOnly{true};
    PhysAckHandling physAck{PhysAckHandling::Normal};       ///< bits 6-7

    static LineCouplerConfig decode(uint8_t raw);
    uint8_t encode() const;
};

/// The specification's default LCCONFIG byte: PHYS_ROUT, PHYS_REPEAT,
/// broadcasts routed and repeated, GROUP_IACK_ROUT normal, PHYS_IACK normal.
inline constexpr uint8_t kDefaultLineCouplerConfig = 0x77;

/**
 * @brief PID_MAIN_LCGRPCONFIG / PID_SUB_LCGRPCONFIG (03/05/01 §4.4.5).
 *
 * Governs group-addressed frames arriving on one side.
 */
struct GroupCouplerConfig {
    /// bits 0-1. Group addresses ≤ 0x6FFF, i.e. main groups 0..13.
    GroupHandling group6FFF{GroupHandling::Route};
    /**
     * bits 2-3. Group addresses ≥ 0x7000, i.e. main groups 14..31.
     *
     * The default is Unlock, not Route: ETS does not put these into a filter
     * table, so filtering them would silently black-hole them.
     */
    GroupHandling group7000{GroupHandling::Unlock};
    bool groupRepeat{true};  ///< bit 4

    static GroupCouplerConfig decode(uint8_t raw);
    uint8_t encode() const;
};

/// The specification's default LCGRPCONFIG byte: GROUP_ROUT6FFF,
/// GROUP_UNLOCK7000, GROUP_REPEAT.
inline constexpr uint8_t kDefaultGroupCouplerConfig = 0x17;

/// What layer 2 should answer for a frame the coupler has just judged.
enum class AckPolicy : uint8_t {
    Acknowledge,
    NoAcknowledge,
    NegativeAcknowledge,
};

/**
 * @brief The routing algorithm of 03/03/03 §2.4.2.4.
 *
 * Stateless with respect to traffic: the same frame arriving on the same port
 * always yields the same action. That is what makes it testable against the
 * specification tables directly.
 *
 * The filter table is referenced, not owned, because a coupler's filter table
 * is also reachable through the Router Object's PID_ROUTETABLE_CONTROL and
 * both views have to see the same entries.
 */
class CouplerRoutingPolicy {
public:
    CouplerRoutingPolicy() = default;

    /// Derive the coupler role from an individual address per 03/03/03 §2.4.2.4.
    static CouplerRole roleFor(const IndividualAddress& address);

    /**
     * @brief Set the coupler's own individual address.
     *
     * The role is re-derived from it. Every point-to-point routing decision is
     * made relative to this address, so a coupler that has not been given one
     * behaves as a repeater rather than guessing a subnetwork.
     */
    void setOwnAddress(const IndividualAddress& address);
    IndividualAddress ownAddress() const { return _ownAddress; }
    CouplerRole role() const { return _role; }

    /**
     * @brief Override the derived role.
     *
     * Only useful for a device deliberately acting as a repeater on a coupler
     * address, or for tests. Cleared again by @ref setOwnAddress.
     */
    void setRole(CouplerRole role) { _role = role; }

    /// The filter table consulted when GROUP_ROUT is in effect. Not owned.
    void setFilterTable(FilterTable* table) { _filterTable = table; }
    FilterTable* filterTable() const { return _filterTable; }

    /**
     * @brief PID_FILTER_TABLE_USE.
     *
     * When false, GROUP_ROUT degrades to GROUP_UNLOCK rather than blocking
     * everything: a coupler whose filter table has not been downloaded yet must
     * pass traffic, not sever the line.
     */
    void setFilterTableInUse(bool inUse) { _filterTableInUse = inUse; }
    bool isFilterTableInUse() const { return _filterTableInUse; }

    /// Configuration governing frames arriving on @p origin. See the file header.
    LineCouplerConfig& lineConfig(CouplerPort origin) { return _lineConfig[index(origin)]; }
    const LineCouplerConfig& lineConfig(CouplerPort origin) const {
        return _lineConfig[index(origin)];
    }
    GroupCouplerConfig& groupConfig(CouplerPort origin) { return _groupConfig[index(origin)]; }
    const GroupCouplerConfig& groupConfig(CouplerPort origin) const {
        return _groupConfig[index(origin)];
    }

    /// Classify a frame by its addressing. Never returns SystemBroadcast.
    static FrameClass classify(const datalink::LDataFrame& frame);

    /// Decide what to do with @p frame, classifying it from its addressing.
    RoutingAction decide(CouplerPort origin, const datalink::LDataFrame& frame) const;

    /// Decide with an externally determined class, for media that can tell a
    /// system broadcast from an ordinary one.
    RoutingAction decide(CouplerPort origin, const datalink::LDataFrame& frame,
                         FrameClass frameClass) const;

    /**
     * @brief The layer-2 acknowledge to give for a frame that yielded @p action.
     *
     * Separate from the routing decision because the acknowledge fields are
     * explicitly "independent of the Coupler Mode" (03/05/01 §4.4.4): a frame
     * can be dropped and still acknowledged, or routed and not acknowledged.
     */
    AckPolicy ackPolicy(CouplerPort origin, const datalink::LDataFrame& frame,
                        RoutingAction action) const;

    /// Whether a failed transmission of @p frame should be repeated, per the
    /// REPEAT bits of the configuration for the port it is leaving towards.
    bool shouldRepeat(CouplerPort origin, const datalink::LDataFrame& frame) const;

    /// Apply @p action to a frame's hop count. Only valid for forwarding
    /// actions; returns the hop count the outgoing frame must carry.
    static uint8_t applyHopCount(RoutingAction action, uint8_t hopCount);

private:
    static constexpr size_t index(CouplerPort port) { return static_cast<size_t>(port); }

    /// The tail shared by every rule in §2.4.2.4.
    static RoutingAction hopAction(uint8_t hopCount);

    RoutingAction decidePointToPoint(CouplerPort origin,
                                     const datalink::LDataFrame& frame) const;
    RoutingAction decideMulticast(CouplerPort origin, const datalink::LDataFrame& frame) const;
    RoutingAction decideBroadcast(CouplerPort origin, const datalink::LDataFrame& frame) const;
    static RoutingAction decideSystemBroadcast(const datalink::LDataFrame& frame);

    /// The address-based part of PHYS_ROUT, split out per role.
    RoutingAction routePointToPointAsLineCoupler(CouplerPort origin,
                                                 const IndividualAddress& destination,
                                                 uint8_t hopCount) const;
    RoutingAction routePointToPointAsBackboneCoupler(CouplerPort origin,
                                                     const IndividualAddress& destination,
                                                     uint8_t hopCount) const;

    bool groupRoutingCondition(CouplerPort origin, const GroupAddress& destination) const;

    IndividualAddress _ownAddress{};
    CouplerRole _role{CouplerRole::Repeater};
    FilterTable* _filterTable{nullptr};
    bool _filterTableInUse{true};

    LineCouplerConfig _lineConfig[2]{};
    GroupCouplerConfig _groupConfig[2]{};
};

} // namespace network
} // namespace knx
