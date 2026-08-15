// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/timer_gpio_hal_virtual.hpp"
#include "knx/platform/virtual_platform.hpp"
#include "knx/platform/virtual_test_clock.hpp"

namespace knx {
namespace physical {

class VirtualTp1TestRuntime {
public:
    VirtualTp1TestRuntime();

    void reset();
    void advanceTimeUs(uint64_t deltaUs);
    void advanceTimeMs(uint32_t deltaMs);

    uint64_t nowUs() const;
    uint32_t nowMs() const;

    platform::VirtualTestClock& clock();
    const platform::VirtualTestClock& clock() const;

    platform::VirtualPlatform& platform();
    const platform::VirtualPlatform& platform() const;

    TimerGpioHalVirtualBus& bus();
    const TimerGpioHalVirtualBus& bus() const;

private:
    platform::VirtualTestClock _clock;
    platform::VirtualPlatform _platform;
    TimerGpioHalVirtualBus _bus;
};

} // namespace physical
} // namespace knx
