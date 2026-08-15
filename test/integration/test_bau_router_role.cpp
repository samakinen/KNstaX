// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_bau_router_role.cpp
 * @brief The Router Object is opt-in, and once opted in it is functional.
 *
 * Two properties are being protected here, and they pull in opposite
 * directions:
 *
 *  - A plain end device must NOT publish a Router Object. Advertising one tells
 *    ETS the device forwards frames; an integrator would then configure a
 *    filter table that nothing consults, and the misconfiguration is invisible.
 *  - A device that HAS declared a coupler role must expose an object ETS can
 *    actually configure — an object that reads back zeros and ignores
 *    PID_ROUTETABLE_CONTROL would be the same trap with extra steps.
 */

#include "unity.h"

#include "knx/bau/bau.hpp"
#include "knx/network/filter_table.hpp"
#include "knx/network/routing_table_control.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/platform/linux_platform.hpp"

#include <filesystem>
#include <memory>
#include <vector>

using namespace knx;

namespace {

std::unique_ptr<platform::LinuxPlatform> gPlatform;
std::unique_ptr<bau::BusAccessUnit> gBau;
network::FilterTable gFilterTable;

void clearPersistenceStore() {
    std::error_code ec;
    (void)std::filesystem::remove_all(objects::persistenceNamespaceDir("knx_objects"), ec);
}

/// Fresh BAU over a null TP1 backend — enough to exercise interface-object
/// registration without a bus.
void makeBau() {
    gPlatform = std::make_unique<platform::LinuxPlatform>();
    gBau = std::make_unique<bau::BusAccessUnit>(
        *gPlatform, std::unique_ptr<physical::Tp1MediumBackend>(new physical::NullTp1MediumBackend()));
}

bool routerObjectRegistered() {
    const auto& types = gBau->management().referenceInterfaceObjectTypes();
    for (const auto type : types) {
        if (type == InterfaceObjectType::router()) {
            return true;
        }
    }
    return false;
}

} // namespace

void setUp(void) {
    clearPersistenceStore();
    gFilterTable = network::FilterTable{};
    makeBau();
}

void tearDown(void) {
    if (gBau) {
        gBau->close();
    }
    gBau.reset();
    gPlatform.reset();
    clearPersistenceStore();
}

void test_end_device_does_not_publish_router_object(void) {
    TEST_ASSERT_TRUE(gBau->init(IndividualAddress(1, 1, 1)).isOk());

    TEST_ASSERT_FALSE(gBau->management().hasRouterRole());
    TEST_ASSERT_FALSE(routerObjectRegistered());
}

void test_router_role_requires_a_filter_table(void) {
    // Without a table the object would be configurable but inert, so the
    // configuration is refused rather than half-applied.
    bau::BusAccessUnit::RouterRoleConfig config{};
    config.filterTable = nullptr;

    TEST_ASSERT_TRUE(gBau->management().configureRouterRole(config).isError());
    TEST_ASSERT_FALSE(gBau->management().hasRouterRole());

    TEST_ASSERT_TRUE(gBau->init(IndividualAddress(1, 1, 1)).isOk());
    TEST_ASSERT_FALSE(routerObjectRegistered());
}

void test_declared_coupler_publishes_router_object(void) {
    bau::BusAccessUnit::RouterRoleConfig config{};
    config.filterTable = &gFilterTable;
    config.subMedium = 0;  // TP1
    config.hopCount = 5;
    config.maxRoutedApduLength = 254;

    TEST_ASSERT_TRUE(gBau->management().configureRouterRole(config).isOk());
    TEST_ASSERT_TRUE(gBau->management().hasRouterRole());

    TEST_ASSERT_TRUE(gBau->init(IndividualAddress(1, 1, 0)).isOk());
    TEST_ASSERT_TRUE(routerObjectRegistered());
}

void test_router_role_can_be_declared_after_init(void) {
    // A coupler runtime may only know its topology after the stack is up.
    TEST_ASSERT_TRUE(gBau->init(IndividualAddress(1, 1, 0)).isOk());
    TEST_ASSERT_FALSE(routerObjectRegistered());

    bau::BusAccessUnit::RouterRoleConfig config{};
    config.filterTable = &gFilterTable;
    TEST_ASSERT_TRUE(gBau->management().configureRouterRole(config).isOk());

    TEST_ASSERT_TRUE(routerObjectRegistered());
}

void test_routing_table_control_reaches_the_couplers_own_table(void) {
    // The whole point of binding the filter table into the role: an ETS
    // filter-table download must land on the table the forwarding path reads,
    // not on a private copy.
    bau::BusAccessUnit::RouterRoleConfig config{};
    config.filterTable = &gFilterTable;
    TEST_ASSERT_TRUE(gBau->management().configureRouterRole(config).isOk());
    TEST_ASSERT_TRUE(gBau->init(IndividualAddress(1, 1, 0)).isOk());

    gFilterTable.setDefaultAction(network::FilterAction::Allow);

    // SRVID_CLEAR_ROUTINGTABLE through the same service object the BAU's
    // Function Property handler uses.
    network::RoutingTableControl control(gFilterTable);
    const std::vector<uint8_t> clearRequest{
        0x00, static_cast<uint8_t>(network::RouteTableServiceId::ClearRoutingTable)};
    const auto res =
        control.invoke(application::FunctionPropertyInvocation::Command, clearRequest);
    TEST_ASSERT_TRUE(res.isOk());

    TEST_ASSERT_EQUAL(static_cast<int>(network::FilterAction::Block),
                      static_cast<int>(gFilterTable.defaultAction()));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_end_device_does_not_publish_router_object);
    RUN_TEST(test_router_role_requires_a_filter_table);
    RUN_TEST(test_declared_coupler_publishes_router_object);
    RUN_TEST(test_router_role_can_be_declared_after_init);
    RUN_TEST(test_routing_table_control_reaches_the_couplers_own_table);
    return UNITY_END();
}
