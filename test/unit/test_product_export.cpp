// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/product/endpoint.hpp"

#include <string>

using namespace knx;
using namespace knx::application;
using namespace knx::product::endpoint;

namespace {

enum class ExportSwitchPort : uint16_t {
    RelayCommand = 5,
    RelayState = 33,
};

using ExportRelayCommandPort = CommandPort<ExportSwitchPort::RelayCommand,
                                           bool,
                                           "relay_command",
                                           "Relay command",
                                           dptids::Switch>;
using ExportRelayStatePort = StatePort<ExportSwitchPort::RelayState,
                                       bool,
                                       "relay_state",
                                       "Relay state",
                                       dptids::Switch,
                                       true>;

constexpr auto kExportSwitchDefinition =
    makeEndpointDefinition<ExportSwitchPort, ExportRelayCommandPort, ExportRelayStatePort>(
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

enum class ExportThermostatPort : uint16_t {
    Setpoint = 10,
    MeasuredTemperature = 40,
    HvacMode = 90,
};

using ExportSetpointPort = StateInOutPort<ExportThermostatPort::Setpoint,
                                          float,
                                          "setpoint_temperature",
                                          "Setpoint temperature",
                                          dptids::Temperature,
                                          true>;
using ExportMeasuredTemperaturePort = StatePort<ExportThermostatPort::MeasuredTemperature,
                                                float,
                                                "measured_temperature",
                                                "Measured temperature",
                                                dptids::Temperature,
                                                false>;
using ExportHvacModePort = StateInOutPort<ExportThermostatPort::HvacMode,
                                          Dpt20Mode,
                                          "hvac_mode",
                                          "HVAC mode",
                                          dptids::HvacMode,
                                          true>;

constexpr auto kExportThermostatDefinition =
    makeEndpointDefinition<ExportThermostatPort,
                           ExportSetpointPort,
                           ExportMeasuredTemperaturePort,
                           ExportHvacModePort>(
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

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_product_export_bridge_serializes_proof_products_through_existing_exporter_pipeline(void)
{
    const std::string switchJson = exportEndpointToJson(kExportSwitchDefinition);
    const std::string switchXml = exportEndpointToKaenxXml(kExportSwitchDefinition);
    const std::string thermostatJson = exportEndpointToJson(kExportThermostatDefinition);
    const std::string thermostatXml = exportEndpointToKaenxXml(kExportThermostatDefinition);

    TEST_ASSERT_TRUE(switchJson.find("\"tp1_switch\"") != std::string::npos);
    TEST_ASSERT_TRUE(switchJson.find("\"logicalId\":33") != std::string::npos);
    TEST_ASSERT_TRUE(switchJson.find("\"relay_state\"") != std::string::npos);
    TEST_ASSERT_TRUE(switchXml.find("<ProfileKey>tp1_switch</ProfileKey>") != std::string::npos);
    TEST_ASSERT_TRUE(switchXml.find("<Key>relay_command</Key>") != std::string::npos);

    TEST_ASSERT_TRUE(thermostatJson.find("\"tp1_thermostat\"") != std::string::npos);
    TEST_ASSERT_TRUE(thermostatJson.find("\"logicalId\":90") != std::string::npos);
    TEST_ASSERT_TRUE(thermostatJson.find("\"hvac_mode\"") != std::string::npos);
    TEST_ASSERT_TRUE(thermostatXml.find("<ProfileKey>tp1_thermostat</ProfileKey>") != std::string::npos);
    TEST_ASSERT_TRUE(thermostatXml.find("<Key>setpoint_temperature</Key>") != std::string::npos);
}

void test_product_export_bridge_uses_the_same_compiled_source_as_runtime(void)
{
    EndpointRuntime<decltype(kExportThermostatDefinition)> runtime(
        kExportThermostatDefinition,
        EndpointBindings<decltype(kExportThermostatDefinition)>{},
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 60),
            .persistenceNamespace = "export_runtime_0",
            .restoreKnxStateOnBoot = true,
        });

    const auto descriptorFromDefinition = makeEndpointExportDescriptor(kExportThermostatDefinition);
    const auto descriptorFromRuntimeCompiled = makeEndpointExportDescriptor(runtime.compiledDefinition());
    const std::string jsonFromDefinition = exportEndpointToJson(kExportThermostatDefinition);
    const std::string jsonFromRuntimeCompiled = exportEndpointToJson(runtime.compiledDefinition());
    const std::string xmlFromDefinition = exportEndpointToKaenxXml(kExportThermostatDefinition);
    const std::string xmlFromRuntimeCompiled = exportEndpointToKaenxXml(runtime.compiledDefinition());

    TEST_ASSERT_EQUAL_UINT(3u, descriptorFromDefinition.kCommunicationObjectCount);
    TEST_ASSERT_EQUAL_UINT(3u, descriptorFromRuntimeCompiled.kCommunicationObjectCount);
    TEST_ASSERT_EQUAL_UINT16(descriptorFromDefinition.communicationObjects[2].logicalId,
                             descriptorFromRuntimeCompiled.communicationObjects[2].logicalId);
    TEST_ASSERT_EQUAL_UINT16(descriptorFromDefinition.communicationObjects[2].exportNumber,
                             descriptorFromRuntimeCompiled.communicationObjects[2].exportNumber);
    TEST_ASSERT_TRUE(descriptorFromDefinition.communicationObjects[2].key
                     == descriptorFromRuntimeCompiled.communicationObjects[2].key);
    TEST_ASSERT_EQUAL(descriptorFromDefinition.features.readResponsesEnabled,
                      descriptorFromRuntimeCompiled.features.readResponsesEnabled);
    TEST_ASSERT_EQUAL_STRING(jsonFromDefinition.c_str(), jsonFromRuntimeCompiled.c_str());
    TEST_ASSERT_EQUAL_STRING(xmlFromDefinition.c_str(), xmlFromRuntimeCompiled.c_str());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_product_export_bridge_serializes_proof_products_through_existing_exporter_pipeline);
    RUN_TEST(test_product_export_bridge_uses_the_same_compiled_source_as_runtime);
    return UNITY_END();
}