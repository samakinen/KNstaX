// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/platform/platform.hpp"

#include <chrono>
#include <cstdint>
#include <thread>

namespace knx {
namespace util {

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

} // namespace util
} // namespace knx