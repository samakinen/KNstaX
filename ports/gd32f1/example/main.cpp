// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file main.cpp
 * @brief The GD32F1 port example: KNstaX over a TP-UART2 on a GD32F103
 *
 * What every bare-metal KNstaX product does, in the order it must be done:
 *
 *   1. Outputs safe before anything else (gd32f1_early_init).
 *   2. Clock and time, then mount the flash log and finish an erase a reset
 *      interrupted, before the stack's first save.
 *   3. A CooperativePlatform from the board's time and restart hooks.
 *   4. The TP-UART backend over the board's UART HAL, with the SAVE early
 *      warning if the board wires SAVE.
 *   5. startCommissionedProduct, the stack's clock, the erase guard, and the
 *      heap locked: nothing allocates after start-up.
 *   6. One loop: the stack, the flash log's background step, the board's
 *      inputs, the watchdog, then sleep until the next interrupt.
 *
 * RTT log: start-up, lifecycle changes and the switch. RTT commands: p toggles
 * programming mode, s prints statistics.
 */

#include "product.hpp"

#include "gd32f1_fmc.h"
#include "gd32f1_port_config.h"
#include "gd32f1_system.h"
#include "gd32f1_tpuart.h"
#include "rtt.h"

#include "knx/objects/flash_log.hpp"
#include "knx/objects/flash_log_persistence.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/physical/tpuart_medium_backend.hpp"
#include "knx/platform/cooperative_platform.hpp"
#include "knx/util/heap.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

using namespace example;

// 1. Runs from Reset_Handler before .data and .bss exist: registers only.
extern "C" void gd32f1_early_init(void) {
    *reinterpret_cast<volatile uint32_t*>(0x40021018u) |= 1u << (2u + EXAMPLE_OUTPUT_PORT);  // port clock
    gd32f1_gpio_write(EXAMPLE_OUTPUT_PORT, EXAMPLE_OUTPUT_PIN, false);
    gd32f1_gpio_mode(EXAMPLE_OUTPUT_PORT, EXAMPLE_OUTPUT_PIN, GD32F1_GPIO_OUTPUT_2MHZ);
}

namespace {

void rttLog(const char* text) {
    rtt_write(text, static_cast<uint32_t>(std::strlen(text)));
    rtt_write("\n", 1u);
}

void logNumber(const char* label, uint32_t value) {
    char digits[11];
    int n = 0;
    do {
        digits[n++] = static_cast<char>('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);
    rtt_write(label, static_cast<uint32_t>(std::strlen(label)));
    while (n > 0) rtt_write(&digits[--n], 1u);
    rtt_write("\n", 1u);
}

// Platform hooks: the board's time, serial number and restart.
uint32_t hookMillis(void*) { return gd32f1_millis(); }
uint64_t hookMicros(void*) { return gd32f1_micros(); }
void hookIdle(void*) { asm volatile("wfi"); }
uint32_t hookSerial(void*) { return gd32f1_serial_number(); }
void hookRestart(void*) { gd32f1_restart(); }
void hookFatal(void*) {
    rttLog("fatal error in the stack");
    gd32f1_restart();
}
void hookLogLine(void*, const char* text) { rttLog(text); }

knx::objects::FlashLog g_flashLog(gd32f1_fmc_hal());

std::unique_ptr<knx::objects::ObjectPersistence> makePersistence() {
    return std::make_unique<knx::objects::FlashLogPersistence>(g_flashLog);
}

bool g_output = false;
bool g_statusDirty = false;

void setOutput(bool on) {
    g_output = on;
    gd32f1_gpio_write(EXAMPLE_OUTPUT_PORT, EXAMPLE_OUTPUT_PIN, on);
    g_statusDirty = true;
    rttLog(on ? "switch on" : "switch off");
}

bool buttonPressed() { return !gd32f1_gpio_read(EXAMPLE_BUTTON_PORT, EXAMPLE_BUTTON_PIN); }

}  // namespace

int main() {
    // 2. Clock, time, log; persistence before the stack.
    gd32f1_system_init();
    gd32f1_gpio_mode(EXAMPLE_BUTTON_PORT, EXAMPLE_BUTTON_PIN, GD32F1_GPIO_INPUT_PULL_UP);
    gd32f1_gpio_write(EXAMPLE_LED_PORT, EXAMPLE_LED_PIN, EXAMPLE_LED_ACTIVE_LOW != 0);  // off, then output
    gd32f1_gpio_mode(EXAMPLE_LED_PORT, EXAMPLE_LED_PIN, GD32F1_GPIO_OUTPUT_2MHZ);
    rtt_init();
    rttLog("KNstaX GD32F1 example");
    rttLog(gd32f1_reset_cause_text(gd32f1_reset_cause()));

    if (g_flashLog.mount().isError()) rttLog("flash log mount failed");
    for (int i = 0; i < 8 && g_flashLog.servicePending(); ++i) (void)g_flashLog.service();
    knx::objects::setPersistenceFactory(&makePersistence);

    // 3. The platform.
    knx::platform::CooperativeHooks hooks{};
    hooks.millis = &hookMillis;
    hooks.micros = &hookMicros;
    hooks.idle = &hookIdle;
    hooks.serialNumber = &hookSerial;
    hooks.restart = &hookRestart;
    hooks.fatalError = &hookFatal;
    hooks.logLine = &hookLogLine;
    static knx::platform::CooperativePlatform platform(hooks);

    // 4. The TP-UART backend. With SAVE wired, the stack debounces it and
    // reports the link state; the early warning would let a product save state.
    knx::physical::TpUartConfig tpuartConfig{};
    tpuartConfig.saveSignal.notifyPowerFailFromIsr = false;
    auto* backend = new knx::physical::TpUartMediumBackend(gd32f1_tpuart_hal(), platform, tpuartConfig);

    // 5. The product.
    auto bindings = makeCommissionedBindings(kProduct);
    bindings.onCommand<Port::Switch>([](bool on) { setOutput(on); });
    bindings.provideState<Port::Status>([]() -> bool { return g_output; });
    bindings.onLifecycleChanged([](DeviceLifecycleState state) {
        gd32f1_gpio_write(EXAMPLE_LED_PORT, EXAMPLE_LED_PIN,
                          (state == DeviceLifecycleState::Commissioning) != (EXAMPLE_LED_ACTIVE_LOW != 0));
        rttLog(state == DeviceLifecycleState::Operational     ? "operational"
            : state == DeviceLifecycleState::Commissioning ? "programming mode"
                                                           : "uncommissioned");
    });
    auto started = startCommissionedProduct(platform, kProduct, std::move(bindings),
                                            std::unique_ptr<knx::physical::Tp1MediumBackend>(backend));
    if (started.isError()) {
        rttLog("start failed; restarting in 10 s");
        const uint32_t t0 = gd32f1_millis();
        while (gd32f1_millis() - t0 < 10000u) asm volatile("wfi");
        gd32f1_restart();
    }
    auto app = std::move(started.value());
    // The stack's clock: without it a coalesced configuration save never falls due.
    app->setTimeSource([]() -> uint32_t { return gd32f1_millis(); });
    g_flashLog.setEraseGuard(backend);
    knx::heap::lock();
    logNumber("started, heap ", gd32f1_heap_high_water());
    gd32f1_watchdog_start();

    // 6. The loop.
    uint32_t lastTick = gd32f1_millis();
    bool buttonStable = false;
    uint32_t buttonSince = 0;
    for (;;) {
        app->loop();
        (void)g_flashLog.service();

        const uint32_t now = gd32f1_millis();
        if (now != lastTick) {
            lastTick = now;
            const bool pressed = buttonPressed();
            if (pressed == buttonStable) {
                buttonSince = now;
            } else if (now - buttonSince >= 50u) {  // 50 ms debounce
                buttonStable = pressed;
                if (pressed) app->toggleProgrammingMode();
            }
        }

        if (g_statusDirty && app->lifecycleState() == DeviceLifecycleState::Operational) {
            g_statusDirty = !app->publish<Port::Status>(g_output).isOk();  // retried while the pipeline is full
        }

        if (const int c = rtt_read_char(); c == 'p') {
            app->toggleProgrammingMode();
        } else if (c == 's') {
            const auto s = backend->statistics();
            logNumber("frames received ", s.framesReceived);
            logNumber("acknowledges sent ", s.acksSent);
            logNumber("confirmations negative ", s.confirmsNegative);
            logNumber("erases guarded ", s.erasesGuarded);
            logNumber("heap high water ", gd32f1_heap_high_water());
        }

        gd32f1_watchdog_feed();
        asm volatile("wfi");
    }
}
