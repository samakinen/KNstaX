// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/product/endpoint.hpp"

using namespace knx;
using namespace knx::application;
using namespace knx::product::endpoint;

namespace {

enum class DimmerPort : uint16_t {
    Switch = 0,
    Level = 1,
    Dimming = 2,
    Status = 3,
};

constexpr auto kDimmerDefinition = makeEndpointDefinition<
    DimmerPort,
    semantics::SwitchCommand<DimmerPort::Switch, "switch", "Switch">,
    semantics::PercentCommand<DimmerPort::Level, "level", "Dimming level">,
    semantics::RelativeDimmingCommand<DimmerPort::Dimming, "dimming", "Relative dimming">,
    semantics::PercentState<DimmerPort::Status, "status", "Status (level)", false>>(
    ProductIdentity{
        .productKey = "test_dimmer",
        .productDisplayName = "Test Dimmer",
        .manufacturerId = ManufacturerId(0x00FA),
        .medium = Medium::TP1,
        .applicationNumber = 10,
        .applicationVersion = 1,
        .firmwareRevision = 1,
        .maxApduLength = 254,
    },
    PersistencePolicy{
        .namespacePrefix = "test_dimmer",
        .schemaVersion = 1,
        .persistKnxState = true,
    });

enum class SensorPort : uint16_t {
    Temperature = 0,
    Humidity = 1,
    CO2 = 2,
    Illuminance = 3,
};

constexpr auto kSensorDefinition = makeEndpointDefinition<
    SensorPort,
    semantics::TemperatureState<SensorPort::Temperature, "temperature", "Temperature (C)", false>,
    semantics::HumidityState<SensorPort::Humidity, "humidity", "Relative humidity (%)", false>,
    semantics::Co2State<SensorPort::CO2, "co2", "CO2 concentration (ppm)", false>,
    semantics::IlluminanceState<SensorPort::Illuminance, "illuminance", "Illuminance (lux)", false>>(
    ProductIdentity{
        .productKey = "test_sensor",
        .productDisplayName = "Test Sensor",
        .manufacturerId = ManufacturerId(0x00FA),
        .medium = Medium::TP1,
        .applicationNumber = 11,
        .applicationVersion = 1,
        .firmwareRevision = 1,
        .maxApduLength = 254,
    },
    PersistencePolicy{
        .namespacePrefix = "test_sensor",
        .schemaVersion = 1,
        .persistKnxState = true,
    });

constexpr auto kCompiledDimmer = compileEndpointDefinition(kDimmerDefinition);
constexpr auto kCompiledSensor = compileEndpointDefinition(kSensorDefinition);

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_product_endpoint_semantics_compose_common_ports_without_device_factories(void)
{
    TEST_ASSERT_EQUAL_UINT(4u, kCompiledDimmer.runtime.kCommunicationObjectCount);
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(PortDirection::CommandIn),
                           static_cast<unsigned int>(kCompiledDimmer.runtime.communicationObjects[0].direction));
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(PortDirection::StateOut),
                           static_cast<unsigned int>(kCompiledDimmer.runtime.communicationObjects[3].direction));
    TEST_ASSERT_TRUE(kCompiledDimmer.runtime.communicationObjects[2].dpt == dptids::Dimming);
    TEST_ASSERT_TRUE(kCompiledSensor.runtime.communicationObjects[0].dpt == dptids::Temperature);
    TEST_ASSERT_TRUE(kCompiledSensor.runtime.communicationObjects[1].dpt == dptids::Humidity);
    TEST_ASSERT_TRUE(kCompiledSensor.runtime.communicationObjects[2].dpt == dptids::CO2);
    TEST_ASSERT_TRUE(kCompiledSensor.runtime.communicationObjects[3].dpt == dptids::Illuminance);
}

void test_product_endpoint_helpers_bind_and_publish_by_logical_port(void)
{
    uint8_t status = 42u;

    auto bindings = makeEndpointBindings(kDimmerDefinition)
                        .provideState<DimmerPort::Status>([&]() {
                            return status;
                        });

    auto runtime = makeEndpointRuntime(
        kDimmerDefinition,
        std::move(bindings),
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 42),
            .persistenceNamespace = "semantics_test_0",
            .restoreKnxStateOnBoot = true,
        });

    TEST_ASSERT_TRUE(bindGroupAddresses(
        runtime,
        groupAddressBinding(DimmerPort::Switch, GroupAddress(2, 1, 1)),
        groupAddressBinding(DimmerPort::Level, GroupAddress(2, 1, 2)),
        groupAddressBinding(DimmerPort::Dimming, GroupAddress(2, 1, 3)),
        groupAddressBinding(DimmerPort::Status, GroupAddress(2, 1, 4)))
                         .isOk());

    TEST_ASSERT_TRUE(runtime.publish(DimmerPort::Status, status).isOk());
    TEST_ASSERT_EQUAL_UINT(1u, runtime.pendingActionCount());

    const auto* action = runtime.pendingActionAt(0u);
    TEST_ASSERT_NOT_NULL(action);
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(DimmerPort::Status),
                           static_cast<unsigned int>(action->logicalId));
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(PendingBusActionKind::Publish),
                           static_cast<unsigned int>(action->kind));
    TEST_ASSERT_EQUAL_UINT(42u, action->value.asUInt8());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_product_endpoint_semantics_compose_common_ports_without_device_factories);
    RUN_TEST(test_product_endpoint_helpers_bind_and_publish_by_logical_port);
    return UNITY_END();
}