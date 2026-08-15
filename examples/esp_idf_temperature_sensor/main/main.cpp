// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/platform/esp32_platform.hpp"
#include "knx/physical/bitbang_driver_timer_isr_espidf.hpp"
#include "knx/physical/physical_factory.hpp"
#include "knx/product/commissioned_product.hpp"
#include "knx/util/log.hpp"

#include "esp_err.h"
#include "nvs_flash.h"

#include <cstdint>
#include <memory>

using namespace knx;
using namespace knx::application;
using namespace knx::product;

namespace {

enum class SensorPort : uint16_t {
    Temperature = 0,
};

constexpr auto kTemperatureSensorProduct =
    makeCommissionedProduct(
        makeEndpointDefinition<
            SensorPort,
            semantics::TemperatureState<SensorPort::Temperature, "temperature", "Temperature", false>>(
            ProductIdentity{
                .productKey = "esp_idf_temperature_sensor",
                .productDisplayName = "ESP-IDF Temperature Sensor",
                .manufacturerId = ManufacturerId(0x00FA),
                .medium = endpoint::Medium::TP1,
                .applicationNumber = 10,
                .applicationVersion = 1,
                .firmwareRevision = 1,
                .maxApduLength = 254,
            },
            PersistencePolicy{
                .namespacePrefix = "esp_temp_sensor",
                .schemaVersion = 1,
                .persistKnxState = true,
            }));

float readBoardTemperatureC()
{
    // Replace with a real sensor read (I2C/SPI/ADC). This value is only a stub.
    return 22.5f;
}

util::Result<std::unique_ptr<physical::Tp1MacPhysical>> createTp1Physical(platform::Esp32Platform& platform)
{
    physical::Tp1BackendSelection selection{};

#if defined(CONFIG_KNX_TP1_TPUART)
    selection.family = physical::Tp1BackendFamily::Tpuart;
#elif defined(CONFIG_KNX_TP1_BITBANG)
    selection.family = physical::Tp1BackendFamily::Bitbang;
#else
    return util::ErrorCode::OperationNotSupported;
#endif

    physical::Tp1PlatformDependencies dependencies{};
    dependencies.platform = &platform;
    dependencies.uart = platform.uart();

#if defined(CONFIG_KNX_TP1_BITBANG)
    static physical::BitBangDriverTimerIsrEspIdf bitbangDriver;
    dependencies.bitbangDriver = &bitbangDriver;
    dependencies.bitbangTp1Driver = &bitbangDriver;
#endif

    return physical::createTp1PhysicalForPlatform(selection, dependencies);
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

} // namespace

extern "C" void app_main(void)
{
    initNvs();

    platform::Esp32Platform platform;

    auto physicalResult = createTp1Physical(platform);
    if (physicalResult.isError()) {
        KNX_LOGE("TempSensor", "TP1 physical init failed: %d", static_cast<int>(physicalResult.error()));
        return;
    }

    auto appResult = startCommissionedProduct(
        platform,
        kTemperatureSensorProduct,
        makeCommissionedBindings(kTemperatureSensorProduct)
            .provideState<SensorPort::Temperature>([]() {
                return readBoardTemperatureC();
            })
            .onProgrammingModeChanged([](bool enabled) {
                KNX_LOGI("TempSensor", "Programming mode: %s", enabled ? "ON" : "OFF");
            })
            .onLifecycleChanged([](DeviceLifecycleState state) {
                KNX_LOGI("TempSensor", "Lifecycle: %s",
                         state == DeviceLifecycleState::Operational   ? "Operational" :
                         state == DeviceLifecycleState::Commissioning ? "Commissioning" :
                                                                       "Uncommissioned");
            })
            .onFault([](FaultInfo info) {
                KNX_LOGE("TempSensor", "Fault: code=%d detail=%s",
                         static_cast<int>(info.code),
                         info.detail != nullptr ? info.detail : "");
            }),
        std::move(physicalResult.value()));

    if (appResult.isError()) {
        KNX_LOGE("TempSensor", "startCommissionedProduct failed: %d", static_cast<int>(appResult.error()));
        return;
    }

    auto app = std::move(appResult.value());

    KNX_LOGI("TempSensor", "ETS-commissionable TP1 temperature sensor started");

    uint32_t lastPublishMs = 0;
    for (;;) {
        app->loop();

        const uint32_t now = platform.millis();
        if (app->lifecycleState() == DeviceLifecycleState::Operational && (now - lastPublishMs) >= 10'000u) {
            lastPublishMs = now;
            const float temperature = readBoardTemperatureC();
            const auto publish = app->publish<SensorPort::Temperature>(temperature);
            if (publish.isError()) {
                KNX_LOGW("TempSensor", "Temperature publish failed: %d", static_cast<int>(publish.error()));
            } else {
                KNX_LOGI("TempSensor", "Published temperature %.2f C", temperature);
            }
        }

        platform.delay(5);
    }
}
