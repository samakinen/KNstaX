// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/platform/virtual_platform.hpp"
#include "knx/platform/virtual_test_clock.hpp"

#include <cstring>
#include <span>
#include <utility>

namespace knx::testsupport {

/**
 * @brief Runs a test body as a VirtualPlatform task, deleted on scope exit.
 *
 * The body runs at once, up to its first blocking call, and from then on
 * whenever virtual time or a wake-up makes it runnable. Deleting it on scope
 * exit matters when an assertion fails while the body is still blocked: the
 * task is discarded instead of being left for the next test's platform.
 */
class ScopedVirtualTask {
public:
    template <typename Fn>
    ScopedVirtualTask(platform::VirtualPlatform& platform, const char* name, Fn&& body)
        : _platform(platform)
    {
        platform::TaskConfig config{};
        config.name = name;
        config.function = std::forward<Fn>(body);
        _handle = _platform.createTask(config);
    }

    ScopedVirtualTask(const ScopedVirtualTask&) = delete;
    ScopedVirtualTask& operator=(const ScopedVirtualTask&) = delete;

    ~ScopedVirtualTask()
    {
        if (_handle) {
            _platform.deleteTask(_handle);
        }
    }

    platform::TaskHandle handle() const noexcept { return _handle; }

private:
    platform::VirtualPlatform& _platform;
    platform::TaskHandle _handle{nullptr};
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