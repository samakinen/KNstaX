// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file random_source.hpp
 * @brief Where KNXnet/IP routing gets its randomness from.
 *
 * Three things on the routing medium must be random, and none of them is a key:
 *
 *  - the ROUTING_BUSY back-off trandom (03_08_05 §2.3.5), so that devices told
 *    to stop together do not all resume on the same millisecond;
 *  - the Secure Routing message tag (03_08_09 §2.2.2.4.2), so that the clear
 *    security information of consecutive frames does not just count up;
 *  - the TIMER_NOTIFY delays (03_08_09 §2.2.2.3), so that devices powered up
 *    together do not all notify at once.
 *
 * The product wires the platform's RNG in (Platform::randomBytes, the hardware
 * RNG on an ESP32). A routing stack used without that wiring still gets
 * different values per draw and per boot from the fallback below; it is not a
 * source for key material, and nothing here uses it as one.
 */

#include "knx/util/inplace_function.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

/// Fill the span with unpredictable octets.
using RandomBytesSource = util::InplaceFunction<void(std::span<uint8_t> out), 32>;

namespace detail {

/// SplitMix64 over a shared counter seeded from the high-resolution clock the
/// first time it is used. Distinct per draw, distinct per boot in practice.
inline uint64_t fallbackRandom64() noexcept
{
    static std::atomic<uint64_t> state{0};
    uint64_t seed = state.load(std::memory_order_relaxed);
    if (seed == 0) {
        const auto ticks = static_cast<uint64_t>(
            std::chrono::high_resolution_clock::now().time_since_epoch().count());
        seed = ticks ^ (reinterpret_cast<uintptr_t>(&state) * 0x9E3779B97F4A7C15ULL) ^ 1u;
        uint64_t expected = 0;
        (void)state.compare_exchange_strong(expected, seed, std::memory_order_relaxed);
    }
    uint64_t z = state.fetch_add(0x9E3779B97F4A7C15ULL, std::memory_order_relaxed) + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

} // namespace detail

/// A uniform 32-bit draw from `source`, or from the fallback when none is set.
inline uint32_t randomU32(const RandomBytesSource& source) noexcept
{
    if (!source) {
        return static_cast<uint32_t>(detail::fallbackRandom64() >> 32);
    }
    std::array<uint8_t, 4> bytes{};
    source(bytes);
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

/// A uniform draw from [0, 1).
inline float randomUnit(const RandomBytesSource& source) noexcept
{
    // 24 bits is all a float mantissa holds; more would round up to 1.0.
    return static_cast<float>(randomU32(source) >> 8) / 16777216.0f;
}

} // namespace netip
} // namespace knx
