// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/product/endpoint.hpp"

#include <string>

using namespace knx;
using namespace knx::application;
using namespace knx::product::endpoint;

namespace {

enum class PublicSwitchPort : uint16_t {
    RelayCommand = 7,
    RelayState = 9,
};

constexpr auto kPublicSwitchDefinition =
    makeEndpointDefinition<PublicSwitchPort,
                           semantics::SwitchCommand<PublicSwitchPort::RelayCommand, "relay_command">,
                           semantics::SwitchState<PublicSwitchPort::RelayState, "relay_state">>(
        ProductIdentity{
            .productKey = "public_switch",
            .productDisplayName = "Public Switch",
            .manufacturerId = ManufacturerId(0x00FA),
            .medium = Medium::TP1,
            .applicationNumber = 7,
            .applicationVersion = 1,
            .firmwareRevision = 1,
            .maxApduLength = 254,
        },
        PersistencePolicy{
            .namespacePrefix = "public_switch",
            .schemaVersion = 1,
            .persistKnxState = true,
        });

constexpr auto kCompiledPublicSwitch = compileEndpointDefinition(kPublicSwitchDefinition);

static_assert(port_index_v<decltype(kPublicSwitchDefinition), PublicSwitchPort::RelayCommand> == 0u);
static_assert(port_index_v<decltype(kPublicSwitchDefinition), PublicSwitchPort::RelayState> == 1u);
static_assert(kCompiledPublicSwitch.runtime.communicationObjects[1].logicalId == PublicSwitchPort::RelayState);

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_product_endpoint_public_surface_supports_definition_runtime_and_export(void)
{
    bool relayState = false;

    auto bindings = makeEndpointBindings(kPublicSwitchDefinition)
                        .onCommand<PublicSwitchPort::RelayCommand>([&](bool on) {
                            relayState = on;
                        })
                        .provideState<PublicSwitchPort::RelayState>([&]() {
                            return relayState;
                        });

    auto runtime = makeEndpointRuntime(
        kPublicSwitchDefinition,
        std::move(bindings),
        EndpointInstanceConfig{
            .defaultIndividualAddress = IndividualAddress(1, 1, 70),
            .persistenceNamespace = "public_surface_0",
            .restoreKnxStateOnBoot = true,
        });

    TEST_ASSERT_EQUAL_UINT(2u, runtime.compiledDefinition().runtime.kCommunicationObjectCount);
    TEST_ASSERT_TRUE(bindGroupAddresses(
        runtime,
        groupAddressBinding(PublicSwitchPort::RelayCommand, GroupAddress(2, 1, 1)),
        groupAddressBinding(PublicSwitchPort::RelayState, GroupAddress(2, 1, 2)))
                         .isOk());
    TEST_ASSERT_TRUE(runtime.handleIncomingWrite<PublicSwitchPort::RelayCommand>(true).isOk());
    TEST_ASSERT_TRUE(relayState);
    TEST_ASSERT_TRUE(runtime.publish(PublicSwitchPort::RelayState, relayState).isOk());
    TEST_ASSERT_EQUAL_UINT(1u, runtime.pendingActionCount());

    const auto descriptor = makeEndpointExportDescriptor(runtime.compiledDefinition());
    const std::string json = exportEndpointToJson(runtime.compiledDefinition());

    TEST_ASSERT_EQUAL_UINT16(2u, descriptor.kCommunicationObjectCount);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(PublicSwitchPort::RelayState),
                             descriptor.communicationObjects[1].logicalId);
    TEST_ASSERT_TRUE(json.find("\"public_switch\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"relay_state\"") != std::string::npos);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_product_endpoint_public_surface_supports_definition_runtime_and_export);
    return UNITY_END();
}