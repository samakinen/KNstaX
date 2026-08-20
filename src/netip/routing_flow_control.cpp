// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/routing_flow_control.hpp"

#include <algorithm>

namespace knx {
namespace netip {

namespace {

/// Millisecond clocks wrap. Comparing the difference rather than the values
/// keeps every deadline check correct across the wrap instead of once every
/// 49.7 days turning "deadline passed" into "deadline 49 days away".
constexpr bool reached(uint32_t nowMs, uint32_t deadlineMs) noexcept
{
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

constexpr uint32_t remaining(uint32_t nowMs, uint32_t deadlineMs) noexcept
{
    return reached(nowMs, deadlineMs) ? 0u : (deadlineMs - nowMs);
}

constexpr uint32_t kWindowMs = 1000;

} // namespace

void RoutingFlowControl::reset() noexcept
{
    hasSent_ = false;
    lastSendMs_ = 0;
    windowStartMs_ = 0;
    sentInWindow_ = 0;
    busyActive_ = false;
    busyUntilMs_ = 0;
    busyCounter_ = 0;
    hasBusy_ = false;
    lastBusyMs_ = 0;
    busyDecayStartMs_ = 0;
}

uint32_t RoutingFlowControl::decayedBusyCounter(uint32_t nowMs) const noexcept
{
    if (busyCounter_ == 0 || !hasBusy_) return 0;
    if (!reached(nowMs, busyDecayStartMs_)) return busyCounter_;

    // One step of N is shed every tbd = 5 ms once tslowduration has elapsed.
    const uint32_t elapsed = nowMs - busyDecayStartMs_;
    const uint32_t shed = elapsed / kBusyCounterDecayStepMs;
    return (shed >= busyCounter_) ? 0u : (busyCounter_ - shed);
}

uint32_t RoutingFlowControl::busyCounter(uint32_t nowMs) const noexcept
{
    return decayedBusyCounter(nowMs);
}

bool RoutingFlowControl::isBusyBackoffActive(uint32_t nowMs) const noexcept
{
    return busyActive_ && !reached(nowMs, busyUntilMs_);
}

bool RoutingFlowControl::canSend(uint32_t nowMs) const noexcept
{
    return millisecondsUntilSendable(nowMs) == 0;
}

uint32_t RoutingFlowControl::millisecondsUntilSendable(uint32_t nowMs) const noexcept
{
    uint32_t wait = 0;

    if (busyActive_ && !reached(nowMs, busyUntilMs_)) {
        wait = std::max(wait, remaining(nowMs, busyUntilMs_));
    }

    if (hasSent_ && config_.minSendGapMs != 0) {
        const uint32_t gapDeadline = lastSendMs_ + config_.minSendGapMs;
        wait = std::max(wait, remaining(nowMs, gapDeadline));
    }

    if (config_.maxDatagramsPerSecond != 0 && sentInWindow_ >= config_.maxDatagramsPerSecond) {
        const uint32_t windowEnd = windowStartMs_ + kWindowMs;
        if (!reached(nowMs, windowEnd)) {
            wait = std::max(wait, remaining(nowMs, windowEnd));
        }
    }

    return wait;
}

void RoutingFlowControl::noteSent(uint32_t nowMs) noexcept
{
    if (!hasSent_ || reached(nowMs, windowStartMs_ + kWindowMs)) {
        windowStartMs_ = nowMs;
        sentInWindow_ = 0;
    }

    if (sentInWindow_ < UINT16_MAX) {
        ++sentInWindow_;
    }
    lastSendMs_ = nowMs;
    hasSent_ = true;
}

void RoutingFlowControl::noteBusyReceived(const RoutingBusyInfo& info,
                                          uint32_t nowMs,
                                          float random01) noexcept
{
    // A control field this device does not interpret still stops it: §4.3 makes
    // the general flow control apply either way, so there is nothing to filter.
    // N only counts notifications far enough apart to be independent events;
    // several devices crossing their queue threshold together produce one.
    if (!hasBusy_ || (nowMs - lastBusyMs_) >= kBusyCounterHoldoffMs) {
        busyCounter_ = decayedBusyCounter(nowMs) + 1;
    } else {
        busyCounter_ = std::max<uint32_t>(decayedBusyCounter(nowMs), 1u);
    }
    hasBusy_ = true;
    lastBusyMs_ = nowMs;
    busyDecayStartMs_ = nowMs + (busyCounter_ * 100u);  // tslowduration = N * 100 ms

    uint32_t wait = info.waitTimeMs;
    if (config_.randomBackoffSlotMs != 0) {
        const float clamped = std::clamp(random01, 0.0f, 1.0f);
        wait += static_cast<uint32_t>(clamped * static_cast<float>(busyCounter_) *
                                      static_cast<float>(config_.randomBackoffSlotMs));
    }

    const uint32_t candidate = nowMs + wait;
    // Overlapping notifications take the longer of the two, never the newer.
    if (busyActive_ && !reached(nowMs, busyUntilMs_) && reached(busyUntilMs_, candidate)) {
        return;
    }
    busyUntilMs_ = candidate;
    busyActive_ = true;
}

} // namespace netip
} // namespace knx
