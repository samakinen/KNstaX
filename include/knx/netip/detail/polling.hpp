// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file polling.hpp
 * @brief Shared polling and timeout helpers for KNXnet/IP transport code
 */

#pragma once

#include "knx/platform/platform.hpp"
#include "knx/util/timing_utils.hpp"
#include "knx/util/result.hpp"
#include "knx/util/operation_progress.hpp"

#include <cstdint>

namespace knx {
namespace netip {
namespace detail {

inline constexpr auto kPollInterval = std::chrono::milliseconds{1};

inline uint32_t nowMs(platform::TimingPlatform* timingPlatform) noexcept
{
    return util::nowMs(timingPlatform);
}

inline void delayForNextPoll(platform::TimingPlatform* timingPlatform)
{
    util::delayMs(timingPlatform, static_cast<uint32_t>(kPollInterval.count()));
}

inline int remainingTimeoutMs(platform::TimingPlatform* timingPlatform,
                              uint32_t startMs,
                              int timeoutMs) noexcept
{
    if (timeoutMs <= 0) {
        return 0;
    }

    const uint32_t elapsed = nowMs(timingPlatform) - startMs;
    if (elapsed >= static_cast<uint32_t>(timeoutMs)) {
        return 0;
    }

    return timeoutMs - static_cast<int>(elapsed);
}

constexpr bool isDeferredProgress(util::OperationProgressState state) noexcept
{
    return state == util::OperationProgressState::Pending ||
           state == util::OperationProgressState::Busy;
}

inline util::Result<void> completionResult(util::OperationProgressState state)
{
    switch (state) {
        case util::OperationProgressState::Success:
            return util::Result<void>::ok();
        case util::OperationProgressState::Timeout:
            return util::ErrorCode::Timeout;
        case util::OperationProgressState::Pending:
        case util::OperationProgressState::Busy:
            return util::ErrorCode::Busy;
        case util::OperationProgressState::TransmissionFailed:
            return util::ErrorCode::TransmissionFailed;
    }

    return util::ErrorCode::OperationFailed;
}

template <typename PollFn>
util::Result<util::OperationProgressState> waitForTerminalProgress(platform::TimingPlatform* timingPlatform,
                                                                   PollFn&& poll)
{
    while (true) {
        auto progress = poll();
        if (progress.isError()) {
            return progress.error();
        }

        if (!isDeferredProgress(progress.value())) {
            return progress.value();
        }

        delayForNextPoll(timingPlatform);
    }
}

template <typename ReadyFn>
util::Result<bool> waitUntilReadable(platform::TimingPlatform* timingPlatform,
                                     int timeoutMs,
                                     ReadyFn&& ready)
{
    if (timeoutMs <= 0) {
        return ready();
    }

    const uint32_t deadline = nowMs(timingPlatform) + static_cast<uint32_t>(timeoutMs);
    while (nowMs(timingPlatform) < deadline) {
        if (ready()) {
            return true;
        }
        delayForNextPoll(timingPlatform);
    }

    return ready();
}

} // namespace detail
} // namespace netip
} // namespace knx