// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/espidf_gpio_register.hpp"

#ifdef ESP_PLATFORM

#include "knx/physical/espidf_timer_gpio_hal_context.hpp"
#include "knx/physical/timer_gpio_hal.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "soc/gpio_struct.h"

#include <cstdint>

namespace knx {
namespace physical {

/// ISR hardware policy for ESP-IDF.
///
/// Every hot-path operation is a direct register access, always inlined into
/// the ISR core: no vtable, no driver call, no null check. attach() copies the
/// handles out of the HAL context once, at init, so the interrupt reads only
/// these few DRAM words.
class EspIdfIsrHalPolicy {
public:
    inline void attach(const knx_timer_gpio_hal_t& hal, bool txDominantHigh) noexcept
    {
        const auto* context = static_cast<const espidf::TimerGpioHalContext*>(hal.context);
#if KNX_TP1_BITBANG_TIMER_DIRECT
        _timerHw = context->timerHw;
#else
        _timer = context->timer;
#endif
        _txPinMask = context->txPinMask;
        _rxPin = static_cast<uint32_t>(context->rxPin);
        _txDominantHigh = txDominantHigh;
    }

    /// Current timer count in µs. On the register path this latches the
    /// counter (waiting the few cycles the register clock domain needs) and
    /// reads it.
    [[gnu::always_inline]] uint64_t timerNow() const noexcept
    {
#if KNX_TP1_BITBANG_TIMER_DIRECT
        timer_ll_trigger_soft_capture(_timerHw, espidf::kBitBangTimerIndex);
        return timer_ll_get_counter_value(_timerHw, espidf::kBitBangTimerIndex);
#else
        uint64_t now = 0;
        (void)gptimer_get_raw_count(_timer, &now);
        return now;
#endif
    }

    /// Program the alarm for absolute time @p absUs and enable it. The same
    /// register writes gptimer_set_alarm_action() makes without auto-reload;
    /// the hardware disables the alarm when it fires.
    [[gnu::always_inline]] void rearmTimerAbs(uint64_t absUs) noexcept
    {
#if KNX_TP1_BITBANG_TIMER_DIRECT
        timer_ll_set_alarm_value(_timerHw, espidf::kBitBangTimerIndex, absUs);
        timer_ll_enable_alarm(_timerHw, espidf::kBitBangTimerIndex, true);
#else
        // Stack-local: the driver copies it, and a shared static would be
        // written from both task (send) and ISR context.
        gptimer_alarm_config_t alarm{};
        alarm.alarm_count = absUs;
        (void)gptimer_set_alarm_action(_timer, &alarm);
#endif
    }

    /// Mask the driver's interrupts while a task writes ISR-owned state.
    /// Edges and alarms are latched meanwhile and serviced on release.
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
#if KNX_TP1_BITBANG_TIMER_DIRECT
    timg_dev_t*      _timerHw{nullptr};
#else
    gptimer_handle_t _timer{nullptr};
#endif
    uint32_t         _txPinMask{0};
    uint32_t         _rxPin{0};
    bool             _txDominantHigh{false};
    portMUX_TYPE     _mux = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace physical
} // namespace knx

#endif // ESP_PLATFORM
