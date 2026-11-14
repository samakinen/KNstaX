// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_bau_preinit_configuration.cpp
 * @brief Configuration set before BusAccessUnit::init(): the documented rule
 *
 * What firmware configures before init() is the device's factory default;
 * init() keeps it, and state restored from persistence takes precedence where
 * there is any (see BusAccessUnit::init). Each test sets something before
 * init() and checks the outcome after it, on a fresh store and on one that
 * holds what an earlier start (standing in for an ETS download) left behind.
 *
 * The regression this guards: init() rebuilt the group key map from the
 * downloaded key table, so a group key set beforehand was silently wiped and
 * every secured telegram was then refused with "Unknown Key".
 */

#include "unity.h"

#include "knx/bau/bau.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/objects/security_interface_object.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/types.hpp"

#include "../mocks/mock_physical_layer.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

using namespace knx;
using knx::objects::SecurityMode;
using knx::objects::SecurityProperty;

namespace {

using Key = std::array<uint8_t, 16>;

constexpr IndividualAddress kOwnAddress(1, 1, 10);
constexpr GroupAddress kDownloadedGroup(1, 2, 3);
constexpr GroupAddress kFirmwareOnlyGroup(1, 2, 4);
constexpr IndividualAddress kPeer(1, 1, 7);

constexpr Key makeKey(uint8_t seed) {
    Key key{};
    for (size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<uint8_t>(seed + i);
    }
    return key;
}

constexpr Key kFactoryToolKey = makeKey(0x10);
constexpr Key kEtsToolKey = makeKey(0x40);
constexpr Key kFirmwareGroupKey = makeKey(0x70);
constexpr Key kDownloadedGroupKey = makeKey(0xA0);
constexpr Key kFirmwareDeviceKey = makeKey(0xC0);

void clearPersistenceStore() {
    std::error_code ec;
    (void)std::filesystem::remove_all(knx::objects::persistenceNamespaceDir("knx_objects"), ec);
}

/// One boot of the device: a fresh BAU on whatever the store holds.
struct Device {
    std::unique_ptr<platform::LinuxPlatform> platform = std::make_unique<platform::LinuxPlatform>();
    std::unique_ptr<bau::BusAccessUnit> bau;

    Device() {
        auto stackPort = knx::test::createTp1TestStackPort(*platform, std::make_unique<knx::test::MockPhysicalLayer>());
        bau = std::make_unique<bau::BusAccessUnit>(*platform, std::move(stackPort));
    }
    ~Device() {
        if (bau) {
            bau->close();
        }
    }

    objects::SecurityInterfaceObject& security() { return bau->securityObject(); }

    void init() { TEST_ASSERT_TRUE(bau->init(kOwnAddress).isOk()); }

    void writeSecurityProperty(SecurityProperty property, std::span<const uint8_t> value) {
        TEST_ASSERT_TRUE(bau->writeProperty(InterfaceObjectType::security(), InterfaceObjectInstance(1),
                                            static_cast<application::PropertyID>(property), 1, value)
                             .isOk());
    }

    void writeSendingSequence(uint64_t sequence) {
        std::array<uint8_t, 6> bytes{};
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<uint8_t>(sequence >> (8u * (bytes.size() - 1u - i)));
        }
        writeSecurityProperty(SecurityProperty::SequenceNumberSending, bytes);
    }

    bool groupKeyIs(GroupAddress address, const Key& expected) {
        Key key{};
        return security().getGroupKey(address, key).isOk() && key == expected;
    }
};

/// A device ETS commissioned: the group address at GA_Index 1 and a group key
/// table entry for it, its own Tool Key, all persisted.
void commissionWithDownloadedKeys() {
    Device device;
    device.init();
    TEST_ASSERT_TRUE(device.bau->addGroupObject(kDownloadedGroup, application::dptids::Bool,
                                                true, true, true, true).isValid());
    std::vector<uint8_t> table{0x00, 0x01};  // GA_Index 1
    table.insert(table.end(), kDownloadedGroupKey.begin(), kDownloadedGroupKey.end());
    device.writeSecurityProperty(SecurityProperty::GroupKeyTable, table);
    device.writeSecurityProperty(SecurityProperty::ToolKey, kEtsToolKey);
    device.bau->flushPendingPersistence();
}

} // namespace

void setUp(void) {
    clearPersistenceStore();
}

void tearDown(void) {
    clearPersistenceStore();
}

// Fresh store: everything set before init() is in force after it.
void test_preinit_security_configuration_is_kept_on_a_fresh_device(void) {
    Device device;
    device.security().setSecurityMode(SecurityMode::Enabled);
    device.security().setToolKey(kFactoryToolKey);
    device.security().setGroupKey(kFirmwareOnlyGroup, kFirmwareGroupKey);
    device.security().setDeviceKey(kPeer, kFirmwareDeviceKey);
    device.security().setSendingSequence(1234u);
    device.security().setToolAccessSequence(77u);
    device.init();

    TEST_ASSERT_TRUE(device.security().isSecurityEnabled());
    TEST_ASSERT_TRUE(device.security().getToolKey() == kFactoryToolKey);
    TEST_ASSERT_TRUE(device.groupKeyIs(kFirmwareOnlyGroup, kFirmwareGroupKey));
    Key deviceKey{};
    TEST_ASSERT_TRUE(device.security().getDeviceKey(kPeer, deviceKey).isOk());
    TEST_ASSERT_TRUE(deviceKey == kFirmwareDeviceKey);
    TEST_ASSERT_FALSE(device.security().downloadedGroupKeysInForce());
    TEST_ASSERT_TRUE(device.security().getSendingSequence() == 1234u);
    TEST_ASSERT_TRUE(device.security().getToolAccessSequence() == 77u);
}

// A group key set before init() and a downloaded group key table: the table is
// ETS's, so it alone is in force — for the address it names, and also for one
// it does not, where the firmware key would make security mandatory against
// the project.
void test_preinit_group_key_yields_to_a_downloaded_table(void) {
    commissionWithDownloadedKeys();

    Device device;
    device.security().setGroupKey(kDownloadedGroup, kFirmwareGroupKey);
    device.security().setGroupKey(kFirmwareOnlyGroup, kFirmwareGroupKey);
    device.init();

    TEST_ASSERT_TRUE(device.security().downloadedGroupKeysInForce());
    TEST_ASSERT_TRUE(device.groupKeyIs(kDownloadedGroup, kDownloadedGroupKey));
    Key key{};
    TEST_ASSERT_FALSE(device.security().getGroupKey(kFirmwareOnlyGroup, key).isOk());
    TEST_ASSERT_EQUAL_UINT16(1u, device.security().groupKeyCount());

    // After init() the same rule: recorded, not in force.
    device.security().setGroupKey(GroupAddress(1, 2, 5), kFirmwareGroupKey);
    TEST_ASSERT_FALSE(device.security().getGroupKey(GroupAddress(1, 2, 5), key).isOk());
}

// The firmware group key survives every rebuild of the key maps while no
// table is downloaded — the rebuild that used to wipe it runs on every ETS
// write to the address table, after init() as well as in it.
void test_firmware_group_key_survives_key_table_rebuilds(void) {
    Device device;
    device.security().setGroupKey(kFirmwareOnlyGroup, kFirmwareGroupKey);
    device.init();
    TEST_ASSERT_TRUE(device.groupKeyIs(kFirmwareOnlyGroup, kFirmwareGroupKey));

    // Any write to the Security Interface Object rebuilds the maps.
    device.writeSendingSequence(10u);
    TEST_ASSERT_TRUE(device.groupKeyIs(kFirmwareOnlyGroup, kFirmwareGroupKey));

    // A downloaded table takes over; emptying it hands back to the firmware key.
    TEST_ASSERT_TRUE(device.bau->addGroupObject(kDownloadedGroup, application::dptids::Bool,
                                                true, true, true, true).isValid());
    const uint16_t index = device.bau->addressTable().findIndex(kDownloadedGroup).value();
    std::vector<uint8_t> table{static_cast<uint8_t>(index >> 8), static_cast<uint8_t>(index & 0xFFu)};
    table.insert(table.end(), kDownloadedGroupKey.begin(), kDownloadedGroupKey.end());
    device.writeSecurityProperty(SecurityProperty::GroupKeyTable, table);
    TEST_ASSERT_TRUE(device.groupKeyIs(kDownloadedGroup, kDownloadedGroupKey));
    Key key{};
    TEST_ASSERT_FALSE(device.security().getGroupKey(kFirmwareOnlyGroup, key).isOk());

    device.security().loadControlReset();  // what ETS's Unload does to the tables
    TEST_ASSERT_TRUE(device.groupKeyIs(kFirmwareOnlyGroup, kFirmwareGroupKey));
    TEST_ASSERT_FALSE(device.security().getGroupKey(kDownloadedGroup, key).isOk());
}

// Tool Key and Security Mode set before init(): the restored values win.
void test_preinit_tool_key_and_mode_yield_to_the_stored_ones(void) {
    commissionWithDownloadedKeys();  // stores the ETS Tool Key and Security Mode off

    Device device;
    device.security().setSecurityMode(SecurityMode::Enabled);
    device.security().setToolKey(kFactoryToolKey);
    device.init();

    TEST_ASSERT_TRUE(device.security().getToolKey() == kEtsToolKey);
    TEST_ASSERT_FALSE(device.security().isSecurityEnabled());
}

// Every save stores PID_TOOL_KEY, so a device that ran without a key has
// zeros on record. Zeros are no key: the factory Tool Key stays.
void test_preinit_tool_key_survives_a_stored_all_zero_key(void) {
    {
        Device device;
        device.init();
        device.writeSendingSequence(1u);  // anything that saves the object
        device.bau->flushPendingPersistence();
    }

    Device device;
    device.security().setToolKey(kFactoryToolKey);
    device.init();
    TEST_ASSERT_TRUE(device.security().getToolKey() == kFactoryToolKey);
}

// Sequence Number Sending never moves backwards: the higher value wins.
void test_preinit_sending_sequence_keeps_the_higher_value(void) {
    {
        Device device;
        device.init();
        device.writeSendingSequence(5000u);
        device.bau->flushPendingPersistence();
    }
    {
        Device device;
        device.security().setSendingSequence(100u);
        device.init();
        TEST_ASSERT_TRUE(device.security().getSendingSequence() == 5000u);
    }
    Device device;
    device.security().setSendingSequence(9000u);
    device.init();
    TEST_ASSERT_TRUE(device.security().getSendingSequence() == 9000u);
}

// init()'s ownAddress is the factory-default address. One set on the Device
// Object beforehand is replaced (and logged), not silently mixed in.
void test_preinit_individual_address_is_replaced_by_init(void) {
    Device device;
    device.bau->deviceObject().setIndividualAddress(IndividualAddress(1, 1, 99));
    device.init();
    TEST_ASSERT_EQUAL_HEX16(kOwnAddress.raw, device.bau->deviceObject().readIndividualAddress().raw);
}

// What needs the started stack refuses before init() rather than succeeding
// into nothing.
void test_preinit_property_access_is_refused(void) {
    Device device;
    const std::array<uint8_t, 1> value{0x01};
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::NotInitialized),
                          static_cast<int>(device.bau->writeProperty(
                              InterfaceObjectType::security(), InterfaceObjectInstance(1),
                              static_cast<application::PropertyID>(SecurityProperty::SecurityMode), 1,
                              value).error()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::NotInitialized),
                          static_cast<int>(device.bau->management().setReferenceObjectProperty(
                              InterfaceObjectType::router(), static_cast<application::PropertyID>(52),
                              value).error()));
}

#if KNX_FEATURE_NETIP
// Adopting the routing group needs the restored KNXnet/IP Parameter Object.
// Before init() it used to answer with the product default and set nothing,
// and the restore then brought back a different group.
void test_preinit_adopt_routing_multicast_address_is_refused(void) {
    const IpAddress group = IpAddress::fromOctets(224, 0, 23, 12);
    Device device;
    TEST_ASSERT_TRUE(device.bau->management().adoptRoutingMulticastAddress(group, false) == IpAddress(0));
    device.init();
    TEST_ASSERT_TRUE(device.bau->management().adoptRoutingMulticastAddress(group, false) == group);
    TEST_ASSERT_TRUE(device.bau->management().routingMulticastAddress() == group);
}
#endif

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_preinit_security_configuration_is_kept_on_a_fresh_device);
    RUN_TEST(test_preinit_group_key_yields_to_a_downloaded_table);
    RUN_TEST(test_firmware_group_key_survives_key_table_rebuilds);
    RUN_TEST(test_preinit_tool_key_and_mode_yield_to_the_stored_ones);
    RUN_TEST(test_preinit_tool_key_survives_a_stored_all_zero_key);
    RUN_TEST(test_preinit_sending_sequence_keeps_the_higher_value);
    RUN_TEST(test_preinit_individual_address_is_replaced_by_init);
    RUN_TEST(test_preinit_property_access_is_refused);
#if KNX_FEATURE_NETIP
    RUN_TEST(test_preinit_adopt_routing_multicast_address_is_refused);
#endif
    return UNITY_END();
}
