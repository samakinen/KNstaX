// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file mutex.hpp
 * @brief Build-selected lock types for protocol-layer members
 *
 * Protocol code that needs a lock on a member declares a `platform::LocalMutex`
 * and takes it with `platform::LocalLock`. The build picks the
 * implementation:
 *
 * - hosts and ESP-IDF: `std::mutex` (ESP-IDF maps it onto FreeRTOS locks);
 * - single-threaded targets (`KNX_PLATFORM_BAREMETAL`, set by the standalone
 *   CMake with `-DKNX_PLATFORM=baremetal`): a type with the same interface
 *   that does nothing. The arm-none-eabi libstdc++ is built without gthreads
 *   and has no `std::mutex`.
 *
 * @thread_safety The no-op types provide no exclusion. A bare-metal product
 *                must not share these objects between an interrupt and the
 *                main loop; data shared with an interrupt uses critical
 *                sections instead (see the `lockFromTask()` policies).
 */

#pragma once

#if defined(KNX_PLATFORM_BAREMETAL)

namespace knx::platform {

/** @brief Lock type for a single-threaded target: every operation is a no-op. */
class LocalMutex {
public:
    constexpr LocalMutex() noexcept = default;
    LocalMutex(const LocalMutex&) = delete;
    LocalMutex& operator=(const LocalMutex&) = delete;

    void lock() noexcept {}
    void unlock() noexcept {}
    bool try_lock() noexcept { return true; }
};

/** @brief Recursive variant; identical on a single thread. */
using LocalRecursiveMutex = LocalMutex;

/** @brief Scoped lock over a `LocalMutex`; the no-op counterpart of `std::lock_guard`. */
template <class M>
class LocalLock {
public:
    explicit LocalLock(M& mutex) noexcept { (void)mutex; }
    LocalLock(const LocalLock&) = delete;
    LocalLock& operator=(const LocalLock&) = delete;
};

}  // namespace knx::platform

#else

#include <mutex>

namespace knx::platform {

using LocalMutex = std::mutex;
using LocalRecursiveMutex = std::recursive_mutex;

template <class M>
using LocalLock = std::lock_guard<M>;

}  // namespace knx::platform

#endif
