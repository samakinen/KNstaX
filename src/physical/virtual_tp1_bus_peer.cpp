// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/virtual_tp1_bus_peer.hpp"

#include <algorithm>
#include <limits>

namespace knx {
namespace physical {

VirtualTp1BusPeer::VirtualTp1BusPeer(TimerGpioHalVirtualBus& bus)
    : _bus(bus)
    , _script()
    , _lastInjected()
{
}

void VirtualTp1BusPeer::clearScript()
{
    _script.clear();
    _lastInjected.clear();
}

bool VirtualTp1BusPeer::addEdgeAtUs(uint64_t timestampUs, uint8_t level)
{
    RxEdge edge;
    edge.timestampUs = timestampUs;
    edge.level = static_cast<uint8_t>(level & 0x1u);

    const auto position = std::upper_bound(
        _script.begin(),
        _script.end(),
        edge.timestampUs,
        [](uint64_t lhsTimestampUs, const RxEdge& rhs) {
            return lhsTimestampUs < rhs.timestampUs;
        });

    _script.insert(position, edge);
    return true;
}

bool VirtualTp1BusPeer::addByteWaveformAtUs(uint64_t startUs,
                                            uint8_t value,
                                            const BitBangConfig& config,
                                            bool malformedStopBit)
{
    uint64_t timestampUs = startUs;
    const uint8_t dominantLevel = config.rxDominantHigh ? 1u : 0u;
    const uint8_t recessiveLevel = static_cast<uint8_t>(dominantLevel == 0u ? 1u : 0u);

    const uint64_t preEdgeUs = (startUs > 0u) ? (startUs - 1u) : startUs;
    (void)addEdgeAtUs(preEdgeUs, recessiveLevel);

    auto emitBit = [&](uint8_t bit) {
        if (bit == 0u) {
            (void)addEdgeAtUs(timestampUs, dominantLevel);
            (void)addEdgeAtUs(timestampUs + config.zeroActiveTimeUs, recessiveLevel);
        }
        timestampUs += config.serialBitTimeUs;
    };

    emitBit(0u);
    for (int bit = 0; bit < 8; ++bit) {
        emitBit(static_cast<uint8_t>((value >> bit) & 0x1u));
    }

    const uint8_t parity = static_cast<uint8_t>(__builtin_popcount(value) & 0x1u);
    emitBit(parity);
    emitBit(malformedStopBit ? 0u : 1u);

    return true;
}

bool VirtualTp1BusPeer::injectScript()
{
    return injectScript(FaultProfile{});
}

bool VirtualTp1BusPeer::injectScript(const FaultProfile& faults)
{
    _lastInjected.clear();

    uint32_t jitterState = (faults.seed == 0u) ? 1u : faults.seed;
    for (size_t i = 0; i < _script.size(); ++i) {
        const RxEdge& original = _script[i];

        const bool shouldDrop = (faults.dropEveryN > 0u) && (((i + 1u) % faults.dropEveryN) == 0u);
        if (shouldDrop) {
            continue;
        }

        int64_t timestampUs = static_cast<int64_t>(original.timestampUs) + static_cast<int64_t>(faults.fixedDelayUs);
        timestampUs += nextJitterOffsetUs(jitterState, faults.jitterUs);
        if (timestampUs < 0) {
            timestampUs = 0;
        }

        RxEdge injected;
        injected.timestampUs = static_cast<uint64_t>(timestampUs);
        injected.level = original.level;
        _lastInjected.push_back(injected);

        if (!_bus.scheduleRxLevelAtUs(injected.timestampUs, injected.level)) {
            return false;
        }

        const bool shouldDuplicate = (faults.duplicateEveryN > 0u) && (((i + 1u) % faults.duplicateEveryN) == 0u);
        if (shouldDuplicate) {
            uint64_t duplicateTimestampUs = injected.timestampUs;
            if (duplicateTimestampUs <= std::numeric_limits<uint64_t>::max() - faults.duplicateSpacingUs) {
                duplicateTimestampUs += faults.duplicateSpacingUs;
            }
            RxEdge duplicate;
            duplicate.timestampUs = duplicateTimestampUs;
            duplicate.level = injected.level;
            _lastInjected.push_back(duplicate);
            if (!_bus.scheduleRxLevelAtUs(duplicate.timestampUs, duplicate.level)) {
                return false;
            }
        }
    }

    std::stable_sort(_lastInjected.begin(), _lastInjected.end(), [](const RxEdge& lhs, const RxEdge& rhs) {
        return lhs.timestampUs < rhs.timestampUs;
    });

    return true;
}

bool VirtualTp1BusPeer::injectCollisionPulseAtUs(uint64_t timestampUs, uint8_t dominantLevel)
{
    const uint8_t dominant = static_cast<uint8_t>(dominantLevel & 0x1u);
    const uint8_t recessive = static_cast<uint8_t>(dominant == 0u ? 1u : 0u);
    const uint64_t preEdgeUs = (timestampUs > 0u) ? (timestampUs - 1u) : timestampUs;
    return _bus.scheduleRxLevelAtUs(preEdgeUs, recessive) && _bus.scheduleRxLevelAtUs(timestampUs, dominant);
}

const std::vector<VirtualTp1BusPeer::RxEdge>& VirtualTp1BusPeer::script() const
{
    return _script;
}

const std::vector<VirtualTp1BusPeer::RxEdge>& VirtualTp1BusPeer::lastInjectedEdges() const
{
    return _lastInjected;
}

int32_t VirtualTp1BusPeer::nextJitterOffsetUs(uint32_t& state, uint32_t jitterUs)
{
    if (jitterUs == 0u) {
        return 0;
    }

    state = state * 1664525u + 1013904223u;
    const uint32_t span = (jitterUs * 2u) + 1u;
    const int32_t centered = static_cast<int32_t>(state % span) - static_cast<int32_t>(jitterUs);
    return centered;
}

} // namespace physical
} // namespace knx
