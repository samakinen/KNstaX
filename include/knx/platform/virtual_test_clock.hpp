// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>

namespace knx {
namespace platform {

class VirtualTestClock {
public:
    using ObserverId = uint64_t;
    using AdvanceObserver = std::function<void(uint64_t)>;

    VirtualTestClock();

    void reset();

    uint64_t nowUs() const;
    uint32_t nowMs() const;

    void advanceUs(uint64_t deltaUs);
    void advanceMs(uint32_t deltaMs);
    void advanceToUs(uint64_t targetUs);

    ObserverId addAdvanceObserver(AdvanceObserver observer);
    bool removeAdvanceObserver(ObserverId observerId);

private:
    mutable std::mutex _mutex;
    uint64_t _nowUs;
    ObserverId _nextObserverId;
    std::map<ObserverId, AdvanceObserver> _observers;
};

} // namespace platform
} // namespace knx
