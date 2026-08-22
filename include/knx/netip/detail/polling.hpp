// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file polling.hpp
 * @brief Shared polling and timeout helpers for KNXnet/IP transport code
 */

#pragma once

#include "knx/platform/platform.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/util/timing_utils.hpp"
#include "knx/util/result.hpp"
#include "knx/util/operation_progress.hpp"

#include <concepts>
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

/**
 * @brief Wait for a datagram on a socket, sleeping in the socket where possible
 *
 * The predicate overload below is a busy-wait: it asks available() every
 * kPollInterval until the deadline. That is the fallback, not the intent — it
 * burns a wakeup per millisecond and still adds latency, because a datagram
 * that lands just after a check waits for the next one. A socket that can wait
 * on the kernel returns the moment the datagram arrives and costs nothing while
 * the medium is quiet, which on a KNXnet/IP routing device is most of the time.
 *
 * @return true when readable, false when the timeout expired.
 */
inline util::Result<bool> waitUntilReadable(platform::TimingPlatform* timingPlatform,
                                            int timeoutMs,
                                            platform::UdpSocket& socket)
{
    if (timeoutMs <= 0) {
        return socket.available() > 0;
    }

    auto waited = socket.waitReadable(static_cast<uint32_t>(timeoutMs));
    if (waited.isOk()) {
        return waited.value();
    }
    if (waited.error() != util::ErrorCode::OperationNotSupported) {
        return waited.error();
    }

    // Platform cannot wait on the socket; fall back to asking on an interval.
    const uint32_t deadline = nowMs(timingPlatform) + static_cast<uint32_t>(timeoutMs);
    while (nowMs(timingPlatform) < deadline) {
        if (socket.available() > 0) {
            return true;
        }
        delayForNextPoll(timingPlatform);
    }
    return socket.available() > 0;
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

/// Predicate overload, for a readiness test that is not simply "this socket has
/// a datagram". Constrained so that passing a socket picks the overload above
/// rather than trying to call the socket.
template <typename ReadyFn>
    requires std::invocable<ReadyFn>
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