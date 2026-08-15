// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/timer_gpio_hal.h"
#include "knx/platform/virtual_test_clock.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace knx {
namespace physical {

class TimerGpioHalVirtualBus {
public:
    enum class TxSource : uint8_t {
        Driver = 0,
    };

    struct TxTransition {
        uint64_t timestampUs{0};
        uint16_t pin{0};
        uint8_t level{0};
        TxSource source{TxSource::Driver};
    };

    TimerGpioHalVirtualBus();
    ~TimerGpioHalVirtualBus();

    bool bind(knx_timer_gpio_hal_t& outHal);

    void reset();
    void advanceTimeUs(uint64_t deltaUs);
    void syncToUs(uint64_t targetUs);
    uint64_t nowUs() const;

    bool attachClock(platform::VirtualTestClock& clock);
    void detachClock();
    bool isAttachedToClock() const;

    bool scheduleRxLevelAtUs(uint64_t timestampUs, uint8_t level);
    bool scheduleRxLevelAfterUs(uint64_t delayUs, uint8_t level);

    void clearCapturedTxTransitions();
    const std::vector<TxTransition>& capturedTxTransitions() const;
    void setRxEdgeMasked(bool masked);
    bool isRxEdgeMasked() const;

    uint8_t txLevel() const;
    uint8_t rxLevel() const;

    /// Drive the optional link-health input (STKNX KNX_OK equivalent) and fire
    /// its edge ISR, so link-state handling can be exercised without hardware.
    void setStatusLevel(uint8_t level);
    uint8_t statusLevel() const;

private:
    struct ScheduledRxLevel {
        uint64_t timestampUs{0};
        uint8_t level{1};
    };

    uint64_t _nowUs;
    int _txPin;
    int _rxPin;
    bool _pullupEnabled;
    knx_timer_gpio_hal_rx_edge_t _rxEdgeTrigger;

    bool _timerStarted;
    bool _timerArmed;
    uint64_t _timerAlarmUs;
    knx_timer_gpio_hal_timer_alarm_cb_t _timerAlarmCb;
    void* _timerAlarmContext;

    knx_timer_gpio_hal_gpio_edge_isr_t _rxEdgeIsr;
    void* _rxEdgeIsrContext;
    bool _rxEdgeMasked;

    int _statusPin;
    bool _statusPullupEnabled;
    knx_timer_gpio_hal_gpio_edge_isr_t _statusEdgeIsr;
    void* _statusEdgeIsrContext;
    uint8_t _statusLevel;

    uint8_t _txLevel;
    uint8_t _rxLevel;

    platform::VirtualTestClock* _sharedClock;
    platform::VirtualTestClock::ObserverId _clockObserverId;

    std::vector<ScheduledRxLevel> _scheduledRxLevels;
    std::vector<TxTransition> _txTransitions;

    void runUntilUs(uint64_t targetUs);
    void onClockAdvanced(uint64_t nowUs);
    bool popNextScheduledRx(ScheduledRxLevel& outEvent, uint64_t targetUs);
    void applyRxLevel(uint8_t level);
    void captureTxLevel(uint8_t level);

    static bool configurePinsShim(void* context,
                                  int tx_pin,
                                  int rx_pin,
                                  bool enable_pullup,
                                  knx_timer_gpio_hal_rx_edge_t rx_edge);
    static bool installRxEdgeIsrShim(void* context,
                                     knx_timer_gpio_hal_gpio_edge_isr_t isr,
                                     void* isr_context);
    static void removeRxEdgeIsrShim(void* context);
    static bool startTimerShim(void* context,
                               knx_timer_gpio_hal_timer_alarm_cb_t alarm_cb,
                               void* alarm_context);
    static bool stopTimerShim(void* context);
    static bool rearmTimerAbsUsShim(void* context, uint64_t alarm_time_us);
    static uint64_t timerNowUsShim(void* context);
    static void setTxHighFastShim(void* context);
    static void setTxLowFastShim(void* context);
    static int readRxLevelFastShim(void* context);
    static bool configureStatusPinShim(void* context, int status_pin, bool enable_pullup);
    static bool installStatusEdgeIsrShim(void* context,
                                         knx_timer_gpio_hal_gpio_edge_isr_t isr,
                                         void* isr_context);
    static void removeStatusEdgeIsrShim(void* context);
    static int readStatusLevelFastShim(void* context);
};

} // namespace physical
} // namespace knx
