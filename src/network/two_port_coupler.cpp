// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <array>
#include <span>

#include "knx/network/two_port_coupler.hpp"

#include "knx/datalink/frame_codec.hpp"

namespace knx {
namespace network {

TwoPortCoupler::TwoPortCoupler(datalink::Tp1DataLinkLayer& primary,
                               datalink::Tp1DataLinkLayer& secondary)
    : primary_(primary)
    , secondary_(secondary)
    , initialized_(false)
    , routingEnabled_(true)
    , policy_()
    , routingTable_()
    , filterTable_()
    , recentPrimary_()
    , recentSecondary_()
{
    // The policy consults the coupler's own table, so the Router Object's
    // PID_ROUTETABLE_CONTROL and the forwarding path cannot drift apart.
    policy_.setFilterTable(&filterTable_);
}

TwoPortCoupler::~TwoPortCoupler() {
    close();
}

util::Result<void> TwoPortCoupler::init() {
    if (initialized_.load()) return util::Result<void>::ok();

    // A coupler must see all frames on both ports, not just those addressed to
    // it: deciding whether to forward is the whole job.
    primary_.setPromiscuousMode(datalink::PromiscuousMode::Enable);
    secondary_.setPromiscuousMode(datalink::PromiscuousMode::Enable);

    primary_.setReceiveCallback(
        [this](const datalink::LDataFrame& frame) { handleRx(Port::Primary, frame); });
    secondary_.setReceiveCallback(
        [this](const datalink::LDataFrame& frame) { handleRx(Port::Secondary, frame); });

    initialized_.store(true);
    return util::Result<void>::ok();
}

void TwoPortCoupler::close() {
    if (!initialized_.load()) return;
    initialized_.store(false);
    // Leave DL callbacks as-is; the DL lifetime is managed externally.
}

util::Result<void> TwoPortCoupler::setOwnAddress(const IndividualAddress& address) {
    policy_.setOwnAddress(address);

    // Both sides must answer to the coupler's address, so that ETS can reach it
    // from either subnetwork.
    primary_.setOwnAddress(address);
    secondary_.setOwnAddress(address);
    return util::Result<void>::ok();
}

datalink::Tp1DataLinkLayer& TwoPortCoupler::linkFor(Port port) {
    return port == Port::Primary ? primary_ : secondary_;
}

TwoPortCoupler::RecentHashes& TwoPortCoupler::recentFor(Port port) {
    return port == Port::Primary ? recentPrimary_ : recentSecondary_;
}

uint64_t TwoPortCoupler::hashFrame(const datalink::LDataFrame& frame) {
    static constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    static constexpr uint64_t FNV_PRIME = 1099511628211ull;

    uint64_t h = FNV_OFFSET;
    const auto mix = [&h](uint8_t b) {
        h ^= b;
        h *= FNV_PRIME;
    };

    std::array<uint8_t, 32> buf{};
    auto res = datalink::FrameCodec::encodeFrame(frame, buf);
    if (res.isOk()) {
        for (auto b : std::span<const uint8_t>(buf.data(), res.value())) mix(b);
        return h;
    }

    // Fallback for frames too large for the scratch buffer: hash the canonical
    // fields instead.
    mix(static_cast<uint8_t>((frame.source.raw >> 8) & 0xFF));
    mix(static_cast<uint8_t>(frame.source.raw & 0xFF));
    mix(static_cast<uint8_t>((frame.destination.raw >> 8) & 0xFF));
    mix(static_cast<uint8_t>(frame.destination.raw & 0xFF));
    mix(isGroupAddress(frame.destinationType) ? 1u : 0u);
    mix(frame.hopCount);
    for (auto b : frame.tpdu) mix(b);
    return h;
}

bool TwoPortCoupler::shouldDropDueToRecent(Port port, uint64_t h) {
    RecentHashes& r = recentFor(port);
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.contains(h);
}

void TwoPortCoupler::rememberSentTo(Port port, uint64_t h) {
    RecentHashes& r = recentFor(port);
    std::lock_guard<std::mutex> lock(r.mutex);
    r.remember(h);
}

void TwoPortCoupler::handleRx(Port origin, const datalink::LDataFrame& frame) {
    if (!initialized_.load()) return;

    const uint64_t h = hashFrame(frame);
    if (shouldDropDueToRecent(origin, h)) {
        // Our own forwarded frame coming back at us off the bus.
        return;
    }

    const RoutingAction action = policy_.decide(origin, frame);

    // Report the acknowledge the specification asks for even though the TP1
    // backends decide L_ACK in the ISR — see setAckPolicyCallback().
    if (onAckPolicy_) onAckPolicy_(origin, policy_.ackPolicy(origin, frame, action));

    // Local reception is independent of the routing decision. A coupler that
    // also carries group objects must see group traffic its own address table
    // matches even when the filter table blocks it from crossing, so the
    // decision of local relevance belongs to the receiving stack, not here.
    // Individually addressed frames are the exception: only FORWARD_LOCALLY
    // means "this one is ours".
    if (onLocalDelivery_) {
        const bool individuallyAddressedToUs = (action == RoutingAction::ForwardLocally);
        const bool groupOrBroadcast = isGroupAddress(frame.destinationType);
        if (individuallyAddressedToUs || groupOrBroadcast) {
            onLocalDelivery_(origin, frame);
        }
    }

    applyAction(origin, frame, action);
}

void TwoPortCoupler::applyAction(Port origin, const datalink::LDataFrame& frame,
                                 RoutingAction action) {
    switch (action) {
        case RoutingAction::ForwardLocally:
            // Addressed to the coupler itself; handleRx() has already delivered
            // it locally. Nothing crosses to the other port.
            return;

        case RoutingAction::IgnoreTotally:
            // The routing condition was false: wrong side, LOCK, or the filter
            // table said no.
            if (onFrameFiltered_) onFrameFiltered_(origin);
            return;

        case RoutingAction::IgnoreAcked:
            // The frame was meant to be routed but its hop count is spent, so
            // it will never reach its destination. Worth surfacing separately.
            if (onFrameDropped_) onFrameDropped_(origin);
            return;

        case RoutingAction::RouteUnmodified:
        case RoutingAction::RouteDecremented:
        case RoutingAction::RouteLast:
            forward(origin, frame, action);
            return;
    }
}

void TwoPortCoupler::forward(Port origin, const datalink::LDataFrame& frame,
                             RoutingAction action) {
    if (!routingEnabled_.load()) return;

    datalink::LDataFrame fwd = frame;
    fwd.hopCount = CouplerRoutingPolicy::applyHopCount(action, frame.hopCount);

    // A forwarded frame is a fresh transmission on the far side, not a repeat
    // of one this coupler already sent there. Leaving the repeat flag set would
    // make the receiving devices treat a first arrival as a duplicate.
    fwd.repeated = false;

    const Port outPort = otherPort(origin);
    rememberSentTo(outPort, hashFrame(fwd));
    (void)linkFor(outPort).sendFrame(fwd);

    if (onFrameForwarded_) onFrameForwarded_(origin);
}

} // namespace network
} // namespace knx
