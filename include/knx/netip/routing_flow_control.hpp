// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/routing.hpp"
#include "knx/util/inplace_function.hpp"

#include <cstdint>

namespace knx {
namespace netip {

/**
 * @file routing_flow_control.hpp
 * @brief When a KNX IP device is allowed to put another datagram on the wire.
 *
 * IP has no medium access control, so KNXnet/IP Routing carries its own, and it
 * is not advisory: 03/02/06 §2.1 makes both halves a SHALL.
 *
 *   - Egress budget. At most 50 ROUTING_INDICATION datagrams per second, and
 *     never two closer together than 5 ms. The limit exists because the KNX IP
 *     side of a media coupler is orders of magnitude faster than the TP1 side
 *     it feeds; a device that ignores it overruns couplers rather than its own
 *     network.
 *   - ROUTING_BUSY back-off (03/08/05 §4.3). A congested receiver multicasts a
 *     wait time and every sender stops for it. Overlapping notifications take
 *     the *longer* of the remaining and the new wait, so a device that keeps
 *     complaining is not repeatedly let off early.
 *
 * The optional random slowdown after the wait expires is implemented as well.
 * It is the part that keeps a busy storm from resolving into every device
 * resuming on the same millisecond and immediately re-congesting the receiver,
 * which is the failure the fixed wait alone does not prevent.
 *
 * All state is millisecond arithmetic against a clock the caller supplies, so
 * this is exercised in host tests at simulated time rather than in real time.
 */

/// A uniform draw from [0,1), used for the ROUTING_BUSY random back-off. Kept
/// as an injected function rather than a call into a global RNG so the back-off
/// can be pinned in tests and so an embedded build decides for itself which
/// entropy source it can afford on this path.
using RandomUnitSource = util::InplaceFunction<float(), 32>;

class RoutingFlowControl {
public:
    struct Config {
        /// 03/02/06 §2.1. Zero disables the budget (never on a shipping device;
        /// it exists so a test can isolate the gap rule from the rate rule).
        uint16_t maxDatagramsPerSecond{50};
        /// Minimum spacing between two outbound routing datagrams.
        uint16_t minSendGapMs{5};
        /// trandom = [0..1] * N * kRandomSlotMs (§4.3). Zero disables it.
        uint16_t randomBackoffSlotMs{50};
    };

    static constexpr uint32_t kBusyCounterHoldoffMs = 10;   ///< §4.3 note 3
    static constexpr uint32_t kBusyCounterDecayStepMs = 5;  ///< §4.3 tbd

    RoutingFlowControl() noexcept = default;
    explicit RoutingFlowControl(Config config) noexcept : config_(config) {}

    void reset() noexcept;

    /// May a routing datagram be sent at `nowMs`? Advances no state.
    [[nodiscard]] bool canSend(uint32_t nowMs) const noexcept;

    /// How long to wait before `canSend()` turns true. 0 when it already is.
    [[nodiscard]] uint32_t millisecondsUntilSendable(uint32_t nowMs) const noexcept;

    /// Record that a datagram went out at `nowMs`. Call only after canSend().
    void noteSent(uint32_t nowMs) noexcept;

    /// A ROUTING_BUSY arrived. `random01` is a uniform [0,1) draw used for
    /// trandom; supply 0 for a deterministic wait.
    void noteBusyReceived(const RoutingBusyInfo& info, uint32_t nowMs, float random01 = 0.0f) noexcept;

    /// Busy counter N of §4.3, after decay. Diagnostic, and asserted in tests.
    [[nodiscard]] uint32_t busyCounter(uint32_t nowMs) const noexcept;

    [[nodiscard]] bool isBusyBackoffActive(uint32_t nowMs) const noexcept;

private:
    [[nodiscard]] uint32_t decayedBusyCounter(uint32_t nowMs) const noexcept;

    Config config_;

    bool hasSent_{false};
    uint32_t lastSendMs_{0};

    // Rolling one-second budget. A full timestamp ring would be exact; this
    // keeps a count and the window it belongs to, which costs two words and
    // errs towards sending less, never more.
    uint32_t windowStartMs_{0};
    uint16_t sentInWindow_{0};

    bool busyActive_{false};
    uint32_t busyUntilMs_{0};

    uint32_t busyCounter_{0};
    bool hasBusy_{false};
    uint32_t lastBusyMs_{0};
    /// When the counter starts decaying: last busy + tslowduration (N * 100 ms).
    uint32_t busyDecayStartMs_{0};
};

} // namespace netip
} // namespace knx
