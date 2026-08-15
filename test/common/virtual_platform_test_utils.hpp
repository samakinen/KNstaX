// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/platform/virtual_platform.hpp"
#include "knx/platform/virtual_test_clock.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <span>
#include <thread>
#include <utility>

namespace knx::testsupport {

/**
 * @brief Owns a worker thread that blocks on virtual time.
 *
 * A failing TEST_ASSERT unwinds the test body, and ~thread() on a still-joinable
 * thread calls std::terminate() — aborting the process and losing every
 * remaining test in the binary. Such a worker cannot finish on its own either:
 * it is parked on a virtual deadline, and the main thread that would have
 * advanced the clock is busy unwinding.
 *
 * So on scope exit, advance virtual time until the worker observes its deadline
 * and returns, then join. A clock advance updates the platform's observed time
 * and notifies every waiter, which releases both delayMicroseconds() and
 * waitForPredicate(). Repeating covers a worker that had not latched its
 * deadline yet. The loop is bounded by a wall-clock budget so a genuinely
 * wedged worker cannot spin forever.
 */
class VirtualTimeWorker {
public:
    static constexpr auto kDrainBudget = std::chrono::seconds(5);
    static constexpr uint32_t kDrainStepMs = 1000u;

    template <typename Fn>
    VirtualTimeWorker(platform::VirtualTestClock& clock, Fn&& body)
        : _clock(clock)
    {
        _thread = std::thread([this, body = std::forward<Fn>(body)]() mutable {
            body();
            _done.store(true, std::memory_order_release);
        });
    }

    VirtualTimeWorker(const VirtualTimeWorker&) = delete;
    VirtualTimeWorker& operator=(const VirtualTimeWorker&) = delete;

    /// Safety net: only reached when join() did not run, i.e. an assertion threw.
    ~VirtualTimeWorker() { drainAndJoin(); }

    /// Success path. A plain join, deliberately without draining: the test has
    /// already observed the worker finish, and the completion flag is set after
    /// the body returns, so draining here would race that window and jump the
    /// clock a full step out from under assertions that follow the join.
    void join()
    {
        if (_thread.joinable()) {
            _thread.join();
        }
    }

private:
    void drainAndJoin()
    {
        if (!_thread.joinable()) {
            return;
        }

        const auto limit = std::chrono::steady_clock::now() + kDrainBudget;
        while (!_done.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < limit) {
            _clock.advanceMs(kDrainStepMs);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        _thread.join();
    }

    platform::VirtualTestClock& _clock;
    std::atomic<bool> _done{false};
    std::thread _thread;
};

constexpr bool isSchedulerQuiescent(const platform::VirtualPlatform::SchedulerSnapshot& snapshot) noexcept
{
    // Quiescent = nothing runnable. Predicate-waiters are NOT counted: a task
    // blocked in taskNotifyTake (e.g. the data link RX task waiting for its
    // notify) is idle, not pending work.
    return snapshot.readyTaskCount == 0u && !snapshot.pumpActive;
}

inline const platform::VirtualPlatform::TaskSnapshot* findTaskByName(
    std::span<const platform::VirtualPlatform::TaskSnapshot> snapshots,
    const char* name) noexcept
{
    if (!name) {
        return nullptr;
    }

    for (const auto& snapshot : snapshots) {
        if (snapshot.name && std::strcmp(snapshot.name, name) == 0) {
            return &snapshot;
        }
    }

    return nullptr;
}

} // namespace knx::testsupport