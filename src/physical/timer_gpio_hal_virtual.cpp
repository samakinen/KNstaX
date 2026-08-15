// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/timer_gpio_hal_virtual.hpp"

#include <algorithm>
#include <limits>

namespace knx {
namespace physical {

TimerGpioHalVirtualBus::TimerGpioHalVirtualBus()
    : _nowUs(0)
    , _txPin(-1)
    , _rxPin(-1)
    , _pullupEnabled(false)
    , _rxEdgeTrigger(KNX_TIMER_GPIO_HAL_RX_EDGE_ANY)
    , _timerStarted(false)
    , _timerArmed(false)
    , _timerAlarmUs(0)
    , _timerAlarmCb(nullptr)
    , _timerAlarmContext(nullptr)
    , _rxEdgeIsr(nullptr)
    , _rxEdgeIsrContext(nullptr)
    , _rxEdgeMasked(false)
    , _statusPin(-1)
    , _statusPullupEnabled(false)
    , _statusEdgeIsr(nullptr)
    , _statusEdgeIsrContext(nullptr)
    , _statusLevel(1)
    , _txLevel(0)
    , _rxLevel(1)
    , _sharedClock(nullptr)
    , _clockObserverId(0)
    , _scheduledRxLevels()
    , _txTransitions()
{
}

TimerGpioHalVirtualBus::~TimerGpioHalVirtualBus()
{
    detachClock();
}

bool TimerGpioHalVirtualBus::bind(knx_timer_gpio_hal_t& outHal)
{
    outHal.context = this;
    outHal.ops.configure_pins = &configurePinsShim;
    outHal.ops.install_rx_edge_isr = &installRxEdgeIsrShim;
    outHal.ops.remove_rx_edge_isr = &removeRxEdgeIsrShim;
    outHal.ops.start_timer = &startTimerShim;
    outHal.ops.stop_timer = &stopTimerShim;
    outHal.ops.rearm_timer_abs_us = &rearmTimerAbsUsShim;
    outHal.ops.timer_now_us = &timerNowUsShim;
    outHal.ops.set_tx_high_fast = &setTxHighFastShim;
    outHal.ops.set_tx_low_fast = &setTxLowFastShim;
    outHal.ops.read_rx_level_fast = &readRxLevelFastShim;
    outHal.ops.configure_status_pin = &configureStatusPinShim;
    outHal.ops.install_status_edge_isr = &installStatusEdgeIsrShim;
    outHal.ops.remove_status_edge_isr = &removeStatusEdgeIsrShim;
    outHal.ops.read_status_level_fast = &readStatusLevelFastShim;
    return true;
}

void TimerGpioHalVirtualBus::reset()
{
    _nowUs = 0;
    _txPin = -1;
    _rxPin = -1;
    _pullupEnabled = false;
    _rxEdgeTrigger = KNX_TIMER_GPIO_HAL_RX_EDGE_ANY;
    _timerStarted = false;
    _timerArmed = false;
    _timerAlarmUs = 0;
    _timerAlarmCb = nullptr;
    _timerAlarmContext = nullptr;
    _rxEdgeIsr = nullptr;
    _rxEdgeIsrContext = nullptr;
    _rxEdgeMasked = false;
    _statusPin = -1;
    _statusPullupEnabled = false;
    _statusEdgeIsr = nullptr;
    _statusEdgeIsrContext = nullptr;
    _statusLevel = 1;
    _txLevel = 0;
    _rxLevel = 1;
    _scheduledRxLevels.clear();
    _txTransitions.clear();

    if (_sharedClock != nullptr) {
        syncToUs(_sharedClock->nowUs());
    }
}

void TimerGpioHalVirtualBus::advanceTimeUs(uint64_t deltaUs)
{
    const uint64_t targetUs = (_nowUs > std::numeric_limits<uint64_t>::max() - deltaUs)
                                  ? std::numeric_limits<uint64_t>::max()
                                  : _nowUs + deltaUs;
    runUntilUs(targetUs);
}

void TimerGpioHalVirtualBus::syncToUs(uint64_t targetUs)
{
    runUntilUs(targetUs);
}

uint64_t TimerGpioHalVirtualBus::nowUs() const
{
    return _nowUs;
}

bool TimerGpioHalVirtualBus::attachClock(platform::VirtualTestClock& clock)
{
    if (_sharedClock == &clock && _clockObserverId != 0) {
        syncToUs(clock.nowUs());
        return true;
    }

    detachClock();

    _sharedClock = &clock;
    _clockObserverId = clock.addAdvanceObserver([this](uint64_t nowUs) {
        this->onClockAdvanced(nowUs);
    });

    if (_clockObserverId == 0) {
        _sharedClock = nullptr;
        return false;
    }

    syncToUs(clock.nowUs());
    return true;
}

void TimerGpioHalVirtualBus::detachClock()
{
    if (_sharedClock != nullptr && _clockObserverId != 0) {
        (void)_sharedClock->removeAdvanceObserver(_clockObserverId);
    }

    _sharedClock = nullptr;
    _clockObserverId = 0;
}

bool TimerGpioHalVirtualBus::isAttachedToClock() const
{
    return _sharedClock != nullptr && _clockObserverId != 0;
}

bool TimerGpioHalVirtualBus::scheduleRxLevelAtUs(uint64_t timestampUs, uint8_t level)
{
    ScheduledRxLevel event;
    event.timestampUs = timestampUs;
    event.level = static_cast<uint8_t>(level & 0x1u);

    auto position = std::upper_bound(
        _scheduledRxLevels.begin(),
        _scheduledRxLevels.end(),
        event.timestampUs,
        [](uint64_t lhsTimestampUs, const ScheduledRxLevel& rhs) {
            return lhsTimestampUs < rhs.timestampUs;
        });

    _scheduledRxLevels.insert(position, event);
    return true;
}

bool TimerGpioHalVirtualBus::scheduleRxLevelAfterUs(uint64_t delayUs, uint8_t level)
{
    if (_nowUs > std::numeric_limits<uint64_t>::max() - delayUs) {
        return false;
    }
    return scheduleRxLevelAtUs(_nowUs + delayUs, level);
}

void TimerGpioHalVirtualBus::clearCapturedTxTransitions()
{
    _txTransitions.clear();
}

const std::vector<TimerGpioHalVirtualBus::TxTransition>& TimerGpioHalVirtualBus::capturedTxTransitions() const
{
    return _txTransitions;
}

void TimerGpioHalVirtualBus::setRxEdgeMasked(bool masked)
{
    _rxEdgeMasked = masked;
}

bool TimerGpioHalVirtualBus::isRxEdgeMasked() const
{
    return _rxEdgeMasked;
}

uint8_t TimerGpioHalVirtualBus::txLevel() const
{
    return _txLevel;
}

uint8_t TimerGpioHalVirtualBus::rxLevel() const
{
    return _rxLevel;
}

void TimerGpioHalVirtualBus::setStatusLevel(uint8_t level)
{
    const uint8_t normalized = (level != 0) ? 1u : 0u;
    if (_statusLevel == normalized) {
        return;
    }

    _statusLevel = normalized;
    if (_statusEdgeIsr != nullptr) {
        _statusEdgeIsr(_statusEdgeIsrContext);
    }
}

uint8_t TimerGpioHalVirtualBus::statusLevel() const
{
    return _statusLevel;
}

void TimerGpioHalVirtualBus::runUntilUs(uint64_t targetUs)
{
    if (targetUs <= _nowUs) {
        return;
    }

    while (_nowUs < targetUs) {
        uint64_t nextEventUs = targetUs;

        if (!_scheduledRxLevels.empty()) {
            nextEventUs = std::min(nextEventUs, _scheduledRxLevels.front().timestampUs);
        }

        if (_timerStarted && _timerArmed) {
            nextEventUs = std::min(nextEventUs, _timerAlarmUs);
        }

        if (nextEventUs > _nowUs) {
            _nowUs = nextEventUs;
        }

        ScheduledRxLevel rxEvent;
        while (popNextScheduledRx(rxEvent, _nowUs)) {
            applyRxLevel(rxEvent.level);
        }

        while (_timerStarted && _timerArmed && _timerAlarmCb && _timerAlarmUs <= _nowUs) {
            _timerArmed = false;
            _timerAlarmCb(_timerAlarmContext);
        }

        if (nextEventUs == targetUs) {
            break;
        }
    }

    _nowUs = targetUs;
}

void TimerGpioHalVirtualBus::onClockAdvanced(uint64_t nowUs)
{
    syncToUs(nowUs);
}

bool TimerGpioHalVirtualBus::popNextScheduledRx(ScheduledRxLevel& outEvent, uint64_t targetUs)
{
    if (_scheduledRxLevels.empty()) {
        return false;
    }

    if (_scheduledRxLevels.front().timestampUs > targetUs) {
        return false;
    }

    outEvent = _scheduledRxLevels.front();
    _scheduledRxLevels.erase(_scheduledRxLevels.begin());
    return true;
}

void TimerGpioHalVirtualBus::applyRxLevel(uint8_t level)
{
    level = static_cast<uint8_t>(level & 0x1u);
    if (_rxLevel == level) {
        return;
    }

    const uint8_t previousLevel = _rxLevel;
    _rxLevel = level;
    const bool risingEdge = previousLevel == 0u && level != 0u;
    const bool fallingEdge = previousLevel != 0u && level == 0u;
    const bool edgeMatches = _rxEdgeTrigger == KNX_TIMER_GPIO_HAL_RX_EDGE_ANY ||
                             (_rxEdgeTrigger == KNX_TIMER_GPIO_HAL_RX_EDGE_RISING && risingEdge) ||
                             (_rxEdgeTrigger == KNX_TIMER_GPIO_HAL_RX_EDGE_FALLING && fallingEdge);

    if (_rxEdgeIsr && !_rxEdgeMasked && edgeMatches) {
        _rxEdgeIsr(_rxEdgeIsrContext);
    }
}

void TimerGpioHalVirtualBus::captureTxLevel(uint8_t level)
{
    level = static_cast<uint8_t>(level & 0x1u);
    if (_txLevel == level) {
        return;
    }

    _txLevel = level;
    const uint16_t pin = (_txPin >= 0) ? static_cast<uint16_t>(_txPin) : 0u;
    _txTransitions.push_back(TxTransition{_nowUs, pin, _txLevel, TxSource::Driver});
}

bool TimerGpioHalVirtualBus::configurePinsShim(void* context,
                                               int tx_pin,
                                               int rx_pin,
                                               bool enable_pullup,
                                               knx_timer_gpio_hal_rx_edge_t rx_edge)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return false;
    }

    self->_txPin = tx_pin;
    self->_rxPin = rx_pin;
    self->_pullupEnabled = enable_pullup;
    self->_rxEdgeTrigger = rx_edge;
    return true;
}

bool TimerGpioHalVirtualBus::installRxEdgeIsrShim(void* context,
                                                  knx_timer_gpio_hal_gpio_edge_isr_t isr,
                                                  void* isr_context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return false;
    }

    self->_rxEdgeIsr = isr;
    self->_rxEdgeIsrContext = isr_context;
    return true;
}

void TimerGpioHalVirtualBus::removeRxEdgeIsrShim(void* context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return;
    }

    self->_rxEdgeIsr = nullptr;
    self->_rxEdgeIsrContext = nullptr;
}

bool TimerGpioHalVirtualBus::startTimerShim(void* context,
                                            knx_timer_gpio_hal_timer_alarm_cb_t alarm_cb,
                                            void* alarm_context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self || !alarm_cb) {
        return false;
    }

    self->_timerStarted = true;
    self->_timerArmed = false;
    self->_timerAlarmUs = 0;
    self->_timerAlarmCb = alarm_cb;
    self->_timerAlarmContext = alarm_context;
    return true;
}

bool TimerGpioHalVirtualBus::stopTimerShim(void* context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return false;
    }

    self->_timerStarted = false;
    self->_timerArmed = false;
    return true;
}

bool TimerGpioHalVirtualBus::rearmTimerAbsUsShim(void* context, uint64_t alarm_time_us)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self || !self->_timerStarted || !self->_timerAlarmCb) {
        return false;
    }

    self->_timerArmed = true;
    self->_timerAlarmUs = alarm_time_us;
    return true;
}

uint64_t TimerGpioHalVirtualBus::timerNowUsShim(void* context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return 0;
    }
    return self->_nowUs;
}

void TimerGpioHalVirtualBus::setTxHighFastShim(void* context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return;
    }
    self->captureTxLevel(1);
}

void TimerGpioHalVirtualBus::setTxLowFastShim(void* context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return;
    }
    self->captureTxLevel(0);
}

int TimerGpioHalVirtualBus::readRxLevelFastShim(void* context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return 0;
    }
    return self->_rxLevel;
}

bool TimerGpioHalVirtualBus::configureStatusPinShim(void* context, int status_pin, bool enable_pullup)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self || status_pin < 0) {
        return false;
    }

    self->_statusPin = status_pin;
    self->_statusPullupEnabled = enable_pullup;
    return true;
}

bool TimerGpioHalVirtualBus::installStatusEdgeIsrShim(void* context,
                                                      knx_timer_gpio_hal_gpio_edge_isr_t isr,
                                                      void* isr_context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return false;
    }

    self->_statusEdgeIsr = isr;
    self->_statusEdgeIsrContext = isr_context;
    return true;
}

void TimerGpioHalVirtualBus::removeStatusEdgeIsrShim(void* context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return;
    }

    self->_statusEdgeIsr = nullptr;
    self->_statusEdgeIsrContext = nullptr;
}

int TimerGpioHalVirtualBus::readStatusLevelFastShim(void* context)
{
    auto* self = static_cast<TimerGpioHalVirtualBus*>(context);
    if (!self) {
        return 0;
    }
    return self->_statusLevel;
}

} // namespace physical
} // namespace knx
