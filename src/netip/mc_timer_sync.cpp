// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/mc_timer_sync.hpp"

namespace knx {
namespace netip {
namespace ip_secure {

namespace {

/// PDT_SCALING: 0..255 stands for 0..100 %.
constexpr uint32_t kScalingFull = 255;

} // namespace

McTimerSync::McTimerSync(const Serial& ownSerial, RandomSource random)
    : ownSerial_(ownSerial)
    , random_(std::move(random))
{
    setLatencyTolerance(kDefaultLatencyToleranceMs, kDefaultSyncLatencyFraction);
}

void McTimerSync::setLatencyTolerance(uint16_t latencyToleranceMs, uint8_t syncLatencyFraction)
{
    latencyToleranceMs_ = latencyToleranceMs;
    // syncLatencyTolerance is the fraction of latencyTolerance named by
    // PID_SYNC_LATENCY_FRACTION (§2.2.2.3.2.2), rounded to the millisecond.
    syncLatencyToleranceMs_ =
        (static_cast<uint32_t>(latencyToleranceMs) * syncLatencyFraction + kScalingFull / 2) / kScalingFull;
}

uint64_t McTimerSync::advance(uint32_t nowMs)
{
    if (!clockStarted_) {
        clockStarted_ = true;
        lastNowMs_ = nowMs;
        return localMs_;
    }
    // Unsigned difference: correct across the 32-bit wrap of the caller's clock.
    localMs_ += static_cast<uint32_t>(nowMs - lastNowMs_);
    lastNowMs_ = nowMs;
    return localMs_;
}

void McTimerSync::setTimer(uint64_t value)
{
    timerBase_ = value & kTimerMask;
    localBase_ = localMs_;
}

uint64_t McTimerSync::timer(uint32_t nowMs)
{
    advance(nowMs);
    return (timerBase_ + (localMs_ - localBase_)) & kTimerMask;
}

uint32_t McTimerSync::randomBetween(uint32_t minMs, uint32_t maxMs)
{
    // random(start, end) is inclusive at both ends (§2.2.2.3.2.6, NOTE 1).
    if (maxMs <= minMs) {
        return minMs;
    }
    const uint32_t span = maxMs - minMs + 1u;
    const uint32_t draw = random_ ? random_() : 0u;
    return minMs + (draw % span);
}

McTimerSync::Window McTimerSync::periodicWindow() const noexcept
{
    // §2.2.2.3.2.2, calculated parameters:
    //   maxDelayTimeKeeperPeriodicNotify   = minDelayTimeKeeperPeriodicNotify + 3 * sync
    //   minDelayTimeFollowerPeriodicNotify = maxDelayTimeKeeperPeriodicNotify + 1 * sync
    //   maxDelayTimeFollowerPeriodicNotify = minDelayTimeFollowerPeriodicNotify + 10 * sync
    const uint32_t sync = syncLatencyToleranceMs_;
    const uint32_t keeperMax = kMinDelayTimeKeeperPeriodicNotifyMs + 3u * sync;
    if (timeKeeper_) {
        return {kMinDelayTimeKeeperPeriodicNotifyMs, keeperMax};
    }
    const uint32_t followerMin = keeperMax + sync;
    return {followerMin, followerMin + 10u * sync};
}

McTimerSync::Window McTimerSync::updateWindow() const noexcept
{
    //   maxDelayTimeKeeperUpdateNotify   = minDelayTimeKeeperUpdateNotify + 1 * sync
    //   minDelayTimeFollowerUpdateNotify = maxDelayTimeKeeperUpdateNotify + 1 * sync
    //   maxDelayTimeFollowerUpdateNotify = minDelayTimeFollowerUpdateNotify + 10 * sync
    const uint32_t sync = syncLatencyToleranceMs_;
    const uint32_t keeperMax = kMinDelayTimeKeeperUpdateNotifyMs + sync;
    if (timeKeeper_) {
        return {kMinDelayTimeKeeperUpdateNotifyMs, keeperMax};
    }
    const uint32_t followerMin = keeperMax + sync;
    return {followerMin, followerMin + 10u * sync};
}

void McTimerSync::reschedulePeriodic()
{
    const Window window = periodicWindow();
    notifyAtLocalMs_ = localMs_ + randomBetween(window.minMs, window.maxMs);
}

void McTimerSync::scheduleUpdate(const Serial& serial, const Tag& tag)
{
    rememberedSerial_ = serial;
    rememberedTag_ = tag;
    const Window window = updateWindow();
    notifyAtLocalMs_ = localMs_ + randomBetween(window.minMs, window.maxMs);
    state_ = State::SchedUpdate;
}

void McTimerSync::start(uint32_t nowMs, uint64_t initialTimer, StartReason reason)
{
    advance(nowMs);
    setTimer(initialTimer);
    state_ = State::SchedPeriodic;
    // §2.2.2.3.2.8 does not say which role a device starts in. Starting as a
    // follower means the first periodic notify (E10 → A8) is what makes it a
    // keeper, which is how a device that hears nobody comes to claim the role.
    timeKeeper_ = false;
    rememberedSerial_ = {};
    rememberedTag_ = {};
    const uint32_t delay = (reason == StartReason::PowerUp) ? randomBetween(0, kMaxDelayInitialNotifyMs) : 0u;
    notifyAtLocalMs_ = localMs_ + delay;
}

McTimerSync::Band McTimerSync::classify(uint64_t receivedTimer, uint32_t nowMs)
{
    const uint64_t local = timer(nowMs);
    const uint64_t received = receivedTimer & kTimerMask;
    // Written as additions so a young timer (smaller than the tolerances) does
    // not underflow into a huge "local - tolerance".
    if (received > local) {
        return Band::Newer;
    }
    if (received + syncLatencyToleranceMs_ > local) {
        return Band::InSync;
    }
    if (received + latencyToleranceMs_ > local) {
        return Band::Late;
    }
    return Band::Outdated;
}

void McTimerSync::onTimerNotify(uint64_t receivedTimer, const Serial& serial, const Tag& tag, uint32_t nowMs)
{
    // Transition table §2.2.2.3.2.7, rows E01–E04.
    switch (classify(receivedTimer, nowMs)) {
        case Band::Newer:  // E01: A1 + A9 + A3, >> SCHED_PERIODIC from either state
            setTimer(receivedTimer);
            timeKeeper_ = false;
            reschedulePeriodic();
            state_ = State::SchedPeriodic;
            return;
        case Band::InSync:  // E02: A9 + A3, >> SCHED_PERIODIC from either state
            timeKeeper_ = false;
            reschedulePeriodic();
            state_ = State::SchedPeriodic;
            return;
        case Band::Late:  // E03: A0
            return;
        case Band::Outdated:  // E04: A4 >> SCHED_UPDATE, or A0 when already there
            if (state_ == State::SchedPeriodic) {
                scheduleUpdate(serial, tag);
            }
            return;
    }
}

bool McTimerSync::onSecureWrapper(uint64_t receivedTimer, const Serial& serial, const Tag& tag, uint32_t nowMs)
{
    // Rows E05–E08. A time keeper stays one on a newer SECURE_WRAPPER (NOTE 2),
    // and nothing here cancels a scheduled update (NOTE 3).
    switch (classify(receivedTimer, nowMs)) {
        case Band::Newer:  // E05: A1 + A2 (+ A3 in SCHED_PERIODIC)
            setTimer(receivedTimer);
            if (state_ == State::SchedPeriodic) {
                reschedulePeriodic();
            }
            return true;
        case Band::InSync:  // E06: A2 (+ A3 in SCHED_PERIODIC)
            if (state_ == State::SchedPeriodic) {
                reschedulePeriodic();
            }
            return true;
        case Band::Late:  // E07: A2
            return true;
        case Band::Outdated:  // E08: discarded; A4 >> SCHED_UPDATE, or A0
            if (state_ == State::SchedPeriodic) {
                scheduleUpdate(serial, tag);
            }
            return false;
    }
    return false;
}

uint64_t McTimerSync::onTransmit(uint32_t nowMs)
{
    // E09: A3 in SCHED_PERIODIC — the frame itself tells the group the time —
    // and A0 in SCHED_UPDATE, where the pending answer must still go out.
    const uint64_t now = timer(nowMs);
    if (state_ == State::SchedPeriodic) {
        reschedulePeriodic();
    }
    return now;
}

uint32_t McTimerSync::millisecondsUntilNotify(uint32_t nowMs)
{
    advance(nowMs);
    if (notifyAtLocalMs_ <= localMs_) {
        return 0;
    }
    const uint64_t remaining = notifyAtLocalMs_ - localMs_;
    return remaining > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(remaining);
}

std::optional<McTimerSync::Notify> McTimerSync::poll(uint32_t nowMs)
{
    if (millisecondsUntilNotify(nowMs) != 0) {
        return std::nullopt;
    }

    Notify notify{};
    notify.timer = timer(nowMs);
    if (state_ == State::SchedUpdate) {
        // E10 in SCHED_UPDATE: A6 + A8 + A3, >> SCHED_PERIODIC. The remembered
        // serial and tag make this answer a challenge response the outdated
        // device can trust at once (§2.2.2.4.2, Message Tag).
        notify.serial = rememberedSerial_;
        notify.tag = rememberedTag_;
        notify.update = true;
        state_ = State::SchedPeriodic;
    } else {
        // E10 in SCHED_PERIODIC: A5 + A8 + A3.
        notify.serial = ownSerial_;
        const uint32_t draw = random_ ? random_() : 0u;
        notify.tag = {static_cast<uint8_t>(draw >> 8), static_cast<uint8_t>(draw)};
    }
    timeKeeper_ = true;
    reschedulePeriodic();
    return notify;
}

} // namespace ip_secure
} // namespace netip
} // namespace knx
