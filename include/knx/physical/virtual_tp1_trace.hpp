// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/timer_gpio_hal_virtual.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace knx {
namespace physical {

class VirtualTp1Trace {
public:
    struct Header {
        uint32_t traceVersionMajor{1};
        uint32_t traceVersionMinor{0};
        std::string simProfile{"tp1-virtual-mvp"};
        uint32_t seed{0};
    };

    enum class EventType : uint8_t {
        TimerAlarm = 0,
        GpioEdge = 1,
        TxLevelSet = 2,
        Tp1Event = 3,
    };

    enum class EventSource : uint8_t {
        Driver = 0,
        BusPeer = 1,
        FaultInjector = 2,
    };

    struct Event {
        uint64_t tsUs{0};
        EventType type{EventType::GpioEdge};
        EventSource source{EventSource::Driver};
        uint16_t pin{0};
        int32_t level{-1};
        uint32_t meta{0};
    };

    static std::string encode(const Header& header, const std::vector<Event>& events);
    static bool decode(const std::string& payload, Header& outHeader, std::vector<Event>& outEvents);

    static std::vector<Event> captureTxTransitions(const std::vector<TimerGpioHalVirtualBus::TxTransition>& transitions);
    static bool replayToVirtualBus(TimerGpioHalVirtualBus& bus, const std::vector<Event>& events);
};

} // namespace physical
} // namespace knx
