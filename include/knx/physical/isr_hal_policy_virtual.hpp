// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/timer_gpio_hal.h"

#include <cstdint>

namespace knx {
namespace physical {

/// ISR hardware policy for non-ESP platforms (test/simulation builds).
///
/// Routes all hot-path hardware operations through the knx_timer_gpio_hal_t
/// vtable, enabling mock injection in unit tests. Performance is not a concern
/// on the host; vtable dispatch is acceptable here.
///
/// attach() is called once from init() after startTimer() succeeds.
class VirtualIsrHalPolicy {
public:
    inline void attach(const knx_timer_gpio_hal_t& hal, bool txDominantHigh) noexcept
    {
        _hal            = &hal;
        _txDominantHigh = txDominantHigh;
    }

    uint64_t timerNow() const noexcept
    {
        return _hal->ops.timer_now_us(_hal->context);
    }

    void rearmTimerAbs(uint64_t absUs) noexcept
    {
        (void)_hal->ops.rearm_timer_abs_us(_hal->context, absUs);
    }

    void setTxDominant() noexcept
    {
        if (_txDominantHigh) {
            (void)_hal->ops.set_tx_high_fast(_hal->context);
        } else {
            (void)_hal->ops.set_tx_low_fast(_hal->context);
        }
    }

    void setTxRecessive() noexcept
    {
        if (_txDominantHigh) {
            (void)_hal->ops.set_tx_low_fast(_hal->context);
        } else {
            (void)_hal->ops.set_tx_high_fast(_hal->context);
        }
    }

    int readRxLevel() const noexcept
    {
        return _hal->ops.read_rx_level_fast(_hal->context);
    }

    /// Task↔ISR critical section — no-op on the host: virtual-time tests run
    /// the "ISR" callbacks synchronously on the calling thread.
    void lockFromTask() noexcept {}
    void unlockFromTask() noexcept {}

private:
    const knx_timer_gpio_hal_t* _hal{nullptr};
    bool                        _txDominantHigh{false};
};

} // namespace physical
} // namespace knx
