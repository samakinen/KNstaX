// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * TP1 Switch Actuator — ESP-IDF firmware, real TP1 transceiver.
 *
 * This is the canonical onboarding example: the smallest complete
 * ETS-commissionable KNX device that drives real hardware. Everything a
 * firmware developer needs to answer "how do I wire this to my transceiver"
 * is in this file:
 *
 *   - createTp1Physical()      picks the bitbang or TPUART backend from Kconfig
 *   - onCommand<>()            KNX write  -> GPIO
 *   - provideState<>()         KNX read   <- GPIO
 *   - onLifecycleChanged()     commissioning state -> indicator LED
 *   - programming button       -> app->toggleProgrammingMode()
 *
 * The product definition lives in product.hpp, which stays free of hardware and
 * OS dependencies so the same declaration also feeds the .knxprod exporter.
 *
 * Wiring (defaults; override in `idf.py menuconfig` -> KNX Stack Configuration):
 *   CONFIG_KNX_TP1_BITBANG_TX_PIN   transceiver TX
 *   CONFIG_KNX_TP1_BITBANG_RX_PIN   transceiver RX
 *   CONFIG_KNX_TP1_BITBANG_LINK_PIN transceiver bus-health output (optional)
 *
 * See docs/reference/board_bringup_guide.md before connecting to a live bus:
 * an inverted TX polarity holds the line dominant and takes down the whole
 * installation, not just this device.
 */

#include "product.hpp"

#include "knx/physical/physical_factory.hpp"
#include "knx/platform/esp32_platform.hpp"
#include "knx/util/log.hpp"

#include "driver/gpio.h"
#include "esp_err.h"
#include "nvs_flash.h"

#include <memory>

#if defined(CONFIG_KNX_TP1_BITBANG)
#include "knx/physical/bitbang_driver_timer_isr_espidf.hpp"
#endif

using namespace knx;
using namespace tp1_switch;

namespace {

constexpr const char* TAG = "Switch";

// Board wiring. Replace with your own board's pin map.
constexpr gpio_num_t kRelayGpio = GPIO_NUM_10;
constexpr gpio_num_t kStatusLedGpio = GPIO_NUM_8;
constexpr gpio_num_t kProgButtonGpio = GPIO_NUM_9;

bool g_relayOn = false;

void initNvs()
{
    // KNX commissioned state (individual address, group addresses, parameters)
    // is persisted in NVS, so this must succeed before the stack starts or the
    // device forgets its commissioning on every power cycle.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void initGpio()
{
    gpio_config_t out{};
    out.pin_bit_mask = (1ULL << kRelayGpio) | (1ULL << kStatusLedGpio);
    out.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&out));

    gpio_config_t in{};
    in.pin_bit_mask = (1ULL << kProgButtonGpio);
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&in));
}

void setRelay(bool on)
{
    g_relayOn = on;
    gpio_set_level(kRelayGpio, on ? 1 : 0);
}

/// Build the TP1 physical layer for whichever backend Kconfig selected.
/// Both backends land on the same Tp1MacPhysical, so nothing above this
/// function knows or cares which transceiver is fitted.
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
    // Static: the timer ISR keeps a pointer to it for the life of the program.
    static physical::BitBangDriverTimerIsrEspIdf bitbangDriver;
    dependencies.bitbangDriver = &bitbangDriver;
    dependencies.bitbangTp1Driver = &bitbangDriver;
#else
#error "Select a TP1 backend: CONFIG_KNX_TP1_BITBANG or CONFIG_KNX_TP1_TPUART"
#endif

    return physical::createTp1PhysicalForPlatform(selection, dependencies);
}

/// Debounced programming-button edge detector.
bool progButtonPressed()
{
    static bool wasDown = false;
    const bool isDown = gpio_get_level(kProgButtonGpio) == 0;  // active low
    const bool edge = isDown && !wasDown;
    wasDown = isDown;
    return edge;
}

} // namespace

extern "C" void app_main(void)
{
    initNvs();
    initGpio();

    static platform::Esp32Platform platform;

    auto physicalResult = createTp1Physical(platform);
    if (physicalResult.isError()) {
        KNX_LOGE(TAG, "TP1 physical init failed: %d", static_cast<int>(physicalResult.error()));
        return;
    }

    bool relayStateDirty = false;

    auto appResult = startCommissionedProduct(
        platform,
        kProduct,
        makeCommissionedBindings(kProduct)
            .onCommand<Port::RelayCommand>([&relayStateDirty](bool on) {
                setRelay(on);
                relayStateDirty = true;
                KNX_LOGI(TAG, "Relay: %s", on ? "ON" : "OFF");
            })
            .provideState<Port::RelayState>([]() { return g_relayOn; })
            .onLifecycleChanged([](product::DeviceLifecycleState state) {
                switch (state) {
                    case product::DeviceLifecycleState::Uncommissioned:
                        KNX_LOGI(TAG, "Lifecycle: Uncommissioned — waiting for ETS");
                        gpio_set_level(kStatusLedGpio, 0);
                        break;
                    case product::DeviceLifecycleState::Commissioning:
                        KNX_LOGI(TAG, "Lifecycle: Programming mode active");
                        gpio_set_level(kStatusLedGpio, 1);
                        break;
                    case product::DeviceLifecycleState::Operational:
                        KNX_LOGI(TAG, "Lifecycle: Operational — bus ready");
                        gpio_set_level(kStatusLedGpio, 1);
                        break;
                }
            })
            .onFault([](product::FaultInfo info) {
                KNX_LOGE(TAG, "Fault: code=%d detail=%s", static_cast<int>(info.code),
                         info.detail != nullptr ? info.detail : "");
            }),
        std::move(physicalResult.value()));

    if (appResult.isError()) {
        KNX_LOGE(TAG, "Start failed: %d", static_cast<int>(appResult.error()));
        return;
    }

    auto app = std::move(appResult.value());
    KNX_LOGI(TAG, "TP1 switch actuator started");

    for (;;) {
        app->loop();  // drives the protocol stack and fires firmware callbacks

        if (progButtonPressed()) {
            app->toggleProgrammingMode();
        }

        // Publish state feedback only once the bus is usable. Publishing while
        // uncommissioned would send from the unprogrammed address 15.15.255.
        if (relayStateDirty
            && app->lifecycleState() == product::DeviceLifecycleState::Operational) {
            relayStateDirty = false;
            if (const auto res = app->publish<Port::RelayState>(g_relayOn); res.isError()) {
                KNX_LOGW(TAG, "State publish failed: %d", static_cast<int>(res.error()));
            }
        }

        platform.delay(5);
    }
}
