// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * TP1 Thermostat example.
 *
 * Demonstrates the commissioned-product guide lane:
 *   - business-port endpoint definition instead of manual communication-object registration
 *   - typed parameter access lives next to typed command/state bindings
 *   - commissioned product startup owns runtime assembly and KNX-state persistence wiring
 *   - outgoing process traffic stays suppressed until commissioning assigns an operational IA
 *   - programming-mode callback still maps naturally to a status LED
 */

#include "knx/product/commissioned_product.hpp"
#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/util/log.hpp"

#include <cmath>
#include <memory>

using namespace knx::application;
using namespace knx;
using namespace knx::product;

namespace {

enum class ThermostatPort : uint16_t {
    Setpoint = 0,
    MeasuredTemperature = 1,
    HvacMode = 2,
};

enum class ThermostatParameter : uint16_t {
    DefaultSetpoint = 0,
};

constexpr auto kThermostatProduct =
    makeCommissionedProduct(
        makeEndpointDefinition<ThermostatPort,
                               semantics::TemperatureStateInOut<ThermostatPort::Setpoint,
                                                                "setpoint_temperature",
                                                                "Setpoint Temperature">,
                               semantics::TemperatureState<ThermostatPort::MeasuredTemperature,
                                                           "measured_temperature",
                                                           "Measured Temperature",
                                                           false>,
                               semantics::HvacModeStateInOut<ThermostatPort::HvacMode,
                                                             "hvac_mode",
                                                             "HVAC Mode">>(
            ProductIdentity{
                .productKey = "tp1_thermostat",
                .productDisplayName = "TP1 Thermostat",
                .manufacturerId = ManufacturerId(0x00FA),
                .medium = endpoint::Medium::TP1,
                .applicationNumber = 2,
                .applicationVersion = 1,
                .firmwareRevision = 1,
                .maxApduLength = 254,
            },
            PersistencePolicy{
                .namespacePrefix = "tp1_thermostat",
                .schemaVersion = 1,
                .persistKnxState = true,
            }),
        makeParameterSchema(
            parameter<ThermostatParameter::DefaultSetpoint>("default_setpoint", 22.0f)));

} // namespace

int main() {
    platform::LinuxPlatform platform;

    // Application state — in real firmware these come from sensors/actuators.
    float     roomTemp  = 21.0f;
    float     setpoint  = 0.0f;
    Dpt20Mode hvacMode  = Dpt20Mode::Comfort;

    auto appResult = startCommissionedProduct(
        platform,
        kThermostatProduct,
        makeCommissionedBindings(kThermostatProduct)
            .onStateWrite<ThermostatPort::Setpoint>([&](float value) {
                setpoint = value;
                KNX_LOGI("Example.Thermostat", "Setpoint: %.1f C", setpoint);
            })
            .provideState<ThermostatPort::Setpoint>([&]() {
                return setpoint;
            })
            .provideState<ThermostatPort::MeasuredTemperature>([&]() {
                return roomTemp;
            })
            .onStateWrite<ThermostatPort::HvacMode>([&](Dpt20Mode mode) {
                hvacMode = mode;
                KNX_LOGI("Example.Thermostat", "HVAC mode: %d", static_cast<int>(hvacMode));
            })
            .provideState<ThermostatPort::HvacMode>([&]() {
                return hvacMode;
            })
            .onParameterChanged<ThermostatParameter::DefaultSetpoint>([&](float value) {
                setpoint = value;
                KNX_LOGI("Example.Thermostat", "Default setpoint parameter: %.1f C", setpoint);
            })
            .onProgrammingModeChanged([](bool enabled) {
                KNX_LOGI("Example.Thermostat", "Programming mode: %s", enabled ? "ON" : "OFF");
                // gpio_set_level(PROG_LED_GPIO, enabled);
            })
            .onLifecycleChanged([](product::DeviceLifecycleState state) {
                KNX_LOGI("Example.Thermostat", "Lifecycle: %s",
                         state == product::DeviceLifecycleState::Operational   ? "Operational" :
                         state == product::DeviceLifecycleState::Commissioning ? "Commissioning" :
                                                                                  "Uncommissioned");
            }),
        std::unique_ptr<physical::Tp1MediumBackend>(new physical::NullTp1MediumBackend()));

    if (appResult.isError()) {
        KNX_LOGE("Example.Thermostat", "Start failed: %d", static_cast<int>(appResult.error()));
        return 1;
    }

    auto app = std::move(appResult.value());
    setpoint = app->parameters().get<ThermostatParameter::DefaultSetpoint>();

    float lastPublishedRoomTemp = roomTemp;
    uint32_t lastPublishMs = 0;

    for (;;) {
        app->loop();
        const uint32_t now = platform.millis();

        // Simulate sensor read (replace with real hardware read).
        roomTemp += 0.01f;

        if (app->lifecycleState() == product::DeviceLifecycleState::Operational
            && ((now - lastPublishMs) >= 300'000u
                || std::fabs(roomTemp - lastPublishedRoomTemp) >= 0.5f)) {
            const auto publish = app->publish<ThermostatPort::MeasuredTemperature>(roomTemp);
            if (publish.isOk()) {
                lastPublishMs = now;
                lastPublishedRoomTemp = roomTemp;
            } else {
                KNX_LOGW("Example.Thermostat", "Temperature publish failed: %d", static_cast<int>(publish.error()));
            }
        }

        platform.delay(5);
        // In real firmware: if (button_pressed()) app->toggleProgrammingMode();
    }
}
