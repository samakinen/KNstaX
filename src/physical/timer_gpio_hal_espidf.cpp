// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/physical/timer_gpio_hal.h"
#include "knx/physical/espidf_gpio_register.hpp"

#if defined(ESP_PLATFORM)

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "soc/gpio_struct.h"

#include <new>

// This translation unit lives in the global namespace (it exposes a C ABI), so
// name the register helpers explicitly.
namespace espidf = knx::physical::espidf;

namespace {

// The first three fields must remain in this order and at these offsets.
// EspIdfIsrHalPolicy::attach() in isr_hal_policy_espidf.hpp casts hal.context
// to a local shadow struct of these fields to extract them at init time.
// The static_asserts below enforce this layout contract.
struct EspIdfTimerGpioHalContext {
    gptimer_handle_t timer{nullptr};                          // [0]  used every timer ISR
    uint32_t txPinMask{0};                                    // [4]  precomputed 1u<<txPin
    int rxPin{-1};                                            // [8]  used every edge ISR
    int txPin{-1};
    knx_timer_gpio_hal_timer_alarm_cb_t timerAlarmCb{nullptr};
    void* timerAlarmContext{nullptr};
    knx_timer_gpio_hal_gpio_edge_isr_t rxEdgeIsr{nullptr};
    void* rxEdgeIsrContext{nullptr};
    // Optional link-health input (STKNX KNX_OK). Appended after the fields the
    // ISR policy shadows so the offset contract above stays intact.
    int statusPin{-1};
    knx_timer_gpio_hal_gpio_edge_isr_t statusEdgeIsr{nullptr};
    void* statusEdgeIsrContext{nullptr};
};

// Field-order contract with bitbang_driver_timer_isr.cpp:
static_assert(offsetof(EspIdfTimerGpioHalContext, timer)     == 0,  "timer must be first field");
static_assert(offsetof(EspIdfTimerGpioHalContext, txPinMask) == 4,  "txPinMask must be second field");
static_assert(offsetof(EspIdfTimerGpioHalContext, rxPin)     == 8,  "rxPin must be third field");

// Timer alarm shim — context and callback are always valid while timer runs.
bool IRAM_ATTR onTimerAlarm(gptimer_handle_t,
                            const gptimer_alarm_event_data_t*,
                            void* userContext)
{
    auto* ctx = static_cast<EspIdfTimerGpioHalContext*>(userContext);
    ctx->timerAlarmCb(ctx->timerAlarmContext);
    return false;
}

// GPIO edge shim — context and handler are always valid while isr is installed.
void IRAM_ATTR onRxEdge(void* userContext)
{
    auto* ctx = static_cast<EspIdfTimerGpioHalContext*>(userContext);
    ctx->rxEdgeIsr(ctx->rxEdgeIsrContext);
}

// Status (link-health) edge shim. Same lifetime rules as onRxEdge.
void IRAM_ATTR onStatusEdge(void* userContext)
{
    auto* ctx = static_cast<EspIdfTimerGpioHalContext*>(userContext);
    if (ctx->statusEdgeIsr) {
        ctx->statusEdgeIsr(ctx->statusEdgeIsrContext);
    }
}

bool configurePins(void* opaque,
                   int tx_pin,
                   int rx_pin,
                   bool enable_pullup,
                   knx_timer_gpio_hal_rx_edge_t rx_edge)
{
    auto* context = static_cast<EspIdfTimerGpioHalContext*>(opaque);
    if (!context || tx_pin < 0 || rx_pin < 0) {
        return false;
    }

    context->txPin     = tx_pin;
    context->rxPin     = rx_pin;
    context->txPinMask = (1u << static_cast<unsigned>(tx_pin));

    gpio_config_t tx{};
    tx.pin_bit_mask = (1ULL << static_cast<uint32_t>(tx_pin));
    tx.mode = GPIO_MODE_OUTPUT;
    tx.pull_up_en = GPIO_PULLUP_DISABLE;
    tx.pull_down_en = GPIO_PULLDOWN_DISABLE;
    tx.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&tx) != ESP_OK) {
        return false;
    }

    gpio_config_t rx{};
    rx.pin_bit_mask = (1ULL << static_cast<uint32_t>(rx_pin));
    rx.mode = GPIO_MODE_INPUT;
    rx.pull_up_en = enable_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    rx.pull_down_en = GPIO_PULLDOWN_DISABLE;
    switch (rx_edge) {
        case KNX_TIMER_GPIO_HAL_RX_EDGE_RISING:
            rx.intr_type = GPIO_INTR_POSEDGE;
            break;
        case KNX_TIMER_GPIO_HAL_RX_EDGE_FALLING:
            rx.intr_type = GPIO_INTR_NEGEDGE;
            break;
        case KNX_TIMER_GPIO_HAL_RX_EDGE_ANY:
        default:
            rx.intr_type = GPIO_INTR_ANYEDGE;
            break;
    }
    if (gpio_config(&rx) != ESP_OK) {
        return false;
    }

    return gpio_set_level(static_cast<gpio_num_t>(tx_pin), 0) == ESP_OK;
}

bool ensureGpioIsrService()
{
    // Always ensure the global GPIO ISR service exists before attaching
    // handlers. When app firmware does not install it, KNStaX must do so.
    //
    // ESP_INTR_FLAG_IRAM is NOT optional for this driver and is therefore ORed
    // in rather than left to Kconfig: without it esp_intr_alloc leaves the GPIO
    // interrupt *disabled* for the whole time the flash cache is off, which is
    // exactly what an NVS commit during ETS commissioning does. A stalled RX
    // edge interrupt means missed TP1 start bits and a shortened dominant pulse
    // on the bit being driven — telegrams silently lost for the duration of the
    // erase/write. The first caller's flags win (later calls return
    // ESP_ERR_INVALID_STATE), so this must be right on the first install.
    //
    // The flag obliges EVERY handler attached to this shared service to be
    // IRAM-resident and to touch no flash-mapped data; onRxEdge/onStatusEdge
    // below satisfy that, and so must any application handler added later.
    const int installFlags = CONFIG_KNX_TP1_BITBANG_ISR_SERVICE_FLAGS | ESP_INTR_FLAG_IRAM;

    const auto installResult = gpio_install_isr_service(installFlags);
    return installResult == ESP_OK || installResult == ESP_ERR_INVALID_STATE;
}

bool installRxEdgeIsr(void* opaque, knx_timer_gpio_hal_gpio_edge_isr_t isr, void* isr_context)
{
    auto* context = static_cast<EspIdfTimerGpioHalContext*>(opaque);
    if (!context || context->rxPin < 0) {
        return false;
    }

    context->rxEdgeIsr = isr;
    context->rxEdgeIsrContext = isr_context;

    if (!ensureGpioIsrService()) {
        return false;
    }

    return gpio_isr_handler_add(static_cast<gpio_num_t>(context->rxPin), onRxEdge, context) == ESP_OK;
}

void removeRxEdgeIsr(void* opaque)
{
    auto* context = static_cast<EspIdfTimerGpioHalContext*>(opaque);
    if (!context || context->rxPin < 0) {
        return;
    }

    (void)gpio_isr_handler_remove(static_cast<gpio_num_t>(context->rxPin));
    context->rxEdgeIsr = nullptr;
    context->rxEdgeIsrContext = nullptr;
}

bool configureStatusPin(void* opaque, int status_pin, bool enable_pullup)
{
    auto* context = static_cast<EspIdfTimerGpioHalContext*>(opaque);
    if (!context || status_pin < 0) {
        return false;
    }

    gpio_config_t status{};
    status.pin_bit_mask = (1ULL << static_cast<uint32_t>(status_pin));
    status.mode = GPIO_MODE_INPUT;
    status.pull_up_en = enable_pullup ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    status.pull_down_en = GPIO_PULLDOWN_DISABLE;
    // Both edges: loss and return of the signal are equally interesting.
    status.intr_type = GPIO_INTR_ANYEDGE;
    if (gpio_config(&status) != ESP_OK) {
        return false;
    }

    context->statusPin = status_pin;
    return true;
}

bool installStatusEdgeIsr(void* opaque, knx_timer_gpio_hal_gpio_edge_isr_t isr, void* isr_context)
{
    auto* context = static_cast<EspIdfTimerGpioHalContext*>(opaque);
    if (!context || context->statusPin < 0) {
        return false;
    }

    context->statusEdgeIsr = isr;
    context->statusEdgeIsrContext = isr_context;

    if (!ensureGpioIsrService()) {
        return false;
    }

    return gpio_isr_handler_add(static_cast<gpio_num_t>(context->statusPin), onStatusEdge, context) == ESP_OK;
}

void removeStatusEdgeIsr(void* opaque)
{
    auto* context = static_cast<EspIdfTimerGpioHalContext*>(opaque);
    if (!context || context->statusPin < 0) {
        return;
    }

    (void)gpio_isr_handler_remove(static_cast<gpio_num_t>(context->statusPin));
    context->statusEdgeIsr = nullptr;
    context->statusEdgeIsrContext = nullptr;
}

bool startTimer(void* opaque,
                knx_timer_gpio_hal_timer_alarm_cb_t alarm_cb,
                void* alarm_context)
{
    auto* context = static_cast<EspIdfTimerGpioHalContext*>(opaque);
    if (!context || !alarm_cb) {
        return false;
    }

    context->timerAlarmCb = alarm_cb;
    context->timerAlarmContext = alarm_context;

    if (!context->timer) {
        gptimer_config_t config{};
        config.clk_src       = GPTIMER_CLK_SRC_DEFAULT;
        config.direction     = GPTIMER_COUNT_UP;
        config.resolution_hz = 1000000;
        config.intr_priority = 3;  // level 3 = same as GPIO ISR service (no mutual preemption)

        if (gptimer_new_timer(&config, &context->timer) != ESP_OK) {
            return false;
        }

        gptimer_event_callbacks_t callbacks{};
        callbacks.on_alarm = onTimerAlarm;
        if (gptimer_register_event_callbacks(context->timer, &callbacks, context) != ESP_OK) {
            return false;
        }

        if (gptimer_enable(context->timer) != ESP_OK) {
            return false;
        }
    }

    return gptimer_start(context->timer) == ESP_OK;
}

bool stopTimer(void* opaque)
{
    auto* context = static_cast<EspIdfTimerGpioHalContext*>(opaque);
    if (!context || !context->timer) {
        return false;
    }

    return gptimer_stop(context->timer) == ESP_OK;
}

// Called from ISR context on every timer event — must be in IRAM.
// Stack-local config on purpose: gptimer_set_alarm_action() copies it, and a
// shared static would be a torn-write hazard between task and ISR callers.
bool IRAM_ATTR rearmTimerAbsUs(void* opaque, uint64_t alarm_time_us)
{
    gptimer_alarm_config_t alarm{};
    alarm.alarm_count = alarm_time_us;
    return gptimer_set_alarm_action(static_cast<EspIdfTimerGpioHalContext*>(opaque)->timer,
                                    &alarm) == ESP_OK;
}

// Called from ISR context — must be in IRAM. Context is always valid here.
uint64_t IRAM_ATTR timerNowUs(void* opaque)
{
    uint64_t now = 0;
    (void)gptimer_get_raw_count(static_cast<EspIdfTimerGpioHalContext*>(opaque)->timer, &now);
    return now;
}

// Direct GPIO register writes: atomic, no cache miss, no function-call overhead.
void IRAM_ATTR setTxHighFast(void* opaque)
{
    espidf::writeGpioRegister(GPIO.out_w1ts, static_cast<EspIdfTimerGpioHalContext*>(opaque)->txPinMask);
}

void IRAM_ATTR setTxLowFast(void* opaque)
{
    espidf::writeGpioRegister(GPIO.out_w1tc, static_cast<EspIdfTimerGpioHalContext*>(opaque)->txPinMask);
}

int IRAM_ATTR readRxLevelFast(void* opaque)
{
    const auto* ctx = static_cast<const EspIdfTimerGpioHalContext*>(opaque);
    return (static_cast<int>(espidf::readGpioRegister(GPIO.in)) >> ctx->rxPin) & 1;
}

// Reachable from the status edge ISR, so IRAM like the rest of the fast reads.
int IRAM_ATTR readStatusLevelFast(void* opaque)
{
    const auto* ctx = static_cast<const EspIdfTimerGpioHalContext*>(opaque);
    if (ctx->statusPin < 0) {
        return 0;
    }
    return (static_cast<int>(espidf::readGpioRegister(GPIO.in)) >> ctx->statusPin) & 1;
}

} // namespace

extern "C" bool knx_timer_gpio_hal_make_espidf(knx_timer_gpio_hal_t* outHal)
{
    if (!outHal) {
        return false;
    }

    auto* context = new (std::nothrow) EspIdfTimerGpioHalContext{};
    if (!context) {
        return false;
    }

    outHal->context = context;
    outHal->ops.configure_pins = configurePins;
    outHal->ops.install_rx_edge_isr = installRxEdgeIsr;
    outHal->ops.remove_rx_edge_isr = removeRxEdgeIsr;
    outHal->ops.start_timer = startTimer;
    outHal->ops.stop_timer = stopTimer;
    outHal->ops.rearm_timer_abs_us = rearmTimerAbsUs;
    outHal->ops.timer_now_us = timerNowUs;
    outHal->ops.set_tx_high_fast = setTxHighFast;
    outHal->ops.set_tx_low_fast = setTxLowFast;
    outHal->ops.read_rx_level_fast = readRxLevelFast;
    outHal->ops.configure_status_pin = configureStatusPin;
    outHal->ops.install_status_edge_isr = installStatusEdgeIsr;
    outHal->ops.remove_status_edge_isr = removeStatusEdgeIsr;
    outHal->ops.read_status_level_fast = readStatusLevelFast;
    return true;
}

extern "C" bool knx_timer_gpio_hal_destroy_espidf(knx_timer_gpio_hal_t* hal)
{
    if (!hal || !hal->context) {
        return false;
    }

    auto* context = static_cast<EspIdfTimerGpioHalContext*>(hal->context);
    if (context->timer) {
        (void)gptimer_stop(context->timer);
        (void)gptimer_disable(context->timer);
        (void)gptimer_del_timer(context->timer);
        context->timer = nullptr;
    }

    if (context->rxPin >= 0) {
        (void)gpio_isr_handler_remove(static_cast<gpio_num_t>(context->rxPin));
    }

    if (context->statusPin >= 0) {
        (void)gpio_isr_handler_remove(static_cast<gpio_num_t>(context->statusPin));
    }

    delete context;

    hal->context = nullptr;
    hal->ops = knx_timer_gpio_hal_ops_t{};
    return true;
}

#else

extern "C" bool knx_timer_gpio_hal_make_espidf(knx_timer_gpio_hal_t*)
{
    return false;
}

extern "C" bool knx_timer_gpio_hal_destroy_espidf(knx_timer_gpio_hal_t*)
{
    return false;
}

#endif
