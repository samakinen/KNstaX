// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/product/endpoint.hpp"

#include <cmath>

using namespace knx;
using namespace knx::application;
using namespace knx::product;

enum class RuntimeSwitchPort : uint16_t {
    RelayCommand = 8,
    RelayState = 41,
};

using RuntimeRelayCommandPort = CommandPort<RuntimeSwitchPort::RelayCommand,
                                            bool,
                                            "relay_command",
                                            "Relay command",
                                            dptids::Switch>;
using RuntimeRelayStatePort = StatePort<RuntimeSwitchPort::RelayState,
                                        bool,
                                        "relay_state",
                                        "Relay state",
                                        dptids::Switch,
                                        true>;

constexpr auto kRuntimeSwitchDefinition =
    makeEndpointDefinition<RuntimeSwitchPort, RuntimeRelayCommandPort, RuntimeRelayStatePort>(
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

enum class RuntimeThermostatPort : uint16_t {
    Setpoint = 14,
    MeasuredTemperature = 77,
    HvacMode = 103,
};

using RuntimeSetpointPort = StateInOutPort<RuntimeThermostatPort::Setpoint,
                                           float,
                                           "setpoint_temperature",
                                           "Setpoint temperature",
                                           dptids::Temperature,
                                           true>;
using RuntimeMeasuredTemperaturePort = StatePort<RuntimeThermostatPort::MeasuredTemperature,
                                                 float,
                                                 "measured_temperature",
                                                 "Measured temperature",
                                                 dptids::Temperature,
                                                 false>;
using RuntimeHvacModePort = StateInOutPort<RuntimeThermostatPort::HvacMode,
                                           Dpt20Mode,
                                           "hvac_mode",
                                           "HVAC mode",
                                           dptids::HvacMode,
                                           true>;

constexpr auto kRuntimeThermostatDefinition =
    makeEndpointDefinition<RuntimeThermostatPort,
                           RuntimeSetpointPort,
                           RuntimeMeasuredTemperaturePort,
                           RuntimeHvacModePort>(
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

void setUp(void) {}
void tearDown(void) {}

void test_product_runtime_core_mediates_incoming_writes_and_outgoing_updates(void)
{
    bool relayState = false;
    bool programmingMode = false;
    bool faultSeen = false;

    auto bindings = EndpointBindings<decltype(kRuntimeSwitchDefinition)>{}
                        .onCommand<RuntimeSwitchPort::RelayCommand>([&](bool value) {
                            relayState = value;
                        })
                        .provideState<RuntimeSwitchPort::RelayState>([&]() {
                            return relayState;
                        })
                        .onProgrammingModeChanged([&](bool active) {
                            programmingMode = active;
                        })
                        .onFault([&](knx::product::FaultInfo) {
                            faultSeen = true;
                        });

    EndpointRuntime<decltype(kRuntimeSwitchDefinition)> runtime(
        kRuntimeSwitchDefinition,
        std::move(bindings),
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 10),
            .persistenceNamespace = "switch_0",
            .restoreKnxStateOnBoot = true,
        });

    TEST_ASSERT_EQUAL_UINT16(IndividualAddress(1, 1, 10).value(), runtime.instanceConfig().defaultIndividualAddress.value());
    TEST_ASSERT_TRUE(runtime.instanceConfig().persistenceNamespace == "switch_0");

    const auto writeResult = runtime.handleIncomingWrite<RuntimeSwitchPort::RelayCommand>(true);
    TEST_ASSERT_TRUE(writeResult.isOk());
    TEST_ASSERT_TRUE(relayState);

    TEST_ASSERT_TRUE(runtime.publish<RuntimeSwitchPort::RelayState>(relayState).isOk());
    TEST_ASSERT_TRUE(runtime.requestReadResponse<RuntimeSwitchPort::RelayState>().isOk());
    const auto publishDenied = runtime.publish<RuntimeSwitchPort::RelayCommand>(true);
    TEST_ASSERT_TRUE(publishDenied.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::AccessDenied), static_cast<int>(publishDenied.error()));
    TEST_ASSERT_EQUAL_UINT(2u, runtime.pendingActionCount());

    const auto* publishAction = runtime.pendingActionAt(0u);
    TEST_ASSERT_NOT_NULL(publishAction);
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(PendingBusActionKind::Publish),
                           static_cast<unsigned int>(publishAction->kind));
    TEST_ASSERT_EQUAL_UINT16(1u, publishAction->slot);
    TEST_ASSERT_TRUE(publishAction->value.asBool());

    const auto* readResponseAction = runtime.pendingActionAt(1u);
    TEST_ASSERT_NOT_NULL(readResponseAction);
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(PendingBusActionKind::ReadResponse),
                           static_cast<unsigned int>(readResponseAction->kind));
    TEST_ASSERT_EQUAL_UINT16(1u, readResponseAction->slot);
    TEST_ASSERT_TRUE(readResponseAction->value.asBool());

    runtime.toggleProgrammingMode();
    TEST_ASSERT_TRUE(runtime.isProgrammingModeActive());
    TEST_ASSERT_TRUE(programmingMode);

    runtime.reportFault({knx::product::FaultCode::InternalError, "runtime"});
    TEST_ASSERT_TRUE(faultSeen);
}

void test_product_runtime_core_derives_read_response_and_state_writes_from_bindings(void)
{
    float setpoint = 0.0f;
    float measuredTemperature = 21.5f;
    Dpt20Mode mode = Dpt20Mode::Auto;

    auto bindings = EndpointBindings<decltype(kRuntimeThermostatDefinition)>{}
                        .onStateWrite<RuntimeThermostatPort::Setpoint>([&](float value) {
                            setpoint = value;
                        })
                        .provideState<RuntimeThermostatPort::MeasuredTemperature>([&]() {
                            return measuredTemperature;
                        })
                        .onCommand<RuntimeThermostatPort::HvacMode>([&](Dpt20Mode value) {
                            mode = value;
                        });

    EndpointRuntime<decltype(kRuntimeThermostatDefinition)> runtime(
        kRuntimeThermostatDefinition,
        std::move(bindings),
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 20),
            .persistenceNamespace = "thermostat_0",
            .restoreKnxStateOnBoot = true,
        });

    TEST_ASSERT_TRUE(runtime.handleIncomingWrite<RuntimeThermostatPort::Setpoint>(23.5f).isOk());
    TEST_ASSERT_TRUE(std::fabs(setpoint - 23.5f) < 0.001f);

    TEST_ASSERT_TRUE(runtime.handleIncomingWrite<RuntimeThermostatPort::HvacMode>(Dpt20Mode::Comfort).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(Dpt20Mode::Comfort), static_cast<int>(mode));

    TEST_ASSERT_TRUE(runtime.requestReadResponse<RuntimeThermostatPort::MeasuredTemperature>().isOk());
    const auto* readResponseAction = runtime.pendingActionAt(0u);
    TEST_ASSERT_NOT_NULL(readResponseAction);
    TEST_ASSERT_EQUAL_UINT16(1u, readResponseAction->slot);
    TEST_ASSERT_TRUE(std::fabs(readResponseAction->value.asFloat() - measuredTemperature) < 0.001f);

    const auto publishSetpoint = runtime.publish<RuntimeThermostatPort::Setpoint>(setpoint);
    TEST_ASSERT_TRUE(publishSetpoint.isOk());

    const auto* publishAction = runtime.pendingActionAt(1u);
    TEST_ASSERT_NOT_NULL(publishAction);
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(PendingBusActionKind::Publish),
                           static_cast<unsigned int>(publishAction->kind));
    TEST_ASSERT_EQUAL_UINT16(0u, publishAction->slot);
    TEST_ASSERT_TRUE(std::fabs(publishAction->value.asFloat() - setpoint) < 0.001f);
}

void test_product_runtime_core_reports_missing_state_provider_as_not_ready(void)
{
    auto bindings = EndpointBindings<decltype(kRuntimeSwitchDefinition)>{};
    EndpointRuntime<decltype(kRuntimeSwitchDefinition)> runtime(
        kRuntimeSwitchDefinition,
        std::move(bindings),
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 30),
            .persistenceNamespace = "switch_1",
            .restoreKnxStateOnBoot = false,
        });

    const auto readResponse = runtime.requestReadResponse<RuntimeSwitchPort::RelayState>();
    TEST_ASSERT_TRUE(readResponse.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::OperationNotReady), static_cast<int>(readResponse.error()));
}

void test_product_runtime_core_owner_work_hint_tracks_pending_actions_and_notification_edges(void)
{
    bool relayState = true;
    unsigned int workAvailableCount = 0u;

    auto bindings = EndpointBindings<decltype(kRuntimeSwitchDefinition)>{}
                        .provideState<RuntimeSwitchPort::RelayState>([&]() {
                            return relayState;
                        });

    EndpointRuntime<decltype(kRuntimeSwitchDefinition)> runtime(
        kRuntimeSwitchDefinition,
        std::move(bindings),
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 40),
            .persistenceNamespace = "switch_2",
            .restoreKnxStateOnBoot = false,
        });

    runtime.setWorkAvailableCallback([&workAvailableCount]() {
        ++workAvailableCount;
    });

    auto hint = runtime.ownerWorkHint();
    TEST_ASSERT_FALSE(hint.hasImmediateWork());
    TEST_ASSERT_EQUAL_UINT(0u, hint.pendingLoopWorkItems);
    TEST_ASSERT_EQUAL_UINT(0u, hint.pendingDeferredWorkItems);
    TEST_ASSERT_FALSE(hint.maxSleepMs.has_value());

    TEST_ASSERT_TRUE(runtime.publish<RuntimeSwitchPort::RelayState>(relayState).isOk());

    hint = runtime.ownerWorkHint();
    TEST_ASSERT_TRUE(hint.hasImmediateWork());
    TEST_ASSERT_TRUE(hint.shouldCallLoop());
    TEST_ASSERT_EQUAL_UINT(1u, hint.pendingLoopWorkItems);
    TEST_ASSERT_EQUAL_UINT(0u, hint.pendingDeferredWorkItems);
    TEST_ASSERT_EQUAL_UINT(1u, workAvailableCount);

    TEST_ASSERT_TRUE(runtime.requestReadResponse<RuntimeSwitchPort::RelayState>().isOk());

    hint = runtime.ownerWorkHint();
    TEST_ASSERT_EQUAL_UINT(2u, hint.pendingLoopWorkItems);
    TEST_ASSERT_EQUAL_UINT(1u, workAvailableCount);

    runtime.clearPendingActions();

    hint = runtime.ownerWorkHint();
    TEST_ASSERT_FALSE(hint.hasImmediateWork());
    TEST_ASSERT_EQUAL_UINT(0u, hint.pendingLoopWorkItems);

    TEST_ASSERT_TRUE(runtime.publish<RuntimeSwitchPort::RelayState>(relayState).isOk());

    hint = runtime.ownerWorkHint();
    TEST_ASSERT_EQUAL_UINT(1u, hint.pendingLoopWorkItems);
    TEST_ASSERT_EQUAL_UINT(2u, workAvailableCount);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_product_runtime_core_mediates_incoming_writes_and_outgoing_updates);
    RUN_TEST(test_product_runtime_core_derives_read_response_and_state_writes_from_bindings);
    RUN_TEST(test_product_runtime_core_reports_missing_state_provider_as_not_ready);
    RUN_TEST(test_product_runtime_core_owner_work_hint_tracks_pending_actions_and_notification_edges);
    return UNITY_END();
}