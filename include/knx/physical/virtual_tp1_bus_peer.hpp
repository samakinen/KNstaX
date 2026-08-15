// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/timer_gpio_hal_virtual.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace knx {
namespace physical {

class VirtualTp1BusPeer {
public:
    struct RxEdge {
        uint64_t timestampUs{0};
        uint8_t level{0};
    };

    struct FaultProfile {
        uint32_t fixedDelayUs{0};
        uint32_t jitterUs{0};
        uint32_t dropEveryN{0};
        uint32_t duplicateEveryN{0};
        uint32_t duplicateSpacingUs{1};
        uint32_t seed{1};
    };

    explicit VirtualTp1BusPeer(TimerGpioHalVirtualBus& bus);

    void clearScript();
    bool addEdgeAtUs(uint64_t timestampUs, uint8_t level);

    bool addByteWaveformAtUs(uint64_t startUs,
                             uint8_t value,
                             const BitBangConfig& config,
                             bool malformedStopBit = false);

    bool injectScript();
    bool injectScript(const FaultProfile& faults);

    bool injectCollisionPulseAtUs(uint64_t timestampUs, uint8_t dominantLevel = 1);

    const std::vector<RxEdge>& script() const;
    const std::vector<RxEdge>& lastInjectedEdges() const;

private:
    TimerGpioHalVirtualBus& _bus;
    std::vector<RxEdge> _script;
    std::vector<RxEdge> _lastInjected;

    static int32_t nextJitterOffsetUs(uint32_t& state, uint32_t jitterUs);
};

} // namespace physical
} // namespace knx
