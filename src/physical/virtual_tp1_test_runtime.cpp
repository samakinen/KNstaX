// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/virtual_tp1_test_runtime.hpp"

namespace knx {
namespace physical {

VirtualTp1TestRuntime::VirtualTp1TestRuntime()
    : _clock()
    , _platform(_clock)
    , _bus()
{
    (void)_bus.attachClock(_clock);
}

void VirtualTp1TestRuntime::reset()
{
    _clock.reset();
    _bus.reset();
    (void)_bus.attachClock(_clock);
}

void VirtualTp1TestRuntime::advanceTimeUs(uint64_t deltaUs)
{
    _platform.advanceTimeUs(deltaUs);
}

void VirtualTp1TestRuntime::advanceTimeMs(uint32_t deltaMs)
{
    _platform.advanceTimeMs(deltaMs);
}

uint64_t VirtualTp1TestRuntime::nowUs() const
{
    return _clock.nowUs();
}

uint32_t VirtualTp1TestRuntime::nowMs() const
{
    return _clock.nowMs();
}

platform::VirtualTestClock& VirtualTp1TestRuntime::clock()
{
    return _clock;
}

const platform::VirtualTestClock& VirtualTp1TestRuntime::clock() const
{
    return _clock;
}

platform::VirtualPlatform& VirtualTp1TestRuntime::platform()
{
    return _platform;
}

const platform::VirtualPlatform& VirtualTp1TestRuntime::platform() const
{
    return _platform;
}

TimerGpioHalVirtualBus& VirtualTp1TestRuntime::bus()
{
    return _bus;
}

const TimerGpioHalVirtualBus& VirtualTp1TestRuntime::bus() const
{
    return _bus;
}

} // namespace physical
} // namespace knx
