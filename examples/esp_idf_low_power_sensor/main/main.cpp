// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * Low-power TP1 contact sensor — ESP-IDF firmware, real TP1 transceiver.
 *
 * Demonstrates the owner-context low-power API, which only means anything on
 * target hardware:
 *
 *   setWorkAvailableCallback()  the stack wakes the firmware when KNX work
 *                               appears, instead of the firmware polling
 *   ownerWorkHint()             tells the firmware whether loop() must run now
 *                               and how long it may sleep
 *
 * Together these let the CPU sit in light sleep between telegrams rather than
 * spinning at 200 Hz. On a bus-powered device that difference is most of the
 * current budget.
 *
 * IMPORTANT — why light sleep and not deep sleep:
 * this device receives as well as transmits, and the TP1 receive path is a GPIO
 * edge feeding a timer ISR. Deep sleep powers down the timer, so an incoming
 * telegram would be missed entirely rather than merely delayed. A deep-sleep
 * design needs either a transceiver that buffers frames (TPUART with its own
 * MCU) or a device that only ever transmits. Light sleep keeps the peripherals
 * clocked and still cuts the bulk of the idle draw.
 */

#include "product.hpp"

#include "knx/physical/physical_factory.hpp"
#include "knx/platform/esp32_platform.hpp"
#include "knx/util/log.hpp"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "nvs_flash.h"

#include <atomic>
#include <memory>

#if defined(CONFIG_KNX_TP1_BITBANG)
#include "knx/physical/bitbang_driver_timer_isr_espidf.hpp"
#endif

using namespace knx;
using namespace esp_idf_low_power_sensor;

namespace {

constexpr const char* TAG = "LpContact";

constexpr gpio_num_t kContactGpio = GPIO_NUM_6;

/// Set from the KNX stack's work-available callback, which may run from the
/// data-link RX context. Only ever set here and cleared in the main loop.
std::atomic<bool> g_knxWorkPending{false};

/// Set from the contact's GPIO ISR.
std::atomic<bool> g_contactChanged{false};

bool g_contactOpen = false;

void IRAM_ATTR contactIsr(void*)
{
    g_contactChanged.store(true, std::memory_order_relaxed);
}

void initNvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void initContactGpio()
{
    gpio_config_t in{};
    in.pin_bit_mask = (1ULL << kContactGpio);
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    in.intr_type = GPIO_INTR_ANYEDGE;
    ESP_ERROR_CHECK(gpio_config(&in));

    // The KNX bitbang backend may already own the ISR service; tolerate that.
    const esp_err_t installed = gpio_install_isr_service(0);
    if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(installed);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(kContactGpio, contactIsr, nullptr));

    // Keep the contact able to wake us out of light sleep.
    ESP_ERROR_CHECK(gpio_wakeup_enable(kContactGpio, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
}

bool readContactOpen()
{
    return gpio_get_level(kContactGpio) != 0;
}

util::Result<std::unique_ptr<physical::Tp1MacPhysical>> createTp1Physical(
    platform::Esp32Platform& platform)
{
    physical::Tp1BackendSelection selection{};
    physical::Tp1PlatformDependencies dependencies{};
    dependencies.platform = &platform;
    dependencies.uart = platform.uart();

#if defined(CONFIG_KNX_TP1_TPUART)
    selection.family = physical::Tp1BackendFamily::Tpuart;
#elif defined(CONFIG_KNX_TP1_BITBANG)
    selection.family = physical::Tp1BackendFamily::Bitbang;
    static physical::BitBangDriverTimerIsrEspIdf bitbangDriver;
    dependencies.bitbangDriver = &bitbangDriver;
    dependencies.bitbangTp1Driver = &bitbangDriver;
#else
#error "Select a TP1 backend: CONFIG_KNX_TP1_BITBANG or CONFIG_KNX_TP1_TPUART"
#endif

    return physical::createTp1PhysicalForPlatform(selection, dependencies);
}

/// Enable automatic light sleep. The KNX RX path stays clocked, so the device
/// still receives; it simply stops burning cycles when there is nothing to do.
void enableAutomaticLightSleep()
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm{};
    pm.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    pm.min_freq_mhz = 10;
    pm.light_sleep_enable = true;
    const esp_err_t err = esp_pm_configure(&pm);
    if (err != ESP_OK) {
        KNX_LOGW(TAG, "Power management unavailable (%d); running at full speed", err);
    }
#else
    KNX_LOGW(TAG, "CONFIG_PM_ENABLE is off; enable it for the low-power path");
#endif
}

} // namespace

extern "C" void app_main(void)
{
    initNvs();
    initContactGpio();
    enableAutomaticLightSleep();

    static platform::Esp32Platform platform;

    auto physicalResult = createTp1Physical(platform);
    if (physicalResult.isError()) {
        KNX_LOGE(TAG, "TP1 physical init failed: %d", static_cast<int>(physicalResult.error()));
        return;
    }

    g_contactOpen = readContactOpen();

    auto appResult = startCommissionedProduct(
        platform,
        kProduct,
        makeCommissionedBindings(kProduct)
            .provideState<Port::ContactState>([]() { return g_contactOpen; })
            .onLifecycleChanged([](product::DeviceLifecycleState state) {
                KNX_LOGI(TAG, "Lifecycle: %s",
                         state == product::DeviceLifecycleState::Operational   ? "Operational"
                         : state == product::DeviceLifecycleState::Commissioning ? "Commissioning"
                                                                                 : "Uncommissioned");
            }),
        std::move(physicalResult.value()));

    if (appResult.isError()) {
        KNX_LOGE(TAG, "Start failed: %d", static_cast<int>(appResult.error()));
        return;
    }

    auto app = std::move(appResult.value());

    // The stack calls this when owner-context work appears. It can run from the
    // data-link RX context, so it must stay non-blocking — set a flag, nothing
    // more.
    app->setWorkAvailableCallback([]() { g_knxWorkPending.store(true, std::memory_order_relaxed); });

    // Send-on-change plus a slow heartbeat: a contact that never moves still
    // proves it is alive once an hour, without a telegram every second.
    application::GroupObjectTransmitPolicy policy{};
    policy.onChangeEnabled = true;
    policy.cyclicIntervalMs = 3'600'000u;
    (void)app->setTransmitPolicy(Port::ContactState, policy);

    // Cyclic sends and the rate limiter are time-based and inert without a clock.
    app->setTimeSource([]() { return platform.millis(); });

    KNX_LOGI(TAG, "Low-power TP1 contact sensor started");

    for (;;) {
        const bool contactMoved = g_contactChanged.exchange(false, std::memory_order_relaxed);
        if (contactMoved) {
            const bool open = readContactOpen();
            if (open != g_contactOpen) {
                g_contactOpen = open;
                KNX_LOGI(TAG, "Contact: %s", open ? "OPEN" : "CLOSED");
                if (app->lifecycleState() == product::DeviceLifecycleState::Operational) {
                    (void)app->publish<Port::ContactState>(open);
                }
            }
        }

        g_knxWorkPending.store(false, std::memory_order_relaxed);
        app->loop();

        // Ask the stack what it still needs. `maxSleepMs` accounts for pending
        // cyclic sends and deferred transmissions, so honouring it is what keeps
        // the heartbeat on time while still allowing long idle periods.
        const auto hint = app->ownerWorkHint();
        if (hint.hasImmediateWork() || g_knxWorkPending.load(std::memory_order_relaxed)
            || g_contactChanged.load(std::memory_order_relaxed)) {
            continue;  // more to do; do not sleep yet
        }

        // With automatic light sleep configured, a plain delay is the sleep:
        // the idle task enters light sleep for its duration and any KNX edge or
        // contact edge wakes it early.
        const uint32_t sleepMs = hint.maxSleepMs.value_or(1000u);
        platform.delay(sleepMs == 0u ? 1u : sleepMs);
    }
}
