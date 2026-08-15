// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/virtual_time_engine.hpp"

namespace knx {
namespace physical {

VirtualTimeEngine::VirtualTimeEngine()
    : _nowUs(0)
    , _nextEventId(1)
    , _nextSequence(0)
    , _events()
{
}

void VirtualTimeEngine::reset()
{
    _nowUs = 0;
    _nextEventId = 1;
    _nextSequence = 0;
    _events.clear();
}

uint64_t VirtualTimeEngine::nowUs() const
{
    return _nowUs;
}

VirtualTimeEngine::EventId VirtualTimeEngine::scheduleAt(uint64_t timestampUs,
                                                         Priority priority,
                                                         Callback callback,
                                                         void* context)
{
    if (!callback) {
        return 0;
    }

    ScheduledEvent event;
    event.id = _nextEventId++;
    event.timestampUs = timestampUs;
    event.priority = priority;
    event.sequence = _nextSequence++;
    event.callback = callback;
    event.context = context;
    _events.push_back(event);
    return event.id;
}

bool VirtualTimeEngine::cancel(EventId eventId)
{
    if (eventId == 0) {
        return false;
    }

    for (auto& event : _events) {
        if (event.id == eventId && !event.canceled) {
            event.canceled = true;
            return true;
        }
    }
    return false;
}

bool VirtualTimeEngine::runNext()
{
    auto best = findBestEvent();
    if (best == _events.end()) {
        return false;
    }

    if (best->timestampUs > _nowUs) {
        _nowUs = best->timestampUs;
    }

    const auto callback = best->callback;
    void* context = best->context;
    _events.erase(best);
    callback(context);
    return true;
}

void VirtualTimeEngine::runUntil(uint64_t targetUs)
{
    for (;;) {
        auto best = findBestEvent();
        if (best == _events.end()) {
            break;
        }
        if (best->timestampUs > targetUs) {
            break;
        }

        if (best->timestampUs > _nowUs) {
            _nowUs = best->timestampUs;
        }

        const auto callback = best->callback;
        void* context = best->context;
        _events.erase(best);
        callback(context);
    }

    if (targetUs > _nowUs) {
        _nowUs = targetUs;
    }
}

void VirtualTimeEngine::runIdle()
{
    while (runNext()) {
    }
}

std::vector<VirtualTimeEngine::ScheduledEvent>::iterator VirtualTimeEngine::findBestEvent()
{
    auto best = _events.end();

    for (auto it = _events.begin(); it != _events.end(); ++it) {
        if (it->canceled) {
            continue;
        }
        if (best == _events.end() || isBefore(*it, *best)) {
            best = it;
        }
    }

    return best;
}

bool VirtualTimeEngine::isBefore(const ScheduledEvent& lhs, const ScheduledEvent& rhs)
{
    if (lhs.timestampUs != rhs.timestampUs) {
        return lhs.timestampUs < rhs.timestampUs;
    }
    if (lhs.priority != rhs.priority) {
        return static_cast<uint8_t>(lhs.priority) < static_cast<uint8_t>(rhs.priority);
    }
    return lhs.sequence < rhs.sequence;
}

} // namespace physical
} // namespace knx
