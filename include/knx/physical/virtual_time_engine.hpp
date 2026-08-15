// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace knx {
namespace physical {

class VirtualTimeEngine {
public:
    using Callback = void (*)(void* context);
    using EventId = uint64_t;

    enum class Priority : uint8_t {
        TimerAlarm = 0,
        GpioEdge = 1,
        DeferredTask = 2,
    };

    VirtualTimeEngine();

    void reset();
    uint64_t nowUs() const;

    EventId scheduleAt(uint64_t timestampUs, Priority priority, Callback callback, void* context);
    bool cancel(EventId eventId);

    bool runNext();
    void runUntil(uint64_t targetUs);
    void runIdle();

private:
    struct ScheduledEvent {
        EventId id{0};
        uint64_t timestampUs{0};
        Priority priority{Priority::DeferredTask};
        uint64_t sequence{0};
        Callback callback{nullptr};
        void* context{nullptr};
        bool canceled{false};
    };

    uint64_t _nowUs;
    EventId _nextEventId;
    uint64_t _nextSequence;
    std::vector<ScheduledEvent> _events;

    std::vector<ScheduledEvent>::iterator findBestEvent();
    static bool isBefore(const ScheduledEvent& lhs, const ScheduledEvent& rhs);
};

} // namespace physical
} // namespace knx
