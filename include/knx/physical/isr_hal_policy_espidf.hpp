// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/espidf_gpio_register.hpp"

#ifdef ESP_PLATFORM

#include "knx/physical/timer_gpio_hal.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "soc/gpio_struct.h"

#include <cstdint>

namespace knx {
namespace physical {

/// ISR hardware policy for ESP-IDF.
///
/// All five hot-path operations call hardware registers or gptimer functions
/// directly — no vtable, no null-checks, no runtime dispatch. Every hot-path
/// method is [[gnu::always_inline]] so the compiler collapses each call site
/// to 1–3 instructions when this type is used as a template argument.
///
/// attach() is called once from init() after startTimer() succeeds. From that
/// point on every ISR reads only DRAM-resident members of this struct.
class EspIdfIsrHalPolicy {
public:
    /// Extracts gptimer handle and GPIO pin configuration from the opaque HAL
    /// context. The local shadow struct must match the first three fields of
    /// EspIdfTimerGpioHalContext in timer_gpio_hal_espidf.cpp at offsets 0, 4, 8.
    /// The static_asserts there enforce this layout contract.
    inline void attach(const knx_timer_gpio_hal_t& hal, bool txDominantHigh) noexcept
    {
        // Shadow of EspIdfTimerGpioHalContext's first three fields:
        //   gptimer_handle_t timer  @ offset 0
        //   uint32_t txPinMask      @ offset 4
        //   int rxPin               @ offset 8
        // static_asserts in timer_gpio_hal_espidf.cpp enforce this contract.
        struct Layout { gptimer_handle_t timer; uint32_t txPinMask; int rxPin; };
        const auto* f = static_cast<const Layout*>(hal.context);
        _timer          = f->timer;
        _txPinMask      = f->txPinMask;
        _rxPin          = f->rxPin;
        _txDominantHigh = txDominantHigh;
    }

    [[gnu::always_inline]] uint64_t timerNow() const noexcept
    {
        uint64_t now = 0;
        (void)gptimer_get_raw_count(_timer, &now);
        return now;
    }

    [[gnu::always_inline]] void rearmTimerAbs(uint64_t absUs) noexcept
    {
        // Stack-local on purpose: gptimer_set_alarm_action() copies the config.
        // A shared static here is written from both task context (send → rearm)
        // and ISR context — an ISR firing between the store and the driver call
        // would arm a stale alarm value.
        gptimer_alarm_config_t alarm{};
        alarm.alarm_count = absUs;
        (void)gptimer_set_alarm_action(_timer, &alarm);
    }

    /// Mask the driver's ISRs while a task mutates ISR-owned state (send()).
    /// Held for a few µs only; the GPIO edge and timer alarm are latched in
    /// hardware and serviced immediately after release.
    void lockFromTask() noexcept { portENTER_CRITICAL(&_mux); }
    void unlockFromTask() noexcept { portEXIT_CRITICAL(&_mux); }

    [[gnu::always_inline]] void setTxDominant() noexcept
    {
        if (_txDominantHigh) {
            espidf::writeGpioRegister(GPIO.out_w1ts, _txPinMask);
        } else {
            espidf::writeGpioRegister(GPIO.out_w1tc, _txPinMask);
        }
    }

    [[gnu::always_inline]] void setTxRecessive() noexcept
    {
        if (_txDominantHigh) {
            espidf::writeGpioRegister(GPIO.out_w1tc, _txPinMask);
        } else {
            espidf::writeGpioRegister(GPIO.out_w1ts, _txPinMask);
        }
    }

    [[gnu::always_inline]] int readRxLevel() const noexcept
    {
        return static_cast<int>((espidf::readGpioRegister(GPIO.in) >> _rxPin) & 1u);
    }

private:
    gptimer_handle_t _timer{nullptr};
    uint32_t         _txPinMask{0};
    int              _rxPin{-1};
    bool             _txDominantHigh{false};
    portMUX_TYPE     _mux = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace physical
} // namespace knx

#endif // ESP_PLATFORM
