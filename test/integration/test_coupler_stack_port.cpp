// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_coupler_stack_port.cpp
 * @brief A coupler assembled through the ordinary device path, not by hand.
 *
 * The routing algorithm is covered by test_coupler_routing.cpp and the
 * forwarding behaviour by test_two_port_coupler.cpp. What is pinned here is
 * that a coupler is reachable through `createTp1CouplerStackPort` and behaves
 * as a *device* as well as a router: it commissions, it publishes a Router
 * Object bound to the table the forwarding path actually reads, and it keeps
 * working when ETS moves its individual address.
 *
 * Before this existed the coupler could only be built by assembling two data
 * link layers by hand, which meant giving up the commissioned-product path
 * entirely — the gap these tests exist to keep closed.
 */

#include "unity.h"

#include "knx/bau/bau.hpp"
#include "knx/network/two_port_coupler.hpp"
#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/platform/linux_platform.hpp"

#include <memory>

using namespace knx;
using namespace knx::bau;

namespace {

constexpr uint16_t ia(uint8_t area, uint8_t line, uint8_t device)
{
    return static_cast<uint16_t>((area << 12) | (line << 8) | device);
}

/// Two ports over null backends: enough to exercise assembly, addressing and
/// Router Object binding without needing a bus on either side.
std::unique_ptr<BusAccessStackPort> makeCouplerStackPort(platform::Platform& platform)
{
    return createTp1CouplerStackPort(
        platform,
        std::make_unique<physical::Tp1MacPhysical>(
            std::make_unique<physical::NullTp1MediumBackend>()),
        std::make_unique<physical::Tp1MacPhysical>(
            std::make_unique<physical::NullTp1MediumBackend>()));
}

std::unique_ptr<BusAccessStackPort> makeOrdinaryStackPort(platform::Platform& platform)
{
    return createTp1StackPort(platform,
                              std::make_unique<physical::Tp1MacPhysical>(
                                  std::make_unique<physical::NullTp1MediumBackend>()));
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_a_coupler_stack_port_exposes_its_coupler(void)
{
    platform::LinuxPlatform platform;
    auto stackPort = makeCouplerStackPort(platform);
    TEST_ASSERT_NOT_NULL(stackPort.get());

    // Before init() there is no coupler: its role depends on the individual
    // address, which is not known until then.
    TEST_ASSERT_NULL(stackPort->coupler());

    TEST_ASSERT_TRUE(stackPort->init(IndividualAddress(ia(1, 1, 0))).isOk());

    auto* coupler = stackPort->coupler();
    TEST_ASSERT_NOT_NULL(coupler);
    TEST_ASSERT_EQUAL(static_cast<int>(network::CouplerRole::LineCoupler),
                      static_cast<int>(coupler->role()));
    TEST_ASSERT_EQUAL_HEX16(ia(1, 1, 0), coupler->ownAddress().raw);
}

void test_an_ordinary_stack_port_has_no_coupler(void)
{
    // The discriminator the rest of the stack relies on: asking an end device
    // for its coupler must be answerable, not undefined.
    platform::LinuxPlatform platform;
    auto stackPort = makeOrdinaryStackPort(platform);
    TEST_ASSERT_TRUE(stackPort->init(IndividualAddress(ia(1, 1, 5))).isOk());
    TEST_ASSERT_NULL(stackPort->coupler());
}

void test_the_role_follows_the_address_ets_assigns(void)
{
    // A coupler is commissioned like any other device: it comes up on some
    // address and ETS moves it. The routing role has to move with it, or the
    // device would answer to the new address while routing for the old one.
    platform::LinuxPlatform platform;
    auto stackPort = makeCouplerStackPort(platform);
    TEST_ASSERT_TRUE(stackPort->init(IndividualAddress(ia(1, 1, 0))).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(network::CouplerRole::LineCoupler),
                      static_cast<int>(stackPort->coupler()->role()));

    // Promoted to a backbone coupler.
    TEST_ASSERT_TRUE(stackPort->setOwnAddress(IndividualAddress(ia(2, 0, 0))).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(network::CouplerRole::BackboneCoupler),
                      static_cast<int>(stackPort->coupler()->role()));
    TEST_ASSERT_EQUAL_HEX16(ia(2, 0, 0), stackPort->coupler()->ownAddress().raw);
}

void test_an_uncommissioned_coupler_falls_back_to_repeater(void)
{
    // A coupler fresh out of the box has no meaningful address. Behaving as a
    // repeater keeps traffic flowing; guessing a subnetwork would silently
    // route against the wrong one.
    platform::LinuxPlatform platform;
    auto stackPort = makeCouplerStackPort(platform);
    TEST_ASSERT_TRUE(stackPort->init(IndividualAddress(ia(0, 0, 255))).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(network::CouplerRole::Repeater),
                      static_cast<int>(stackPort->coupler()->role()));
}

void test_the_router_object_binds_to_the_couplers_own_filter_table(void)
{
    // The failure this prevents: a Router Object bound to some *other* filter
    // table. ETS would download filter entries successfully, the coupler would
    // report success, and no forwarding decision would ever consult them.
    platform::LinuxPlatform platform;
    auto stackPort = makeCouplerStackPort(platform);
    auto* stackPortRaw = stackPort.get();

    BusAccessUnit bau(platform, std::move(stackPort));
    TEST_ASSERT_TRUE(bau.init(IndividualAddress(ia(1, 1, 0)), "knx_coupler_test", 1).isOk());

    // No filter table supplied on purpose: it must come from the coupler.
    BusAccessUnit::RouterRoleConfig config;
    TEST_ASSERT_TRUE(bau.management().configureRouterRole(config).isOk());
    TEST_ASSERT_TRUE(bau.management().hasRouterRole());

    auto* coupler = stackPortRaw->coupler();
    TEST_ASSERT_NOT_NULL(coupler);

    // Writing through the coupler's table must be visible to the routing
    // policy, which is the same object.
    TEST_ASSERT_EQUAL(0u, coupler->filterTable().entryCount());
    TEST_ASSERT_TRUE(coupler->filterTable()
                         .addEntry(GroupAddress(0x0801), GroupAddress(0xFFFF),
                                   network::FilterAction::Allow, EntryState::Enabled)
                         .isOk());
    TEST_ASSERT_EQUAL(1u, coupler->filterTable().entryCount());
    TEST_ASSERT_TRUE(&coupler->filterTable() == coupler->policy().filterTable());
}

void test_configure_router_role_still_refuses_on_a_non_coupler(void)
{
    // The auto-wiring must not weaken the original guard: an end device that
    // asks for a Router Object without supplying a filter table is declaring
    // routing it cannot perform.
    platform::LinuxPlatform platform;
    BusAccessUnit bau(platform, makeOrdinaryStackPort(platform));
    TEST_ASSERT_TRUE(bau.init(IndividualAddress(ia(1, 1, 5)), "knx_coupler_test", 1).isOk());

    BusAccessUnit::RouterRoleConfig config;
    TEST_ASSERT_TRUE(bau.management().configureRouterRole(config).isError());
}

void test_downloaded_coupler_configuration_reaches_the_policy(void)
{
    // PID_MAIN_LCCONFIG and friends are ordinary properties: ETS writes them
    // into the Router Object and nothing else happens until they are synced.
    platform::LinuxPlatform platform;
    auto stackPort = makeCouplerStackPort(platform);
    auto* stackPortRaw = stackPort.get();

    BusAccessUnit bau(platform, std::move(stackPort));
    TEST_ASSERT_TRUE(bau.init(IndividualAddress(ia(1, 1, 0)), "knx_coupler_test", 1).isOk());
    TEST_ASSERT_TRUE(bau.management().configureRouterRole({}).isOk());

    auto* coupler = stackPortRaw->coupler();
    TEST_ASSERT_NOT_NULL(coupler);

    // The seeded defaults are the specification's, and they must already be in
    // the policy — a coupler is configured before ETS ever writes to it.
    TEST_ASSERT_EQUAL(static_cast<int>(network::PhysFrameHandling::Route),
                      static_cast<int>(
                          coupler->policy().lineConfig(network::CouplerPort::Primary).physFrame));

    // ETS writes PHYS_LOCK on the primary side: 0x77 with bits 0-1 set to 10.
    const std::array<uint8_t, 1> locked{0x76};
    TEST_ASSERT_TRUE(bau.management()
                         .setReferenceObjectProperty(InterfaceObjectType::router(),
                                                     static_cast<application::PropertyID>(52),
                                                     locked)
                         .isOk());

    // Nothing has changed yet — that is the whole reason sync exists.
    TEST_ASSERT_EQUAL(static_cast<int>(network::PhysFrameHandling::Route),
                      static_cast<int>(
                          coupler->policy().lineConfig(network::CouplerPort::Primary).physFrame));

    TEST_ASSERT_TRUE(bau.management().syncRouterRoutingConfig().isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(network::PhysFrameHandling::Lock),
                      static_cast<int>(
                          coupler->policy().lineConfig(network::CouplerPort::Primary).physFrame));

    // ...and only the side that was written.
    TEST_ASSERT_EQUAL(static_cast<int>(network::PhysFrameHandling::Route),
                      static_cast<int>(
                          coupler->policy().lineConfig(network::CouplerPort::Secondary).physFrame));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_coupler_stack_port_exposes_its_coupler);
    RUN_TEST(test_an_ordinary_stack_port_has_no_coupler);
    RUN_TEST(test_the_role_follows_the_address_ets_assigns);
    RUN_TEST(test_an_uncommissioned_coupler_falls_back_to_repeater);
    RUN_TEST(test_the_router_object_binds_to_the_couplers_own_filter_table);
    RUN_TEST(test_configure_router_role_still_refuses_on_a_non_coupler);
    RUN_TEST(test_downloaded_coupler_configuration_reaches_the_policy);
    return UNITY_END();
}
