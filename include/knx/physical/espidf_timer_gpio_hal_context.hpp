// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file espidf_timer_gpio_hal_context.hpp
 * @brief State of the ESP-IDF timer/GPIO HAL, shared by the HAL
 *        (timer_gpio_hal_espidf.cpp) and the ISR hot-path policy
 *        (isr_hal_policy_espidf.hpp).
 */

#pragma once

#ifdef ESP_PLATFORM

#include "knx/physical/timer_gpio_hal.h"

#include "driver/gptimer.h"
#include "esp_intr_alloc.h"

// ESP-IDF's low-level timer header assigns uint32_t to narrow bit-fields; this
// component builds with -Wconversion -Werror.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#include "hal/timer_ll.h"
#pragma GCC diagnostic pop

#include <cstdint>

/// Whether the bit-bang timer is driven at register level.
///
/// The gptimer driver allocates and clocks the timer either way. Where a timer
/// group has a single timer (ESP32-C2, C3, C5, C6, C61, H2) its index is
/// implied, so the ISR reads and rearms the timer through the TIMG registers
/// and services the alarm from its own interrupt — no driver call, no
/// spinlock, no argument checks on the hot path. On chips with two timers per
/// group the driver does not reveal which one it allocated, and the gptimer
/// API is used instead.
#if TIMG_LL_GPTIMERS_PER_INST == 1
#define KNX_TP1_BITBANG_TIMER_DIRECT 1
#else
#define KNX_TP1_BITBANG_TIMER_DIRECT 0
#endif

namespace knx {
namespace physical {
namespace espidf {

#if KNX_TP1_BITBANG_TIMER_DIRECT
/// The only timer of its group.
inline constexpr uint32_t kBitBangTimerIndex = 0;
#endif

struct TimerGpioHalContext {
    gptimer_handle_t timer{nullptr};
#if KNX_TP1_BITBANG_TIMER_DIRECT
    timg_dev_t* timerHw{nullptr};      ///< register block of the timer's group
    intr_handle_t timerIntr{nullptr};  ///< the alarm interrupt this HAL owns
#endif
    uint32_t txPinMask{0};             ///< 1 << txPin
    int rxPin{-1};
    int txPin{-1};
    knx_timer_gpio_hal_timer_alarm_cb_t timerAlarmCb{nullptr};
    void* timerAlarmContext{nullptr};
    knx_timer_gpio_hal_gpio_edge_isr_t rxEdgeIsr{nullptr};
    void* rxEdgeIsrContext{nullptr};
    /// Optional link-health input (STKNX KNX_OK).
    int statusPin{-1};
    knx_timer_gpio_hal_gpio_edge_isr_t statusEdgeIsr{nullptr};
    void* statusEdgeIsrContext{nullptr};
};

} // namespace espidf
} // namespace physical
} // namespace knx

#endif // ESP_PLATFORM
