// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/network/coupler_routing.hpp"

namespace knx {
namespace network {

namespace {

/// Hop count 7 is the specification's "unlimited" marker, not a count.
constexpr uint8_t kHopCountUnlimited = 7;

/// A coupler always carries device part 0; a non-zero device part in a
/// destination therefore means "some device on that subnetwork", and a zero
/// one means "the coupler itself".
constexpr uint8_t kCouplerDevicePart = 0x00;

uint8_t subnetworkOf(const IndividualAddress& address)
{
    return static_cast<uint8_t>(address.raw >> 8);
}

/// Line and device parts together — "SD" in the specification's notation.
uint16_t lineAndDeviceOf(const IndividualAddress& address)
{
    return static_cast<uint16_t>(address.raw & 0x0FFF);
}

} // namespace

// -----------------------------------------------------------------------------
// Configuration byte encodings (03/05/01 §4.4.4, §4.4.5)
// -----------------------------------------------------------------------------

LineCouplerConfig LineCouplerConfig::decode(uint8_t raw)
{
    LineCouplerConfig config;

    // Encoding 0 is "not used" for both two-bit fields. A property write that
    // lands there is treated as the default rather than rejected: the coupler
    // still has to forward something, and the safe something is normal
    // operation.
    const auto physFrame = static_cast<uint8_t>(raw & 0x03);
    config.physFrame = physFrame == 0 ? PhysFrameHandling::Route
                                      : static_cast<PhysFrameHandling>(physFrame);

    config.physRepeat = (raw & 0x04) != 0;
    config.broadcastLock = (raw & 0x08) != 0;
    config.broadcastRepeat = (raw & 0x10) != 0;
    config.groupAckRoutedOnly = (raw & 0x20) != 0;

    const auto physAck = static_cast<uint8_t>((raw >> 6) & 0x03);
    config.physAck = physAck == 0 ? PhysAckHandling::Normal : static_cast<PhysAckHandling>(physAck);

    return config;
}

uint8_t LineCouplerConfig::encode() const
{
    uint8_t raw = static_cast<uint8_t>(static_cast<uint8_t>(physFrame) & 0x03);
    if (physRepeat) raw |= 0x04;
    if (broadcastLock) raw |= 0x08;
    if (broadcastRepeat) raw |= 0x10;
    if (groupAckRoutedOnly) raw |= 0x20;
    raw |= static_cast<uint8_t>((static_cast<uint8_t>(physAck) & 0x03) << 6);
    return raw;
}

GroupCouplerConfig GroupCouplerConfig::decode(uint8_t raw)
{
    GroupCouplerConfig config;

    const auto group6FFF = static_cast<uint8_t>(raw & 0x03);
    config.group6FFF =
        group6FFF == 0 ? GroupHandling::Route : static_cast<GroupHandling>(group6FFF);

    // The two ranges have different defaults: addresses above 0x6FFF are not
    // put into a filter table by ETS, so their fallback is Unlock.
    const auto group7000 = static_cast<uint8_t>((raw >> 2) & 0x03);
    config.group7000 =
        group7000 == 0 ? GroupHandling::Unlock : static_cast<GroupHandling>(group7000);

    config.groupRepeat = (raw & 0x10) != 0;
    return config;
}

uint8_t GroupCouplerConfig::encode() const
{
    uint8_t raw = static_cast<uint8_t>(static_cast<uint8_t>(group6FFF) & 0x03);
    raw |= static_cast<uint8_t>((static_cast<uint8_t>(group7000) & 0x03) << 2);
    if (groupRepeat) raw |= 0x10;
    return raw;
}

// -----------------------------------------------------------------------------
// Role and configuration
// -----------------------------------------------------------------------------

CouplerRole CouplerRoutingPolicy::roleFor(const IndividualAddress& address)
{
    // 03/03/03 §2.4.2.4: a coupler's own address has device part 0. A backbone
    // coupler additionally has line part 0 (x.0.0), a line coupler does not
    // (x.y.0). Anything else is not a coupler address.
    if (address.device() != kCouplerDevicePart || address.area() == 0) {
        return CouplerRole::Repeater;
    }
    return address.line() == 0 ? CouplerRole::BackboneCoupler : CouplerRole::LineCoupler;
}

void CouplerRoutingPolicy::setOwnAddress(const IndividualAddress& address)
{
    _ownAddress = address;
    _role = roleFor(address);
}

// -----------------------------------------------------------------------------
// Classification and dispatch
// -----------------------------------------------------------------------------

FrameClass CouplerRoutingPolicy::classify(const datalink::LDataFrame& frame)
{
    if (!isGroupAddress(frame.destinationType)) {
        return FrameClass::PointToPoint;
    }
    // Group address 0x0000 is the broadcast address; TP1 gives no way to tell a
    // system broadcast from an ordinary one, so this never reports the former.
    return isGroupBroadcastAddress(frame.destination) ? FrameClass::Broadcast
                                                      : FrameClass::Multicast;
}

RoutingAction CouplerRoutingPolicy::decide(CouplerPort origin,
                                           const datalink::LDataFrame& frame) const
{
    return decide(origin, frame, classify(frame));
}

RoutingAction CouplerRoutingPolicy::decide(CouplerPort origin,
                                           const datalink::LDataFrame& frame,
                                           FrameClass frameClass) const
{
    switch (frameClass) {
        case FrameClass::PointToPoint:
            return decidePointToPoint(origin, frame);
        case FrameClass::Multicast:
            return decideMulticast(origin, frame);
        case FrameClass::Broadcast:
            return decideBroadcast(origin, frame);
        case FrameClass::SystemBroadcast:
            return decideSystemBroadcast(frame);
    }
    return RoutingAction::IgnoreTotally;
}

RoutingAction CouplerRoutingPolicy::hopAction(uint8_t hopCount)
{
    // The tail of every rule in §2.4.2.4:
    //   C = 7      → ROUTE_UNMODIFIED   (unlimited; must not be aged)
    //   0 < C < 7  → ROUTE_DECREMENTED
    //   C = 0      → IGNORE_ACKED       (exhausted, but it was meant for us)
    if (hopCount >= kHopCountUnlimited) return RoutingAction::RouteUnmodified;
    if (hopCount > 0) return RoutingAction::RouteDecremented;
    return RoutingAction::IgnoreAcked;
}

uint8_t CouplerRoutingPolicy::applyHopCount(RoutingAction action, uint8_t hopCount)
{
    switch (action) {
        case RoutingAction::RouteUnmodified:
            return hopCount;
        case RoutingAction::RouteDecremented:
            return hopCount > 0 ? static_cast<uint8_t>(hopCount - 1) : 0;
        case RoutingAction::RouteLast:
            return 0;
        default:
            return hopCount;
    }
}

// -----------------------------------------------------------------------------
// Point-to-point (individually addressed)
// -----------------------------------------------------------------------------

RoutingAction CouplerRoutingPolicy::decidePointToPoint(CouplerPort origin,
                                                       const datalink::LDataFrame& frame) const
{
    const IndividualAddress destination{frame.destination.raw};

    // A frame addressed to the coupler itself belongs to the coupler's own
    // application layer whatever the PHYS_FRAME setting says: PHYS_LOCK stops
    // the coupler *routing*, it does not stop it *being a device*. Losing this
    // would make a locked coupler unreachable by ETS, and therefore
    // unrecoverable.
    if (_ownAddress.isValid() && destination.raw == _ownAddress.raw) {
        return RoutingAction::ForwardLocally;
    }

    switch (lineConfig(origin).physFrame) {
        case PhysFrameHandling::Unlock:
            return hopAction(frame.hopCount);
        case PhysFrameHandling::Lock:
            return RoutingAction::IgnoreTotally;
        case PhysFrameHandling::Route:
            break;
    }

    switch (_role) {
        case CouplerRole::LineCoupler:
            return routePointToPointAsLineCoupler(origin, destination, frame.hopCount);
        case CouplerRole::BackboneCoupler:
            return routePointToPointAsBackboneCoupler(origin, destination, frame.hopCount);
        case CouplerRole::Repeater:
            // No subnetwork of its own to compare against, so there is no
            // address-based decision to make: pass anything with hop count left.
            return hopAction(frame.hopCount);
    }
    return RoutingAction::IgnoreTotally;
}

RoutingAction CouplerRoutingPolicy::routePointToPointAsLineCoupler(
    CouplerPort origin, const IndividualAddress& destination, uint8_t hopCount) const
{
    // 03/03/03 §2.4.2.4.2. ZS is the high octet (area + line), D the low octet.
    const uint8_t ownSubnetwork = subnetworkOf(_ownAddress);
    const uint8_t destinationSubnetwork = subnetworkOf(destination);
    const uint8_t destinationDevice = destination.device();

    if (origin == CouplerPort::Primary) {
        // §2.4.2.4.2.1, main line → sub line. Only traffic for our own
        // subnetwork goes down; everything else is for someone else's line and
        // must not be acknowledged here.
        if (destinationSubnetwork != ownSubnetwork) {
            return RoutingAction::IgnoreTotally;
        }
        if (destinationDevice == kCouplerDevicePart) {
            return RoutingAction::ForwardLocally;
        }
        return hopAction(hopCount);
    }

    // §2.4.2.4.2.2, sub line → main line. Anything not for our own subnetwork
    // is leaving, and anything for the coupler's own address is ours.
    if (destinationSubnetwork != ownSubnetwork) {
        return hopAction(hopCount);
    }
    if (destinationDevice == kCouplerDevicePart) {
        return RoutingAction::ForwardLocally;
    }
    // Same subnetwork, real device: sender and receiver are both on the sub
    // line, so the frame has already arrived. Forwarding it would echo it onto
    // the main line for no reason.
    return RoutingAction::IgnoreTotally;
}

RoutingAction CouplerRoutingPolicy::routePointToPointAsBackboneCoupler(
    CouplerPort origin, const IndividualAddress& destination, uint8_t hopCount) const
{
    // 03/03/03 §2.4.2.4.3. Z is the area nibble, SD the line + device parts.
    const uint8_t ownArea = _ownAddress.area();
    const uint8_t destinationArea = destination.area();
    const uint16_t destinationLineAndDevice = lineAndDeviceOf(destination);

    if (origin == CouplerPort::Primary) {
        // §2.4.2.4.3.1, backbone → main line.
        if (destinationArea != ownArea) {
            return RoutingAction::IgnoreTotally;
        }
        if (destinationLineAndDevice == 0) {
            return RoutingAction::ForwardLocally;
        }
        return hopAction(hopCount);
    }

    // §2.4.2.4.3.2, main line → backbone.
    if (destinationArea != ownArea) {
        return hopAction(hopCount);
    }
    if (destinationLineAndDevice == 0) {
        return RoutingAction::ForwardLocally;
    }
    return RoutingAction::IgnoreTotally;
}

// -----------------------------------------------------------------------------
// Multicast (group addressed)
// -----------------------------------------------------------------------------

bool CouplerRoutingPolicy::groupRoutingCondition(CouplerPort origin,
                                                 const GroupAddress& destination) const
{
    const GroupCouplerConfig& config = groupConfig(origin);

    // 03/05/01 §4.4.5 splits the group address space at 0x6FFF because ETS only
    // ever downloads filter table entries for the lower range.
    const GroupHandling handling =
        destination.raw <= 0x6FFFu ? config.group6FFF : config.group7000;

    switch (handling) {
        case GroupHandling::Unlock:
            return true;
        case GroupHandling::Lock:
            return false;
        case GroupHandling::Route:
            break;
    }

    // PID_FILTER_TABLE_USE = 0, or no table wired up at all. Routing everything
    // is the only safe fallback: a coupler that has not been given a filter
    // table yet must not cut the line in half.
    if (!_filterTableInUse || _filterTable == nullptr) {
        return true;
    }
    return _filterTable->checkFilter(destination) == FilterAction::Allow;
}

RoutingAction CouplerRoutingPolicy::decideMulticast(CouplerPort origin,
                                                    const datalink::LDataFrame& frame) const
{
    // §2.4.2.4.1: the routing condition gates everything. A frame the filter
    // table rejects is ignored totally regardless of hop count — it was never
    // meant to cross this coupler, so acknowledging it would be a lie.
    if (!groupRoutingCondition(origin, frame.destination)) {
        return RoutingAction::IgnoreTotally;
    }
    return hopAction(frame.hopCount);
}

// -----------------------------------------------------------------------------
// Broadcast
// -----------------------------------------------------------------------------

RoutingAction CouplerRoutingPolicy::decideBroadcast(CouplerPort origin,
                                                    const datalink::LDataFrame& frame) const
{
    // BROADCAST_LOCK exists so an integrator can stop broadcast-based
    // management procedures leaking across a coupler during commissioning.
    if (lineConfig(origin).broadcastLock) {
        return RoutingAction::IgnoreTotally;
    }
    // §2.4.2.4.4.1, broadcast between closed media.
    return hopAction(frame.hopCount);
}

RoutingAction CouplerRoutingPolicy::decideSystemBroadcast(const datalink::LDataFrame& frame)
{
    // §2.4.2.4.4.2. ROUTE_LAST rather than ROUTE_DECREMENTED: a system
    // broadcast crosses exactly one media coupler, so the outgoing frame is
    // marked as unable to travel further.
    if (frame.hopCount >= kHopCountUnlimited) return RoutingAction::RouteUnmodified;
    if (frame.hopCount > 0) return RoutingAction::RouteLast;
    return RoutingAction::IgnoreAcked;
}

// -----------------------------------------------------------------------------
// Layer-2 acknowledge and repetition
// -----------------------------------------------------------------------------

AckPolicy CouplerRoutingPolicy::ackPolicy(CouplerPort origin,
                                          const datalink::LDataFrame& frame,
                                          RoutingAction action) const
{
    const LineCouplerConfig& config = lineConfig(origin);
    const FrameClass frameClass = classify(frame);

    if (frameClass == FrameClass::PointToPoint) {
        switch (config.physAck) {
            case PhysAckHandling::Nack:
                // Deliberate: refuse point-to-point traffic from this side
                // entirely, so the subnetwork behind the coupler cannot be
                // reparameterised from it.
                return AckPolicy::NegativeAcknowledge;
            case PhysAckHandling::All:
                return AckPolicy::Acknowledge;
            case PhysAckHandling::Normal:
                break;
        }
        // Normal: acknowledge what is routed or addressed to the coupler.
        // IGNORE_ACKED is acknowledged by definition — the frame was meant to
        // be routed and only its hop count stopped it.
        if (isForwardingAction(action) || action == RoutingAction::ForwardLocally
            || action == RoutingAction::IgnoreAcked) {
            return AckPolicy::Acknowledge;
        }
        return AckPolicy::NoAcknowledge;
    }

    // Multicast and broadcast share GROUP_IACK_ROUT.
    if (!config.groupAckRoutedOnly) {
        // Acknowledge everything, routed or not. Only useful to stop misrouted
        // frames being retransmitted forever.
        return AckPolicy::Acknowledge;
    }
    if (isForwardingAction(action) || action == RoutingAction::IgnoreAcked) {
        return AckPolicy::Acknowledge;
    }
    return AckPolicy::NoAcknowledge;
}

bool CouplerRoutingPolicy::shouldRepeat(CouplerPort origin,
                                        const datalink::LDataFrame& frame) const
{
    switch (classify(frame)) {
        case FrameClass::PointToPoint:
            return lineConfig(origin).physRepeat;
        case FrameClass::Multicast:
            return groupConfig(origin).groupRepeat;
        case FrameClass::Broadcast:
        case FrameClass::SystemBroadcast:
            return lineConfig(origin).broadcastRepeat;
    }
    return true;
}

} // namespace network
} // namespace knx
