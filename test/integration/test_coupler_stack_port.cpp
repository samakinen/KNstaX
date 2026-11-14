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
#include "knx/network/filter_table_load_control.hpp"
#include "knx/network/two_port_coupler.hpp"
#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/platform/linux_platform.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

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

namespace {
void clearCouplerStore()
{
    std::error_code ec;
    (void)std::filesystem::remove_all(objects::persistenceNamespaceDir("knx_coupler_test"), ec);
}
} // namespace

// Every test starts as a device that has never been downloaded.
void setUp(void) { clearCouplerStore(); }
void tearDown(void) { clearCouplerStore(); }

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
    TEST_ASSERT_EQUAL_UINT32(0u, coupler->filterTable().passCount());
    coupler->filterTable().set(GroupAddress(0x0801), true);
    TEST_ASSERT_TRUE(coupler->policy().filterTable()->passes(GroupAddress(0x0801)));
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

void test_filter_table_load_state_drives_the_routing_policy(void)
{
    // 03/05/01 §5.3.1.4: the Filter Table evaluation is negative in every
    // load state but Loaded. The Router Object's PID_LOAD_STATE_CONTROL is
    // a property write; the coupler has to follow it.
    platform::LinuxPlatform platform;
    auto stackPort = makeCouplerStackPort(platform);
    auto* stackPortRaw = stackPort.get();

    BusAccessUnit bau(platform, std::move(stackPort));
    TEST_ASSERT_TRUE(bau.init(IndividualAddress(ia(1, 1, 0)), "knx_coupler_test", 1).isOk());
    TEST_ASSERT_TRUE(bau.management().configureRouterRole({}).isOk());
    auto* coupler = stackPortRaw->coupler();
    auto* load = bau.management().filterTableLoadControl();
    TEST_ASSERT_NOT_NULL(coupler);
    TEST_ASSERT_NOT_NULL(load);
    TEST_ASSERT_TRUE(coupler->policy().isFilterTableLoaded());

    const auto loadEvent = [&](std::initializer_list<uint8_t> block) {
        std::array<uint8_t, 10> data{};
        std::copy(block.begin(), block.end(), data.begin());
        load->applyEvent(data);
    };
    loadEvent({0x04});
    TEST_ASSERT_FALSE(coupler->policy().isFilterTableLoaded());
    loadEvent({0x01});
    loadEvent({0x03, 0x0B, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00});
    TEST_ASSERT_FALSE(coupler->policy().isFilterTableLoaded());
    load->memory()[0x0801u >> 3] = static_cast<uint8_t>(1u << (0x0801u & 7u));
    loadEvent({0x02});
    TEST_ASSERT_TRUE(coupler->policy().isFilterTableLoaded());
    TEST_ASSERT_TRUE(coupler->filterTable().passes(GroupAddress(0x0801)));
}

namespace {

constexpr const char* kNamespace = "knx_coupler_test";

/// One coupler device on the persisted state of kNamespace, the way a product
/// starts it: init(), then the router role.
struct CouplerDevice {
    platform::LinuxPlatform platform;
    BusAccessStackPort* port{nullptr};
    std::unique_ptr<BusAccessUnit> bau;

    CouplerDevice()
    {
        auto stackPort = makeCouplerStackPort(platform);
        port = stackPort.get();
        bau = std::make_unique<BusAccessUnit>(platform, std::move(stackPort));
        TEST_ASSERT_TRUE(bau->init(IndividualAddress(ia(1, 1, 0)), kNamespace, 1).isOk());
        TEST_ASSERT_TRUE(bau->management().configureRouterRole({}).isOk());
        TEST_ASSERT_NOT_NULL(port->coupler());
        TEST_ASSERT_NOT_NULL(load());
    }
    ~CouplerDevice() { bau->close(); }

    network::TwoPortCoupler& coupler() { return *port->coupler(); }
    network::FilterTableLoadControl* load() { return bau->management().filterTableLoadControl(); }

    void loadEvent(std::initializer_list<uint8_t> block)
    {
        std::array<uint8_t, 10> data{};
        std::copy(block.begin(), block.end(), data.begin());
        load()->applyEvent(data);
    }

    /// The ETS sequence of 03/05/01 §5.3.1.5 with the MCB carrying the CRC.
    void download(const network::FilterTable& table)
    {
        loadEvent({0x04});
        loadEvent({0x01});
        loadEvent({0x03, 0x0B, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00});
        const auto source = table.octets();
        std::copy(source.begin(), source.end(), load()->memory().begin());
        const uint16_t crc = network::FilterTableLoadControl::crc16Ccitt(source);
        const std::array<uint8_t, 8> mcb{0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
                                         static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc)};
        TEST_ASSERT_TRUE(load()->writeMcb(mcb).isOk());
        loadEvent({0x02});
        TEST_ASSERT_TRUE(load()->loaded());
    }

    /// Whether a group frame to @p ga arriving on the main line is routed.
    bool routes(uint16_t ga)
    {
        datalink::LDataFrame frame{};
        frame.standardFrame = true;
        frame.priority = Priority::Low;
        frame.source = IndividualAddress(ia(1, 0, 5));
        frame.destination = GroupAddress(ga);
        frame.destinationType = AddressType::Group;
        frame.hopCount = 6;
        return network::isForwardingAction(coupler().policy().decide(network::CouplerPort::Primary, frame));
    }
};

network::FilterTable tableWith(uint16_t first, uint16_t last)
{
    network::FilterTable table;
    table.setRange(first, last, true);
    return table;
}

} // namespace

// A coupler that has never been downloaded is the new device of 03/05/01
// §4.5.3: "The default Load State shall be Loaded and the Routing Table
// cleared". Under GROUP_ROUT6FFF nothing up to 6FFFh is routed.
void test_a_new_coupler_comes_up_loaded_and_cleared(void)
{
    CouplerDevice device;
    TEST_ASSERT_TRUE(device.load()->loaded());
    TEST_ASSERT_TRUE(device.coupler().policy().isFilterTableLoaded());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(device.coupler().filterTable().passCount()));
    TEST_ASSERT_FALSE(device.routes(0x0801));
}

// The download survives the restart: the table, Loaded and the MCB come back
// and the forwarding path routes on them again. Before, a coupler came back
// Loaded with an empty table and blocked every group frame up to 6FFFh.
void test_downloaded_filter_table_survives_restart_and_routes(void)
{
    std::array<uint8_t, 8> mcb{};
    {
        CouplerDevice device;
        device.download(tableWith(0x0800, 0x08FF));
        TEST_ASSERT_TRUE(device.routes(0x0801));
        mcb = device.load()->mcb();
        device.bau->flushPendingPersistence();
    }

    {
        CouplerDevice device;
        TEST_ASSERT_TRUE(device.load()->loaded());
        TEST_ASSERT_TRUE(device.coupler().policy().isFilterTableLoaded());
        TEST_ASSERT_EQUAL_UINT32(256u, static_cast<uint32_t>(device.coupler().filterTable().passCount()));
        TEST_ASSERT_TRUE(device.load()->mcb() == mcb);
        TEST_ASSERT_TRUE(device.routes(0x0801));
        TEST_ASSERT_TRUE(device.routes(0x08FF));
        TEST_ASSERT_FALSE(device.routes(0x0900));

        // An Unload is just as persistent: the next boot does not route on
        // a table ETS declared invalid (§5.3.1.4).
        device.loadEvent({0x04});
        device.bau->flushPendingPersistence();
    }
    CouplerDevice after;
    TEST_ASSERT_EQUAL(static_cast<int>(network::FilterTableLoadControl::State::Unloaded),
                      static_cast<int>(after.load()->state()));
    TEST_ASSERT_FALSE(after.coupler().policy().isFilterTableLoaded());
    TEST_ASSERT_FALSE(after.routes(0x0801));
}

// The load state changes mark the persisted state dirty, so the coalesced
// flush saves a download without anything else asking for it.
void test_a_download_is_saved_by_the_coalesced_flush(void)
{
    {
        uint32_t nowMs = 1000;
        CouplerDevice device;
        device.bau->setTimeSource([&nowMs]() { return nowMs; });
        device.download(tableWith(0x0A00, 0x0A0F));
        device.bau->loop(); // the write burst starts
        nowMs = 2000;
        device.bau->loop(); // quiet: the save runs
        // No flushPendingPersistence(): power is simply lost.
    }
    CouplerDevice device;
    TEST_ASSERT_TRUE(device.load()->loaded());
    TEST_ASSERT_TRUE(device.routes(0x0A05));
    TEST_ASSERT_FALSE(device.routes(0x0A10));
}

// A reset in the middle of a save keeps the previous complete state: the
// record is written into the inactive bank and only the marker switch makes
// it current (InterfaceObjectManager::kPersistenceBankMarkerKeyId).
void test_power_loss_during_save_keeps_the_previous_table(void)
{
    {
        CouplerDevice device;
        device.download(tableWith(0x0800, 0x08FF));
        device.bau->flushPendingPersistence();
    }

    // What an interrupted save of a second download leaves: its record in
    // the other bank, the marker still naming the first.
    using objects::InterfaceObjectManager;
    auto store = objects::createPersistence();
    TEST_ASSERT_TRUE(store->init(kNamespace).isOk());
    std::vector<uint8_t> marker;
    TEST_ASSERT_TRUE(store->loadById(InterfaceObjectManager::kPersistenceBankMarkerKeyId, marker).isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(marker.size()));
    const uint16_t otherBankKey = marker[0] == 0u
        ? static_cast<uint16_t>(InterfaceObjectManager::kRouterFilterTableKeyId
                                | InterfaceObjectManager::kPersistenceBank1KeyBit)
        : InterfaceObjectManager::kRouterFilterTableKeyId;
    {
        network::FilterTable second = tableWith(0x1000, 0x10FF);
        network::FilterTableLoadControl control(second);
        std::vector<uint8_t> record;
        control.serialize(record);
        TEST_ASSERT_TRUE(store->saveById(otherBankKey, record).isOk());
    }
    store->close();

    CouplerDevice device;
    TEST_ASSERT_TRUE(device.load()->loaded());
    TEST_ASSERT_TRUE(device.routes(0x0801));
    TEST_ASSERT_FALSE(device.routes(0x1001));

    // The half-written bank is cleared at the restore.
    auto check = objects::createPersistence();
    TEST_ASSERT_TRUE(check->init(kNamespace).isOk());
    auto exists = check->existsById(otherBankKey);
    TEST_ASSERT_TRUE(exists.isOk());
    TEST_ASSERT_FALSE(exists.value());
    check->close();
}

// A save that fails (flash error) leaves the previous bank in force, and the
// device boots with the previous table.
void test_failed_save_keeps_the_previous_table(void)
{
    namespace fs = std::filesystem;
    const fs::path store = objects::persistenceNamespaceDir(kNamespace);
    const fs::path aside = store.string() + ".aside";
    {
        CouplerDevice device;
        device.download(tableWith(0x0800, 0x08FF));
        device.bau->flushPendingPersistence();

        std::error_code ec;
        fs::remove_all(aside, ec);
        fs::rename(store, aside);
        { std::ofstream blocker(store); blocker << "not a directory"; }

        device.download(tableWith(0x1000, 0x10FF));
        device.bau->flushPendingPersistence(); // fails
        fs::remove(store);
        fs::rename(aside, store);
        // Power lost before the retry.
    }
    CouplerDevice device;
    TEST_ASSERT_TRUE(device.load()->loaded());
    TEST_ASSERT_TRUE(device.routes(0x0801));
    TEST_ASSERT_FALSE(device.routes(0x1001));
}

// PID_ERROR_CODE (03/05/01 §4.2.28) on the Router Object: the CRC error of a
// download whose MCB does not match, read as the property, and kept over a
// restart together with the Error state (Table 94: Error stays Error).
void test_error_code_is_readable_and_survives_restart(void)
{
    const auto readErrorCode = [](BusAccessUnit& bau) {
        std::array<uint8_t, 4> out{};
        const auto read = bau.readProperty(InterfaceObjectType::router(), InterfaceObjectInstance(1),
                                           static_cast<application::PropertyID>(28), 1, 1, out);
        TEST_ASSERT_TRUE(read.isOk());
        TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(read.value()));
        return out[0];
    };
    {
        CouplerDevice device;
        TEST_ASSERT_EQUAL_UINT8(0u, readErrorCode(*device.bau));
        device.loadEvent({0x01});
        const std::array<uint8_t, 8> mcb{0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x12, 0x34};
        TEST_ASSERT_TRUE(device.load()->writeMcb(mcb).isOk());
        device.loadEvent({0x02});
        TEST_ASSERT_EQUAL(static_cast<int>(network::FilterTableLoadControl::State::Error),
                          static_cast<int>(device.load()->state()));
        TEST_ASSERT_EQUAL_UINT8(network::FilterTableLoadControl::kErrorCrc, readErrorCode(*device.bau));
        device.bau->flushPendingPersistence();
    }
    CouplerDevice device;
    TEST_ASSERT_EQUAL(static_cast<int>(network::FilterTableLoadControl::State::Error),
                      static_cast<int>(device.load()->state()));
    TEST_ASSERT_EQUAL_UINT8(network::FilterTableLoadControl::kErrorCrc, readErrorCode(*device.bau));
    TEST_ASSERT_FALSE(device.coupler().policy().isFilterTableLoaded());
    device.loadEvent({0x04});
    TEST_ASSERT_EQUAL_UINT8(0u, readErrorCode(*device.bau));
}

// A coupler declared once the device is running (configureRouterRole() after
// init()) gets its Router Object after init()'s restore has run; the coupler
// configuration stored for it is restored then, and reaches the policy.
void test_routing_configuration_survives_restart_when_declared_at_run_time(void)
{
    {
        CouplerDevice device;
        const std::array<uint8_t, 1> physLock{0x76};
        const std::array<uint8_t, 1> notUsed{0x00};
        TEST_ASSERT_TRUE(device.bau->writeProperty(InterfaceObjectType::router(), InterfaceObjectInstance(1),
                                                   static_cast<application::PropertyID>(52), 1, physLock).isOk());
        TEST_ASSERT_TRUE(device.bau->writeProperty(InterfaceObjectType::router(), InterfaceObjectInstance(1),
                                                   static_cast<application::PropertyID>(67), 1, notUsed).isOk());
        device.bau->flushPendingPersistence();
    }
    CouplerDevice device;
    TEST_ASSERT_EQUAL(static_cast<int>(network::PhysFrameHandling::Lock),
                      static_cast<int>(device.coupler().policy().lineConfig(network::CouplerPort::Primary).physFrame));
    TEST_ASSERT_FALSE(device.coupler().policy().isFilterTableInUse());
    // PID_FILTER_TABLE_USE = 0: the evaluation is negative for every group
    // address (§4.5.13.3), so a loaded table that passes 0801h does not route it.
    device.download(tableWith(0x0800, 0x08FF));
    TEST_ASSERT_FALSE(device.routes(0x0801));
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
    RUN_TEST(test_filter_table_load_state_drives_the_routing_policy);
    RUN_TEST(test_a_new_coupler_comes_up_loaded_and_cleared);
    RUN_TEST(test_downloaded_filter_table_survives_restart_and_routes);
    RUN_TEST(test_a_download_is_saved_by_the_coalesced_flush);
    RUN_TEST(test_power_loss_during_save_keeps_the_previous_table);
    RUN_TEST(test_failed_save_keeps_the_previous_table);
    RUN_TEST(test_error_code_is_readable_and_survives_restart);
    RUN_TEST(test_routing_configuration_survives_restart_when_declared_at_run_time);
    return UNITY_END();
}
