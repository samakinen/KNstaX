// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_object_property_compliance.cpp
 * @brief Unit tests for object property registration compliance validation
 */

#include "knx/objects/object_property_compliance.hpp"
#include "knx/objects/address_table_object.hpp"
#include "knx/objects/application_program_object.hpp"
#include "knx/objects/association_table_object.hpp"
#include "knx/objects/device_object.hpp"
#include "knx/objects/group_object_table_object.hpp"
#include "knx/objects/security_interface_object.hpp"

#include "unity.h"

using namespace knx;
using namespace knx::objects;

void setUp(void) {}
void tearDown(void) {}

void test_ObjectPropertyCompliance_CoreSchemasAccepted(void) {
    {
        DeviceObject obj;
        size_t n = obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>{});
        std::vector<PropertyRegistrationInfo> regs(n);
        obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>(regs));
        auto ok = validateObjectPropertyRegistrations(obj.objectType(), regs);
        TEST_ASSERT_TRUE(ok.isOk());
    }
    {
        AddressTableObject obj;
        size_t n = obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>{});
        std::vector<PropertyRegistrationInfo> regs(n);
        obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>(regs));
        auto ok = validateObjectPropertyRegistrations(obj.objectType(), regs);
        TEST_ASSERT_TRUE(ok.isOk());
    }
    {
        AssociationTableObject obj;
        size_t n = obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>{});
        std::vector<PropertyRegistrationInfo> regs(n);
        obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>(regs));
        auto ok = validateObjectPropertyRegistrations(obj.objectType(), regs);
        TEST_ASSERT_TRUE(ok.isOk());
    }
    {
        ApplicationProgramObject obj;
        size_t n = obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>{});
        std::vector<PropertyRegistrationInfo> regs(n);
        obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>(regs));
        auto ok = validateObjectPropertyRegistrations(obj.objectType(), regs);
        TEST_ASSERT_TRUE(ok.isOk());
    }
    {
        GroupObjectTableObject obj;
        size_t n = obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>{});
        std::vector<PropertyRegistrationInfo> regs(n);
        obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>(regs));
        auto ok = validateObjectPropertyRegistrations(obj.objectType(), regs);
        TEST_ASSERT_TRUE(ok.isOk());
    }
    {
        SecurityInterfaceObject obj;
        size_t n = obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>{});
        std::vector<PropertyRegistrationInfo> regs(n);
        obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>(regs));
        auto ok = validateObjectPropertyRegistrations(obj.objectType(), regs);
        TEST_ASSERT_TRUE(ok.isOk());
    }
}

void test_ObjectPropertyCompliance_StrictRejectsCoreMismatch(void) {
    DeviceObject obj;
    size_t n = obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>{});
    std::vector<PropertyRegistrationInfo> regs(n);
    obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>(regs));
    TEST_ASSERT_TRUE(regs.size() > 1);

    regs[1].maxElements = 2;  // Strict schema mismatch for a scalar property

    auto bad = validateObjectPropertyRegistrations(obj.objectType(), regs);
    TEST_ASSERT_TRUE(bad.isError());
}

void test_ObjectPropertyCompliance_StrictRejectsMissingExpectedProperty(void) {
    SecurityInterfaceObject obj;
    size_t n = obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>{});
    std::vector<PropertyRegistrationInfo> regs(n);
    obj.getPropertyRegistrations(std::span<PropertyRegistrationInfo>(regs));
    TEST_ASSERT_TRUE(regs.size() > 2);

    regs.pop_back();
    auto bad = validateObjectPropertyRegistrations(obj.objectType(), regs);
    TEST_ASSERT_TRUE(bad.isError());
}

void test_ObjectPropertyCompliance_UnknownTypeAllowsStructuralSchema(void) {
    std::vector<PropertyRegistrationInfo> regs = {
        {
            application::PropertyID::ObjectType,
            application::PropertyDataType::UnsignedInt,
            application::PropertyAccess::ReadOnly,
            1,
            0,
            0,
        },
        {
            static_cast<application::PropertyID>(0xE0),
            application::PropertyDataType::GenericData,
            application::PropertyAccess::ReadWrite,
            4,
            0,
            0,
        },
    };

    auto ok = validateObjectPropertyRegistrations(InterfaceObjectType(1000), regs);
    TEST_ASSERT_TRUE(ok.isOk());
}

void test_ObjectPropertyCompliance_StructuralRejectsInvalidSchemas(void) {
    std::vector<PropertyRegistrationInfo> duplicatePid = {
        {
            application::PropertyID::ObjectType,
            application::PropertyDataType::UnsignedInt,
            application::PropertyAccess::ReadOnly,
            1,
            0,
            0,
        },
        {
            application::PropertyID::ObjectType,
            application::PropertyDataType::UnsignedInt,
            application::PropertyAccess::ReadOnly,
            1,
            0,
            0,
        },
    };

    auto badDuplicate = validateObjectPropertyRegistrations(InterfaceObjectType(1001), duplicatePid);
    TEST_ASSERT_TRUE(badDuplicate.isError());

    std::vector<PropertyRegistrationInfo> zeroMaxElements = {
        {
            application::PropertyID::ObjectType,
            application::PropertyDataType::UnsignedInt,
            application::PropertyAccess::ReadOnly,
            1,
            0,
            0,
        },
        {
            static_cast<application::PropertyID>(0xE1),
            application::PropertyDataType::GenericData,
            application::PropertyAccess::ReadWrite,
            0,
            0,
            0,
        },
    };

    auto badZeroMax = validateObjectPropertyRegistrations(InterfaceObjectType(1002), zeroMaxElements);
    TEST_ASSERT_TRUE(badZeroMax.isError());
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();

    RUN_TEST(test_ObjectPropertyCompliance_CoreSchemasAccepted);
    RUN_TEST(test_ObjectPropertyCompliance_StrictRejectsCoreMismatch);
    RUN_TEST(test_ObjectPropertyCompliance_StrictRejectsMissingExpectedProperty);
    RUN_TEST(test_ObjectPropertyCompliance_UnknownTypeAllowsStructuralSchema);
    RUN_TEST(test_ObjectPropertyCompliance_StructuralRejectsInvalidSchemas);

    return UNITY_END();
}
