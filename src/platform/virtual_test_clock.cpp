// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/platform/virtual_test_clock.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <vector>

namespace knx {
namespace platform {

VirtualTestClock::VirtualTestClock()
    : _nowUs(0)
    , _nextObserverId(1)
    , _observers()
{
}

void VirtualTestClock::reset()
{
    std::vector<AdvanceObserver> observers;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _nowUs = 0;
        observers.reserve(_observers.size());
        for (const auto& entry : _observers) {
            observers.push_back(entry.second);
        }
    }

    for (const auto& observer : observers) {
        if (observer) {
            observer(0u);
        }
    }
}

uint64_t VirtualTestClock::nowUs() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _nowUs;
}

uint32_t VirtualTestClock::nowMs() const
{
    return static_cast<uint32_t>(nowUs() / 1000u);
}

void VirtualTestClock::advanceUs(uint64_t deltaUs)
{
    const uint64_t now = nowUs();
    const uint64_t targetUs = (now > std::numeric_limits<uint64_t>::max() - deltaUs)
                                  ? std::numeric_limits<uint64_t>::max()
                                  : now + deltaUs;
    advanceToUs(targetUs);
}

void VirtualTestClock::advanceMs(uint32_t deltaMs)
{
    advanceUs(static_cast<uint64_t>(deltaMs) * 1000u);
}

void VirtualTestClock::advanceToUs(uint64_t targetUs)
{
    std::vector<AdvanceObserver> observers;
    uint64_t effectiveNow = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (targetUs <= _nowUs) {
            return;
        }

        _nowUs = targetUs;
        effectiveNow = _nowUs;
        observers.reserve(_observers.size());
        for (const auto& entry : _observers) {
            observers.push_back(entry.second);
        }
    }

    for (const auto& observer : observers) {
        if (observer) {
            observer(effectiveNow);
        }
    }
}

VirtualTestClock::ObserverId VirtualTestClock::addAdvanceObserver(AdvanceObserver observer)
{
    if (!observer) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    const ObserverId observerId = _nextObserverId++;
    _observers.emplace(observerId, std::move(observer));
    return observerId;
}

bool VirtualTestClock::removeAdvanceObserver(ObserverId observerId)
{
    if (observerId == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    return _observers.erase(observerId) > 0;
}

} // namespace platform
} // namespace knx
