// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file bitbang_timing_stats.hpp
 * @brief Optional zero-overhead ISR timing instrumentation for the bitbang driver.
 *
 * Enable with CONFIG_KNX_TP1_BITBANG_TIMING_STATS=y (Kconfig or sdkconfig).
 * When the symbol is not defined every macro expands to nothing — the compiler
 * emits zero extra code and there is zero runtime overhead.
 *
 * When enabled:
 *  - onTimerAlarm() and onRxEdge() execution time is measured in raw CPU cycles
 *    via esp_cpu_get_cycle_count() (a single CSR read, ~1 cycle on RISC-V).
 *  - processStartBit() and processReceiveBit() execution time is measured the
 *    same way, from the call site in processRxTimer() to keep those functions
 *    free of multiple-exit instrumentation.
 *  - Timer alarm jitter (how many µs late the ISR fired vs the scheduled alarm)
 *    is measured via knx_timer_gpio_hal_timer_now_us(), which is already called
 *    from ISR context elsewhere in the driver.
 *
 * Usage (logging example):
 * @code
 *   #ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
 *   const auto& s = driver.timingStats();
 *   ESP_LOGI(TAG, "jitter   min=%u max=%u avg=%u µs (n=%u)",
 *            s.timerJitterUs.minVal, s.timerJitterUs.maxVal,
 *            s.timerJitterUs.avg(),  s.timerJitterUs.count);
 *   ESP_LOGI(TAG, "onTimer  min=%u max=%u avg=%u cyc (n=%u)",
 *            s.onTimerAlarmCycles.minVal, s.onTimerAlarmCycles.maxVal,
 *            s.onTimerAlarmCycles.avg(),  s.onTimerAlarmCycles.count);
 *   driver.resetTimingStats();
 *   #endif
 * @endcode
 */

#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS

#include <cstdint>
#include "esp_cpu.h"

namespace knx {
namespace physical {

/**
 * @brief Rolling min/max/average accumulator for a single timing metric.
 *
 * Written exclusively from ISR context. On the single-core ESP32-C6 there is no
 * preemption within an ISR, so no locking is required for writes. Reads from
 * task context may observe a partially-updated sample — acceptable for profiling.
 */
struct BitBangTimingChannel {
    uint32_t count{0};
    uint32_t minVal{UINT32_MAX};
    uint32_t maxVal{0};
    uint64_t sumVal{0}; ///< 64-bit prevents overflow: 2^32 samples × 2^20 cycles < 2^64.

    /// Record one sample. Must be called from ISR context (or with IRQs disabled).
    void record(uint32_t v) noexcept
    {
        ++count;
        if (v < minVal) minVal = v;
        if (v > maxVal) maxVal = v;
        sumVal += static_cast<uint64_t>(v);
    }

    void reset() noexcept
    {
        count  = 0;
        minVal = UINT32_MAX;
        maxVal = 0;
        sumVal = 0;
    }

    uint32_t avg() const noexcept
    {
        return count ? static_cast<uint32_t>(sumVal / count) : 0;
    }
};

/**
 * @brief All timing metrics collected by the bitbang ISR driver.
 *
 * Access via BitBangDriverTimerIsr::timingStats() (const reference).
 * Reset via BitBangDriverTimerIsr::resetTimingStats().
 */
struct BitBangTimingStats {
    /**
     * How many µs late the timer ISR fired compared to its scheduled alarm.
     * Computed as timer_now_us() − scheduled_alarm_us at ISR entry.
     * Values > ~20 µs indicate scheduling pressure that may cause framing errors.
     */
    BitBangTimingChannel timerJitterUs;

    /**
     * Total CPU cycles spent inside onTimerAlarm(), including all sub-routines
     * (processStartBit, processReceiveBit, processTxTimer, rearmTimer, …).
     */
    BitBangTimingChannel onTimerAlarmCycles;

    /**
     * Total CPU cycles spent inside onRxEdge().
     */
    BitBangTimingChannel onRxEdgeCycles;

    /**
     * CPU cycles spent inside processStartBit() only.
     * Sub-range of onTimerAlarmCycles; useful for isolating start-bit validation cost.
     */
    BitBangTimingChannel processStartBitCycles;

    /**
     * CPU cycles spent inside processReceiveBit() per call (one call per bit position).
     * Sub-range of onTimerAlarmCycles.
     */
    BitBangTimingChannel processReceiveBitCycles;

    void reset() noexcept
    {
        timerJitterUs.reset();
        onTimerAlarmCycles.reset();
        onRxEdgeCycles.reset();
        processStartBitCycles.reset();
        processReceiveBitCycles.reset();
    }
};

} // namespace physical
} // namespace knx

// ---------------------------------------------------------------------------
// Instrumentation macros — only compiled when the feature is enabled.
// ---------------------------------------------------------------------------

/**
 * @brief Capture the current CPU cycle counter into a local variable _t_<tag>.
 * @param tag  Plain C identifier, unique within the function scope.
 */
#define BITBANG_TIMING_BEGIN(tag) \
    const uint32_t _t_##tag = esp_cpu_get_cycle_count()

/**
 * @brief Record elapsed cycles since BITBANG_TIMING_BEGIN(tag) into a channel.
 * @param channel  BitBangTimingChannel lvalue (e.g. _timingStats.onRxEdgeCycles).
 * @param tag      Same identifier used in the matching BITBANG_TIMING_BEGIN(tag).
 */
#define BITBANG_TIMING_END(channel, tag) \
    (channel).record(esp_cpu_get_cycle_count() - _t_##tag)

/**
 * @brief Record an arbitrary uint32_t value directly into a channel.
 * @param channel  BitBangTimingChannel lvalue.
 * @param value    Expression convertible to uint32_t. Evaluated exactly once.
 */
#define BITBANG_TIMING_RECORD(channel, value) \
    (channel).record(static_cast<uint32_t>(value))

#else // CONFIG_KNX_TP1_BITBANG_TIMING_STATS not defined — zero overhead

// All arguments are silently discarded at preprocessing time; the compiler never
// sees them and emits no code.
#define BITBANG_TIMING_BEGIN(tag)             do {} while (0)
#define BITBANG_TIMING_END(channel, tag)      do {} while (0)
#define BITBANG_TIMING_RECORD(channel, value) do {} while (0)

#endif // CONFIG_KNX_TP1_BITBANG_TIMING_STATS
