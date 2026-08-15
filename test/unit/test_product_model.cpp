// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/product/endpoint.hpp"

#include <cmath>
#include <type_traits>

using namespace knx;
using namespace knx::application;
using namespace knx::product;

enum class SwitchPort : uint16_t {
    RelayCommand = 0,
    RelayState = 1,
};

using RelayCommandPort = CommandPort<SwitchPort::RelayCommand,
                                     bool,
                                     "relay_command",
                                     "Relay command",
                                     dptids::Switch>;
using RelayStatePort = StatePort<SwitchPort::RelayState,
                                 bool,
                                 "relay_state",
                                 "Relay state",
                                 dptids::Switch,
                                 true>;

constexpr auto kSwitchDefinition =
    makeEndpointDefinition<SwitchPort, RelayCommandPort, RelayStatePort>(
        ProductIdentity{
            .productKey = "tp1_switch",
            .productDisplayName = "TP1 Switch",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = Medium::TP1,
            .applicationNumber = 1,
            .applicationVersion = 1,
            .firmwareRevision = 1,
            .maxApduLength = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "tp1_switch",
            .schemaVersion = 1,
            .persistKnxState = true,
        });

enum class ThermostatPort : uint16_t {
    Setpoint = 0,
    MeasuredTemperature = 1,
    HvacMode = 2,
    HeatingOutput = 3,
};

using SetpointPort = StateInOutPort<ThermostatPort::Setpoint,
                                    float,
                                    "setpoint_temperature",
                                    "Setpoint temperature",
                                    dptids::Temperature,
                                    true>;
using MeasuredTemperaturePort = StatePort<ThermostatPort::MeasuredTemperature,
                                          float,
                                          "measured_temperature",
                                          "Measured temperature",
                                          dptids::Temperature,
                                          false>;
using HvacModePort = StateInOutPort<ThermostatPort::HvacMode,
                                    Dpt20Mode,
                                    "hvac_mode",
                                    "HVAC mode",
                                    dptids::HvacMode,
                                    true>;
using HeatingOutputPort = StatePort<ThermostatPort::HeatingOutput,
                                    bool,
                                    "heating_output",
                                    "Heating output",
                                    dptids::Switch,
                                    false>;

constexpr auto kThermostatDefinition =
    makeEndpointDefinition<ThermostatPort,
                           SetpointPort,
                           MeasuredTemperaturePort,
                           HvacModePort,
                           HeatingOutputPort>(
        ProductIdentity{
            .productKey = "tp1_thermostat",
            .productDisplayName = "TP1 Thermostat",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = Medium::TP1,
            .applicationNumber = 2,
            .applicationVersion = 1,
            .firmwareRevision = 3,
            .maxApduLength = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "tp1_thermostat",
            .schemaVersion = 2,
            .persistKnxState = true,
        });

static_assert(std::same_as<port_value_t<decltype(kSwitchDefinition), SwitchPort::RelayCommand>, bool>);
static_assert(std::same_as<port_value_t<decltype(kSwitchDefinition), SwitchPort::RelayState>, bool>);
static_assert(std::same_as<port_value_t<decltype(kThermostatDefinition), ThermostatPort::Setpoint>, float>);
static_assert(std::same_as<port_value_t<decltype(kThermostatDefinition), ThermostatPort::HvacMode>, Dpt20Mode>);
static_assert(port_index_v<decltype(kThermostatDefinition), ThermostatPort::HeatingOutput> == 3u);

void setUp(void) {}
void tearDown(void) {}

void test_product_definition_exposes_stable_compile_time_metadata(void)
{
    using SwitchDefinition = std::remove_cvref_t<decltype(kSwitchDefinition)>;
    using RelayStateMetadata = port_spec_t<SwitchDefinition, SwitchPort::RelayState>;

    TEST_ASSERT_EQUAL_UINT(2u, SwitchDefinition::kPortCount);
    TEST_ASSERT_EQUAL_UINT16(0x00FAu, kSwitchDefinition.identity.manufacturerId.value());
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(Medium::TP1),
                           static_cast<unsigned int>(kSwitchDefinition.identity.medium));
    TEST_ASSERT_TRUE(std::string_view(RelayCommandPort::key) == "relay_command");
    TEST_ASSERT_TRUE(std::string_view(RelayCommandPort::displayName) == "Relay command");
    TEST_ASSERT_TRUE(std::string_view(RelayStateMetadata::displayName) == "Relay state");
    TEST_ASSERT_TRUE(RelayStateMetadata::persisted);
    TEST_ASSERT_TRUE(RelayStateMetadata::readable);
    TEST_ASSERT_FALSE(RelayStateMetadata::writable);
}

void test_product_bindings_dispatch_typed_command_and_state_provider(void)
{
    EndpointBindings<decltype(kSwitchDefinition)> bindings;

    bool relayState = false;
    bool programmingLed = false;
    bool faultSeen = false;
    const char* faultDetail = nullptr;

    bindings.onCommand<SwitchPort::RelayCommand>([&](bool on) {
                relayState = on;
            })
        .provideState<SwitchPort::RelayState>([&]() {
            return relayState;
        })
        .onProgrammingModeChanged([&](bool active) {
            programmingLed = active;
        })
        .onFault([&](knx::product::FaultInfo info) {
            faultSeen = true;
            faultDetail = info.detail;
        });

    TEST_ASSERT_TRUE(bindings.hasCommandBinding<SwitchPort::RelayCommand>());
    TEST_ASSERT_TRUE(bindings.hasStateProvider<SwitchPort::RelayState>());
    TEST_ASSERT_FALSE(bindings.hasStateWriteBinding<SwitchPort::RelayState>());

    TEST_ASSERT_TRUE(bindings.dispatchCommand<SwitchPort::RelayCommand>(true));
    TEST_ASSERT_TRUE(relayState);

    const auto state = bindings.readState<SwitchPort::RelayState>();
    TEST_ASSERT_TRUE(state.has_value());
    TEST_ASSERT_TRUE(state.value());

    bindings.notifyProgrammingModeChanged(true);
    TEST_ASSERT_TRUE(programmingLed);

    bindings.notifyFault({knx::product::FaultCode::PersistenceError, "persist"});
    TEST_ASSERT_TRUE(faultSeen);
    TEST_ASSERT_EQUAL_STRING("persist", faultDetail);
}

void test_product_bindings_accept_thermostat_value_types_without_object_indices(void)
{
    EndpointBindings<decltype(kThermostatDefinition)> bindings;

    float setpoint = 0.0f;
    float measuredTemperature = 21.5f;
    Dpt20Mode mode = Dpt20Mode::Auto;

    bindings.onStateWrite<ThermostatPort::Setpoint>([&](float value) {
                setpoint = value;
            })
        .provideState<ThermostatPort::MeasuredTemperature>([&]() {
            return measuredTemperature;
        })
        .onCommand<ThermostatPort::HvacMode>([&](Dpt20Mode newMode) {
            mode = newMode;
        });

    TEST_ASSERT_TRUE(bindings.hasStateWriteBinding<ThermostatPort::Setpoint>());
    TEST_ASSERT_TRUE(bindings.hasStateProvider<ThermostatPort::MeasuredTemperature>());
    TEST_ASSERT_TRUE(bindings.hasCommandBinding<ThermostatPort::HvacMode>());

    TEST_ASSERT_TRUE(bindings.dispatchStateWrite<ThermostatPort::Setpoint>(23.5f));
    TEST_ASSERT_TRUE(std::fabs(setpoint - 23.5f) < 0.001f);

    TEST_ASSERT_TRUE(bindings.dispatchCommand<ThermostatPort::HvacMode>(Dpt20Mode::Comfort));
    TEST_ASSERT_EQUAL(static_cast<int>(Dpt20Mode::Comfort), static_cast<int>(mode));

    const auto measured = bindings.readState<ThermostatPort::MeasuredTemperature>();
    TEST_ASSERT_TRUE(measured.has_value());
    TEST_ASSERT_TRUE(std::fabs(measured.value() - measuredTemperature) < 0.001f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_product_definition_exposes_stable_compile_time_metadata);
    RUN_TEST(test_product_bindings_dispatch_typed_command_and_state_provider);
    RUN_TEST(test_product_bindings_accept_thermostat_value_types_without_object_indices);
    return UNITY_END();
}