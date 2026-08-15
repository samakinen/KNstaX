// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_manifest_live_coverage.cpp
 * @brief Assembly B compliance test: every Required/Optional manifest-declared
 *        property for each core object type must be accessible through a live
 *        InterfaceObjectManager without returning NotImplemented or InvalidObject.
 *
 * This test enforces the Assembly B acceptance criterion:
 *   "known-core object/property requests do not depend on generic fallthrough"
 *
 * If any manifest-declared Required property is missing from the kernel
 * implementation, this test will catch it here rather than silently returning
 * NotImplemented at runtime.
 */

#include "unity.h"

#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/objects/reference_object_registry.hpp"

#include "knx/application/property.hpp"

#include <cstdint>
#include <vector>

using namespace knx;
using namespace knx::objects;

void setUp(void) {}
void tearDown(void) {}

namespace {

// Property ID pairs: {objectType, pid} that are intentionally WriteOnly and
// therefore a read will return WriteOnly rather than success. This is still
// NOT NotImplemented, so the test handles it correctly.
//
// The compliance check is:
//   result != NotImplemented && result != InvalidObject
//
// i.e. even ReadOnly (write attempt) or WriteOnly (read attempt) count as
// "implemented", because the kernel recognized and rejected the request on
// access-control grounds rather than treating it as an unknown property.

void checkManifestLiveCoverage(InterfaceObjectType objectType)
{
    InterfaceObjectManager mgr;
    const auto initResult = mgr.init(false);
    TEST_ASSERT_TRUE(initResult.isOk());

    const auto entries = coreObjectPropertyManifestEntries(objectType);
    TEST_ASSERT_TRUE(!entries.empty());

    application::PropertyServiceDataBuffer readBuf;

    for (const auto& manifestEntry : entries) {
        if (!isSupportedProperty(manifestEntry)) {
            continue; // Unsupported/Deferred — skip live check
        }

        const auto pid = manifestEntry.registration.propertyId;

        // Attempt a read (element 1, count 1). For WriteOnly properties the
        // kernel returns WriteOnly; for ReadOnly or ReadWrite it returns
        // Success or an access/value error. The only forbidden results are
        // NotImplemented and InvalidObject.
        readBuf = {};
        const auto readResult = mgr.readProperty(
            objectType,
            InterfaceObjectInstance(1),
            pid,
            1, // startIndex (1-based)
            1, // elementCount
            readBuf);

        TEST_ASSERT_NOT_EQUAL(
            static_cast<int>(PropertyAccessResult::NotImplemented),
            static_cast<int>(readResult));

        TEST_ASSERT_NOT_EQUAL(
            static_cast<int>(PropertyAccessResult::InvalidObject),
            static_cast<int>(readResult));

        // Also verify that the manifest gate itself does not incorrectly block
        // a declared property: InvalidProperty on a declared Required property
        // would indicate a manifest/kernel mismatch in the gate logic.
        if (isRequiredProperty(manifestEntry)) {
            TEST_ASSERT_NOT_EQUAL(
                static_cast<int>(PropertyAccessResult::InvalidProperty),
                static_cast<int>(readResult));
        }
    }
}

} // namespace

void test_ManifestLiveCoverage_DeviceObject(void)
{
    checkManifestLiveCoverage(InterfaceObjectType::device());
}

void test_ManifestLiveCoverage_AddressTableObject(void)
{
    checkManifestLiveCoverage(InterfaceObjectType::addressTable());
}

void test_ManifestLiveCoverage_AssociationTableObject(void)
{
    checkManifestLiveCoverage(InterfaceObjectType::associationTable());
}

void test_ManifestLiveCoverage_ApplicationProgramObject(void)
{
    checkManifestLiveCoverage(InterfaceObjectType::applicationProgram());
}

void test_ManifestLiveCoverage_GroupObjectTableObject(void)
{
    checkManifestLiveCoverage(InterfaceObjectType::groupObjectTable());
}

void test_ManifestLiveCoverage_SecurityInterfaceObject(void)
{
    checkManifestLiveCoverage(InterfaceObjectType::security());
}

void test_ManifestGate_RejectsUndeclaredPidForCoreObject(void)
{
    // Property ID 0xEF is not declared in any core object manifest.
    // The manifest gate must return InvalidProperty, not NotImplemented.
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    const auto unknownPid = static_cast<application::PropertyID>(0xEF);
    application::PropertyServiceDataBuffer buf;

    const auto result = mgr.readProperty(
        InterfaceObjectType::device(),
        InterfaceObjectInstance(1),
        unknownPid,
        1, 1,
        buf);

    TEST_ASSERT_EQUAL(
        static_cast<int>(PropertyAccessResult::InvalidProperty),
        static_cast<int>(result));
}

void test_ManifestGate_DescribeRejectsUndeclaredPid(void)
{
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());

    const auto unknownPid = static_cast<application::PropertyID>(0xEE);

    application::PropertyDataType type{};
    uint16_t maxElements = 0;
    uint8_t access = 0;
    application::PropertyID resolvedId = static_cast<application::PropertyID>(0);
    uint8_t readLevel = 0;
    uint8_t writeLevel = 0;

    const auto result = mgr.describeProperty(
        InterfaceObjectType::device(),
        InterfaceObjectInstance(1),
        unknownPid,
        PropertyIndex(0),
        resolvedId,
        type,
        maxElements,
        access,
        readLevel,
        writeLevel);

    TEST_ASSERT_EQUAL(
        static_cast<int>(PropertyAccessResult::InvalidProperty),
        static_cast<int>(result));
}

void test_ManifestLiveCoverage_KnxNetIpParameter_SecurePids(void)
{
    // Register a GenericInterfaceObject for knxNetIpParameter so the manager
    // can route property access through it, then verify all 5 secure PIDs
    // (90-94) are accessible (i.e., not NotImplemented / InvalidObject).
    GenericInterfaceObject paramObj(InterfaceObjectType::knxNetIpParameter());
    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(false).isOk());
    mgr.registerReferenceObject(paramObj);

    const auto entries = referenceObjectPropertyManifestEntries(InterfaceObjectType::knxNetIpParameter());
    TEST_ASSERT_TRUE(!entries.empty());

    application::PropertyServiceDataBuffer readBuf;

    for (const auto& manifestEntry : entries) {
        const auto pid = manifestEntry.registration.propertyId;
        // Only check the 5 new secure PIDs (90-94)
        const uint8_t pidVal = static_cast<uint8_t>(pid);
        if (pidVal < 90 || pidVal > 94) {
            continue;
        }

        readBuf = {};
        const auto readResult = mgr.readProperty(
            InterfaceObjectType::knxNetIpParameter(),
            InterfaceObjectInstance(1),
            pid,
            1, 1,
            readBuf);

        TEST_ASSERT_NOT_EQUAL(
            static_cast<int>(PropertyAccessResult::NotImplemented),
            static_cast<int>(readResult));

        TEST_ASSERT_NOT_EQUAL(
            static_cast<int>(PropertyAccessResult::InvalidObject),
            static_cast<int>(readResult));
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    UNITY_BEGIN();

    RUN_TEST(test_ManifestLiveCoverage_DeviceObject);
    RUN_TEST(test_ManifestLiveCoverage_AddressTableObject);
    RUN_TEST(test_ManifestLiveCoverage_AssociationTableObject);
    RUN_TEST(test_ManifestLiveCoverage_ApplicationProgramObject);
    RUN_TEST(test_ManifestLiveCoverage_GroupObjectTableObject);
    RUN_TEST(test_ManifestLiveCoverage_SecurityInterfaceObject);
    RUN_TEST(test_ManifestGate_RejectsUndeclaredPidForCoreObject);
    RUN_TEST(test_ManifestGate_DescribeRejectsUndeclaredPid);
    RUN_TEST(test_ManifestLiveCoverage_KnxNetIpParameter_SecurePids);

    return UNITY_END();
}
