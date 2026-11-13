// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_interface_object_manager_instances.cpp
 * @brief Unit tests for InterfaceObjectManager object and instance addressing
 *
 * Every interface object is a singleton: instance 1, which instance 0
 * ("unspecified") also names. Reference objects are registered at most once per
 * type, and the manager has room for one of each.
 */

#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/reference_object_registry.hpp"

#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

using namespace knx;
using namespace knx::objects;

namespace {
constexpr auto kPidObjectType = application::PropertyID::ObjectType;
constexpr auto kPidBackboneKey = static_cast<application::PropertyID>(91);  // PID_BACKBONE_KEY
} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_InterfaceObjectManager_SingletonInstanceAddressing(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    const auto addressTable = InterfaceObjectType::addressTable();
    TEST_ASSERT_TRUE(mgr.hasObjectType(addressTable));
    TEST_ASSERT_EQUAL_UINT8(1, mgr.getObjectCount(addressTable));

    // Instance 0 and instance 1 both reach the one object.
    for (const uint8_t instance : {0, 1}) {
        application::PropertyServiceDataBuffer out;
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                          mgr.readProperty(addressTable, InterfaceObjectInstance(instance), kPidObjectType, 1, 1, out).error());
        TEST_ASSERT_EQUAL_UINT32(2, out.size());
        TEST_ASSERT_EQUAL_HEX8(0x00, out[0]);
        TEST_ASSERT_EQUAL_HEX8(0x01, out[1]);

        PropertyDescription description;
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                          mgr.describeProperty(addressTable, InterfaceObjectInstance(instance), kPidObjectType, 0,
                                               description).error());
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(kPidObjectType), static_cast<uint8_t>(description.id));
    }

    // There is no second instance, and an unknown type has none at all.
    application::PropertyServiceDataBuffer out;
    PropertyDescription description;
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::InvalidAddress,
                      mgr.readProperty(addressTable, InterfaceObjectInstance(2), kPidObjectType, 1, 1, out).error());
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::InvalidAddress,
                      mgr.writeProperty(addressTable, InterfaceObjectInstance(2), kPidObjectType, 1, {0x00, 0x01}).error());
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::InvalidAddress,
                      mgr.describeProperty(addressTable, InterfaceObjectInstance(2), kPidObjectType, 0, description).error());

    const auto unknown = InterfaceObjectType(200);
    TEST_ASSERT_FALSE(mgr.hasObjectType(unknown));
    TEST_ASSERT_EQUAL_UINT8(0, mgr.getObjectCount(unknown));
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::InvalidAddress,
                      mgr.readProperty(unknown, InterfaceObjectInstance(1), kPidObjectType, 1, 1, out).error());
}

void test_InterfaceObjectManager_ReferenceObjectsOnePerTypeAndAllFit(void) {
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    const auto knxNetIp = InterfaceObjectType::knxNetIpParameter();
    GenericInterfaceObject first(knxNetIp);
    TEST_ASSERT_TRUE(mgr.registerReferenceObject(first).isOk());

    const std::vector<uint8_t> key = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                      0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                      mgr.writeProperty(knxNetIp, InterfaceObjectInstance(1), kPidBackboneKey, 1, key).error());

    // A second object of the same type replaces the first rather than becoming
    // instance 2.
    GenericInterfaceObject second(knxNetIp);
    TEST_ASSERT_TRUE(mgr.registerReferenceObject(second).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, mgr.getObjectCount(knxNetIp));
    application::PropertyServiceDataBuffer out;
    (void)mgr.readProperty(knxNetIp, InterfaceObjectInstance(1), kPidBackboneKey, 1, 1, out);
    TEST_ASSERT_FALSE(out.size() == key.size() && std::equal(key.begin(), key.end(), out.begin()));

    // One object of every reference type fits beside the core objects.
    const uint16_t candidates[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21};
    std::vector<std::unique_ptr<GenericInterfaceObject>> objects;
    for (const auto raw : candidates) {
        const InterfaceObjectType type(raw);
        if (!isReferenceObjectType(type) || type == knxNetIp) {
            continue;
        }
        objects.push_back(std::make_unique<GenericInterfaceObject>(type));
        TEST_ASSERT_TRUE(mgr.registerReferenceObject(*objects.back()).isOk());
        TEST_ASSERT_TRUE(mgr.hasObjectType(type));
    }
    TEST_ASSERT_EQUAL_UINT32(kReferenceObjectTypeCount - 1u, objects.size());
    TEST_ASSERT_EQUAL_UINT32(InterfaceObjectManager::kMaxInterfaceObjects,
                             InterfaceObjectManager::kCoreObjectCount + kReferenceObjectTypeCount);
    TEST_ASSERT_TRUE(mgr.hasObjectType(InterfaceObjectType::device()));
    TEST_ASSERT_TRUE(mgr.hasObjectType(InterfaceObjectType::security()));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();

    RUN_TEST(test_InterfaceObjectManager_SingletonInstanceAddressing);
    RUN_TEST(test_InterfaceObjectManager_ReferenceObjectsOnePerTypeAndAllFit);

    return UNITY_END();
}
