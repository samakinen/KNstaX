// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/product/endpoint.hpp"

#include <cmath>

using namespace knx;
using namespace knx::application;
using namespace knx::product::endpoint;

namespace {

enum class PolicyPort : uint16_t {
    Temperature = 4,
};

using PolicyTemperaturePort = StatePort<PolicyPort::Temperature,
                                        float,
                                        "temperature",
                                        "Temperature",
                                        dptids::Temperature,
                                        true>;

constexpr auto kPolicyDefinition =
    makeEndpointDefinition<PolicyPort, PolicyTemperaturePort>(
        ProductIdentity{
            .productKey = "policy_sensor",
            .productDisplayName = "Policy Sensor",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = Medium::TP1,
            .applicationNumber = 8,
            .applicationVersion = 1,
            .firmwareRevision = 1,
            .maxApduLength = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "policy_sensor",
            .schemaVersion = 1,
            .persistKnxState = true,
        });

using PolicyRuntime = EndpointRuntime<decltype(kPolicyDefinition)>;

void assertQueuedTemperature(const PolicyRuntime& runtime, float expected)
{
    TEST_ASSERT_EQUAL_UINT(1u, runtime.pendingActionCount());

    const auto* action = runtime.pendingActionAt(0u);
    TEST_ASSERT_NOT_NULL(action);
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(PendingBusActionKind::Publish),
                           static_cast<unsigned int>(action->kind));
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(PolicyPort::Temperature),
                             static_cast<uint16_t>(action->logicalId));
    TEST_ASSERT_TRUE(std::fabs(action->value.asFloat() - expected) < 0.001f);
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_product_endpoint_policies_periodic_publish_uses_typed_port_publish(void)
{
    float temperature = 20.0f;

    PolicyRuntime runtime(
        kPolicyDefinition,
        EndpointBindings<decltype(kPolicyDefinition)>{},
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 80),
            .persistenceNamespace = "policy_periodic",
            .restoreKnxStateOnBoot = true,
        });

    PeriodicPublishPolicy<decltype(kPolicyDefinition), PolicyPort::Temperature> policy(
        1000u,
        [&]() { return temperature; });

    TEST_ASSERT_TRUE(policy.tick(100u, runtime).isOk());
    TEST_ASSERT_EQUAL_UINT(0u, runtime.pendingActionCount());

    TEST_ASSERT_TRUE(policy.tick(1099u, runtime).isOk());
    TEST_ASSERT_EQUAL_UINT(0u, runtime.pendingActionCount());

    temperature = 21.25f;
    TEST_ASSERT_TRUE(policy.tick(1100u, runtime).isOk());
    assertQueuedTemperature(runtime, 21.25f);
}

void test_product_endpoint_policies_on_change_publishes_initial_and_threshold_crossing_values(void)
{
    float temperature = 20.0f;

    PolicyRuntime runtime(
        kPolicyDefinition,
        EndpointBindings<decltype(kPolicyDefinition)>{},
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 81),
            .persistenceNamespace = "policy_on_change",
            .restoreKnxStateOnBoot = true,
        });

    OnChangePolicy<decltype(kPolicyDefinition), PolicyPort::Temperature, float> policy(
        0.5f,
        [&]() { return temperature; });

    TEST_ASSERT_TRUE(policy.tick(0u, runtime).isOk());
    assertQueuedTemperature(runtime, 20.0f);

    runtime.clearPendingActions();
    temperature = 20.2f;
    TEST_ASSERT_TRUE(policy.tick(1u, runtime).isOk());
    TEST_ASSERT_EQUAL_UINT(0u, runtime.pendingActionCount());

    temperature = 20.6f;
    TEST_ASSERT_TRUE(policy.tick(2u, runtime).isOk());
    assertQueuedTemperature(runtime, 20.6f);
}

void test_product_endpoint_policies_combined_sensor_policy_avoids_duplicate_same_tick_publishes(void)
{
    float temperature = 20.0f;

    PolicyRuntime runtime(
        kPolicyDefinition,
        EndpointBindings<decltype(kPolicyDefinition)>{},
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 82),
            .persistenceNamespace = "policy_combined",
            .restoreKnxStateOnBoot = true,
        });

    CombinedSensorPolicy<decltype(kPolicyDefinition), PolicyPort::Temperature, float> policy(
        1000u,
        0.5f,
        [&]() { return temperature; });

    TEST_ASSERT_TRUE(policy.tick(50u, runtime).isOk());
    assertQueuedTemperature(runtime, 20.0f);

    runtime.clearPendingActions();
    temperature = 20.1f;
    TEST_ASSERT_TRUE(policy.tick(500u, runtime).isOk());
    TEST_ASSERT_EQUAL_UINT(0u, runtime.pendingActionCount());

    TEST_ASSERT_TRUE(policy.tick(1050u, runtime).isOk());
    assertQueuedTemperature(runtime, 20.1f);

    runtime.clearPendingActions();
    temperature = 20.8f;
    TEST_ASSERT_TRUE(policy.tick(1100u, runtime).isOk());
    assertQueuedTemperature(runtime, 20.8f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_product_endpoint_policies_periodic_publish_uses_typed_port_publish);
    RUN_TEST(test_product_endpoint_policies_on_change_publishes_initial_and_threshold_crossing_values);
    RUN_TEST(test_product_endpoint_policies_combined_sensor_policy_avoids_duplicate_same_tick_publishes);
    return UNITY_END();
}