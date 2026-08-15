// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/product/exporter.hpp"
#include "knx/product/endpoint.hpp"

#include <string>
#include <string_view>

using namespace knx;
using namespace knx::application;
using namespace knx::product;

enum class CompilerSwitchPort : uint16_t {
    RelayCommand = 5,
    RelayState = 33,
};

using CompilerRelayCommandPort = CommandPort<CompilerSwitchPort::RelayCommand,
                                             bool,
                                             "relay_command",
                                             "Relay command",
                                             dptids::Switch>;
using CompilerRelayStatePort = StatePort<CompilerSwitchPort::RelayState,
                                         bool,
                                         "relay_state",
                                         "Relay state",
                                         dptids::Switch,
                                         true>;

constexpr auto kCompilerSwitchDefinition =
    makeEndpointDefinition<CompilerSwitchPort, CompilerRelayCommandPort, CompilerRelayStatePort>(
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

enum class CompilerThermostatPort : uint16_t {
    Setpoint = 10,
    MeasuredTemperature = 40,
    HvacMode = 90,
    HeatingOutput = 160,
};

using CompilerSetpointPort = StateInOutPort<CompilerThermostatPort::Setpoint,
                                            float,
                                            "setpoint_temperature",
                                            "Setpoint temperature",
                                            dptids::Temperature,
                                            true>;
using CompilerMeasuredTemperaturePort = StatePort<CompilerThermostatPort::MeasuredTemperature,
                                                  float,
                                                  "measured_temperature",
                                                  "Measured temperature",
                                                  dptids::Temperature,
                                                  false>;
using CompilerHvacModePort = StateInOutPort<CompilerThermostatPort::HvacMode,
                                            Dpt20Mode,
                                            "hvac_mode",
                                            "HVAC mode",
                                            dptids::HvacMode,
                                            true>;
using CompilerHeatingOutputPort = StatePort<CompilerThermostatPort::HeatingOutput,
                                            bool,
                                            "heating_output",
                                            "Heating output",
                                            dptids::Switch,
                                            false>;

constexpr auto kCompilerThermostatDefinition =
    makeEndpointDefinition<CompilerThermostatPort,
                           CompilerSetpointPort,
                           CompilerMeasuredTemperaturePort,
                           CompilerHvacModePort,
                           CompilerHeatingOutputPort>(
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

constexpr auto kCompiledSwitch = compileEndpointDefinition(kCompilerSwitchDefinition);
constexpr auto kCompiledThermostat = compileEndpointDefinition(kCompilerThermostatDefinition);

static_assert(kCompiledSwitch.runtime.communicationObjects[0].slot == 0u);
static_assert(kCompiledSwitch.runtime.communicationObjects[1].slot == 1u);
static_assert(kCompiledSwitch.runtime.communicationObjects[1].logicalId == CompilerSwitchPort::RelayState);
static_assert(kCompiledThermostat.runtime.communicationObjects[2].slot == 2u);
static_assert(kCompiledThermostat.exportDescriptor.communicationObjects[2].logicalId
              == static_cast<uint16_t>(CompilerThermostatPort::HvacMode));

void setUp(void) {}
void tearDown(void) {}

void test_product_compiler_derives_runtime_descriptor_set_from_definition(void)
{
    TEST_ASSERT_EQUAL_UINT(2u, kCompiledSwitch.runtime.kCommunicationObjectCount);
    TEST_ASSERT_EQUAL_UINT(4u, kCompiledThermostat.runtime.kCommunicationObjectCount);

    const auto relayStateSlot = kCompiledSwitch.slotForLogicalId(CompilerSwitchPort::RelayState);
    TEST_ASSERT_TRUE(relayStateSlot.has_value());
    TEST_ASSERT_EQUAL_UINT(1u, relayStateSlot.value());

    const auto hvacModeSlot = kCompiledThermostat.slotForLogicalId(CompilerThermostatPort::HvacMode);
    TEST_ASSERT_TRUE(hvacModeSlot.has_value());
    TEST_ASSERT_EQUAL_UINT(2u, hvacModeSlot.value());

    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(CompilerThermostatPort::HvacMode),
                             static_cast<uint16_t>(kCompiledThermostat.runtime.communicationObjects[2].logicalId));
    TEST_ASSERT_EQUAL_UINT16(2u, kCompiledThermostat.runtime.communicationObjects[2].exportNumber);
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(PortDirection::StateInOut),
                           static_cast<unsigned int>(kCompiledThermostat.runtime.communicationObjects[2].direction));
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(DptValue::Type::HvacMode),
                           static_cast<unsigned int>(kCompiledThermostat.runtime.communicationObjects[2].valueType));
}

void test_product_compiler_uses_one_compiled_source_for_runtime_and_export_metadata(void)
{
    const auto& runtimeObject = kCompiledSwitch.runtime.communicationObjects[0];
    const auto& exportObject = kCompiledSwitch.exportDescriptor.communicationObjects[0];

    TEST_ASSERT_EQUAL_UINT16(runtimeObject.exportNumber, exportObject.exportNumber);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(runtimeObject.logicalId), exportObject.logicalId);
    TEST_ASSERT_TRUE(runtimeObject.dpt == exportObject.dpt);
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(runtimeObject.valueType),
                           static_cast<unsigned int>(exportObject.valueType));
    TEST_ASSERT_EQUAL(runtimeObject.readable, exportObject.readable);
    TEST_ASSERT_EQUAL(runtimeObject.writable, exportObject.writable);
    TEST_ASSERT_EQUAL(runtimeObject.transmit, exportObject.transmit);
    TEST_ASSERT_EQUAL(runtimeObject.receivable, exportObject.receivable);
    TEST_ASSERT_EQUAL(runtimeObject.persisted, exportObject.persisted);

    TEST_ASSERT_TRUE(kCompiledSwitch.exportDescriptor.features.persistenceEnabled);
    TEST_ASSERT_TRUE(kCompiledSwitch.exportDescriptor.features.readResponsesEnabled);
    TEST_ASSERT_EQUAL_UINT(2u, kCompiledSwitch.exportDescriptor.capacities.datapointCount);
    TEST_ASSERT_EQUAL_UINT(1u, kCompiledSwitch.exportDescriptor.capacities.autoResponseQueueCapacity);
    TEST_ASSERT_EQUAL_UINT(1u, kCompiledSwitch.exportDescriptor.capacities.transmissionOutcomeQueueCapacity);
    TEST_ASSERT_TRUE(exportObject.key == "relay_command");
    TEST_ASSERT_TRUE(exportObject.displayName == "Relay command");
}

void test_product_compiler_export_descriptor_roundtrips_through_existing_exporter_pipeline(void)
{
    const auto exportDescriptor = makeExportDescriptor(kCompilerThermostatDefinition);
    const std::string json = knx::product::exportDescriptorToJson(exportDescriptor);

    TEST_ASSERT_TRUE(json.find("\"tp1_thermostat\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"logicalId\":90") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"setpoint_temperature\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"hvac_mode\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"groupObjectCount\":4") != std::string::npos);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_product_compiler_derives_runtime_descriptor_set_from_definition);
    RUN_TEST(test_product_compiler_uses_one_compiled_source_for_runtime_and_export_metadata);
    RUN_TEST(test_product_compiler_export_descriptor_roundtrips_through_existing_exporter_pipeline);
    return UNITY_END();
}