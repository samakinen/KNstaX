// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/platform/platform.hpp"

#include <cstdint>
#if !defined(KNX_PLATFORM_BAREMETAL)
#include <chrono>
#include <thread>
#endif

namespace knx {
namespace util {

#if defined(KNX_PLATFORM_BAREMETAL)
// A single-threaded target has no system clock to fall back on: time comes
// from the platform. Callers that reach these without one get a frozen clock
// and no delay, so every code path must be given a TimingPlatform.
inline uint32_t systemNowMs()
{
    return 0u;
}

inline uint32_t nowMs(platform::TimingPlatform* timingPlatform)
{
    return timingPlatform != nullptr ? timingPlatform->millis() : systemNowMs();
}

inline void delayMs(platform::TimingPlatform* timingPlatform, uint32_t ms)
{
    if (timingPlatform != nullptr) {
        timingPlatform->delay(ms);
    }
}
#else
inline uint32_t systemNowMs()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

inline uint32_t nowMs(platform::TimingPlatform* timingPlatform)
{
    return timingPlatform != nullptr ? timingPlatform->millis() : systemNowMs();
}

inline void delayMs(platform::TimingPlatform* timingPlatform, uint32_t ms)
{
    if (timingPlatform != nullptr) {
        timingPlatform->delay(ms);
        return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
#endif

} // namespace util
} // namespace knx