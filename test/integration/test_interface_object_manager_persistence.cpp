// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_interface_object_manager_persistence.cpp
 * @brief Integration test for InterfaceObjectManager persistence
 */

#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/objects/address_table_object.hpp"
#include "knx/objects/association_table_object.hpp"
#include "knx/objects/device_object.hpp"
#include "knx/objects/application_program_object.hpp"
#include "knx/objects/security_interface_object.hpp"
#include <unity.h>
#include <cstdio>
#include <memory>
#include <vector>

using namespace knx;
using namespace knx::objects;

void setUp(void) {}
void tearDown(void) {}

/// Mirrors the encoding in interface_object_manager.cpp.
static constexpr uint16_t makePersistenceKeyId(InterfaceObjectType objectType, application::PropertyID propertyId)
{
    return (static_cast<uint16_t>(objectType.value()) << 8) | static_cast<uint8_t>(propertyId);
}

/// Mirrors the makeCompactKey() encoding in object_persistence.cpp (format: "dXXXX").
static std::string makeCompactKey(uint16_t keyId)
{
    char buf[6];
    std::snprintf(buf, sizeof(buf), "d%04X", static_cast<unsigned>(keyId));
    return std::string(buf);
}

static void clearPersistenceNamespace() {
    auto p = createPersistence();
    TEST_ASSERT_TRUE(p->init("knx_objects"));
    (void)p->eraseAll();
    p->close();
}

void test_InterfaceObjectManager_Persistence_SaveLoadRoundtrip(void) {
    clearPersistenceNamespace();

    // Configure and persist
    {
        InterfaceObjectManager mgr;
        TEST_ASSERT_TRUE(mgr.init(true).isOk());

        // Device properties
        std::vector<uint8_t> progMode = {0x01};
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                  mgr.writeProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(DeviceProperty::ProgMode), 1, progMode));

        std::vector<uint8_t> subnet = {0x12};
        std::vector<uint8_t> device = {0x34};
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
            mgr.writeProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                static_cast<knx::application::PropertyID>(DeviceProperty::SubnetAddress), 1, subnet));
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
            mgr.writeProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                static_cast<knx::application::PropertyID>(DeviceProperty::DeviceAddress), 1, device));

        // Address table: 2 entries at indices 1..2
        std::vector<uint8_t> addrTable = {
            0x00, 0x11,
            0x00, 0x22,
        };
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                  mgr.writeProperty(InterfaceObjectType::addressTable(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(AddressTableProperty::TableData), 1, addrTable));

        // Association table: one entry at index 1
        std::vector<uint8_t> assocTable = {
            0x00, 0x01,
            0x12, 0x34,
        };
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                  mgr.writeProperty(InterfaceObjectType::associationTable(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(AssociationTableProperty::TableData), 1, assocTable));

        // Application Program. PID_PROGRAM_VERSION is PDT_GENERIC_05:
        // manufacturer (2 B) + application number (2 B) + version (1 B).
        std::vector<uint8_t> progVersion = {0x00, 0xFA, 0x00, 0x01, 0x42};
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                  mgr.writeProperty(InterfaceObjectType::applicationProgram(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(AppProgramProperty::ProgramVersion), 1, progVersion));

        std::vector<uint8_t> appId = {0xBE, 0xEF};
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                  mgr.writeProperty(InterfaceObjectType::applicationProgram(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(AppProgramProperty::ApplicationId), 1, appId));

        // Security
        std::vector<uint8_t> secMode = {static_cast<uint8_t>(SecurityMode::Enabled)};
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                  mgr.writeProperty(InterfaceObjectType::security(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(SecurityProperty::SecurityMode), 1, secMode));

        std::vector<uint8_t> toolKey(16, 0xAA);
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                  mgr.writeProperty(InterfaceObjectType::security(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(SecurityProperty::ToolKey), 1, toolKey));

        const size_t saved = mgr.saveToPersistence();
        TEST_ASSERT_TRUE(saved > 0);
    }

    // Verify key enumeration lists at least the keys we wrote.
    {
        auto p = createPersistence();
        TEST_ASSERT_TRUE(p->init("knx_objects"));

        const auto keys = p->listKeys();
        TEST_ASSERT_TRUE(keys.size() > 0);

        const auto has = [&](const std::string& k) {
            return std::find(keys.begin(), keys.end(), k) != keys.end();
        };

        // Verify that each expected property was persisted using the compact key format (dXXXX).
        auto hasProperty = [&](InterfaceObjectType objType, auto pid) {
            const auto keyId = makePersistenceKeyId(objType, static_cast<application::PropertyID>(pid));
            return has(makeCompactKey(keyId));
        };

        // ProgMode is deliberately NOT persisted: KNX devices must leave
        // programming mode on power-up, so a stale entry may exist in older
        // namespaces but must never be (re)saved or restored.
        TEST_ASSERT_FALSE(hasProperty(InterfaceObjectType::device(), DeviceProperty::ProgMode));
        TEST_ASSERT_TRUE(hasProperty(InterfaceObjectType::device(), DeviceProperty::SubnetAddress));
        TEST_ASSERT_TRUE(hasProperty(InterfaceObjectType::device(), DeviceProperty::DeviceAddress));
        TEST_ASSERT_TRUE(hasProperty(InterfaceObjectType::addressTable(), AddressTableProperty::TableData));
        TEST_ASSERT_TRUE(hasProperty(InterfaceObjectType::associationTable(), AssociationTableProperty::TableData));
        TEST_ASSERT_TRUE(hasProperty(InterfaceObjectType::applicationProgram(), AppProgramProperty::ProgramVersion));
        TEST_ASSERT_TRUE(hasProperty(InterfaceObjectType::security(), SecurityProperty::SecurityMode));

        p->close();
    }

    // Reload into fresh instance
    {
        InterfaceObjectManager mgr;
        TEST_ASSERT_TRUE(mgr.init(true).isOk());
        const size_t loaded = mgr.loadFromPersistence();
        TEST_ASSERT_TRUE(loaded > 0);

        // Verify device
        knx::application::PropertyServiceDataBuffer out;
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                    mgr.readProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                        static_cast<knx::application::PropertyID>(DeviceProperty::SubnetAddress), 1, 1, out));
        TEST_ASSERT_EQUAL_UINT32(1, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x12, out[0]);

        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                    mgr.readProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                        static_cast<knx::application::PropertyID>(DeviceProperty::DeviceAddress), 1, 1, out));
        TEST_ASSERT_EQUAL_UINT32(1, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x34, out[0]);

        // ProgMode must come back OFF after a reload — it is never restored
        // from persistence (KNX devices power up out of programming mode).
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                            mgr.readProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                                                                static_cast<knx::application::PropertyID>(DeviceProperty::ProgMode), 1, 1, out));
        TEST_ASSERT_EQUAL_UINT32(1, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);

        // Verify address table slice
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
              mgr.readProperty(InterfaceObjectType::addressTable(), InterfaceObjectInstance(1),
                           static_cast<knx::application::PropertyID>(AddressTableProperty::TableData), 1, 2, out));
        TEST_ASSERT_EQUAL_UINT32(4, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
        TEST_ASSERT_EQUAL_UINT8(0x11, out[1]);
        TEST_ASSERT_EQUAL_UINT8(0x00, out[2]);
        TEST_ASSERT_EQUAL_UINT8(0x22, out[3]);

        // Verify association table slice
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
              mgr.readProperty(InterfaceObjectType::associationTable(), InterfaceObjectInstance(1),
                           static_cast<knx::application::PropertyID>(AssociationTableProperty::TableData), 1, 1, out));
        TEST_ASSERT_EQUAL_UINT32(4, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
        TEST_ASSERT_EQUAL_UINT8(0x01, out[1]);
        TEST_ASSERT_EQUAL_UINT8(0x12, out[2]);
        TEST_ASSERT_EQUAL_UINT8(0x34, out[3]);

        // Verify app program (5-byte PDT_GENERIC_05 version block)
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
              mgr.readProperty(InterfaceObjectType::applicationProgram(), InterfaceObjectInstance(1),
                           static_cast<knx::application::PropertyID>(AppProgramProperty::ProgramVersion), 1, 1, out));
        TEST_ASSERT_EQUAL_UINT32(5, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
        TEST_ASSERT_EQUAL_UINT8(0xFA, out[1]);
        TEST_ASSERT_EQUAL_UINT8(0x42, out[4]);

        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
              mgr.readProperty(InterfaceObjectType::applicationProgram(), InterfaceObjectInstance(1),
                           static_cast<knx::application::PropertyID>(AppProgramProperty::ApplicationId), 1, 1, out));
        TEST_ASSERT_EQUAL_UINT32(2, out.size());
        TEST_ASSERT_EQUAL_UINT8(0xBE, out[0]);
        TEST_ASSERT_EQUAL_UINT8(0xEF, out[1]);

        // Verify security
        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                  mgr.readProperty(InterfaceObjectType::security(), InterfaceObjectInstance(1),
                                           static_cast<knx::application::PropertyID>(SecurityProperty::SecurityMode), 1, 1, out));
        TEST_ASSERT_EQUAL_UINT32(1, out.size());
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::Enabled), out[0]);

        TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                  mgr.readProperty(InterfaceObjectType::security(), InterfaceObjectInstance(1),
                                           static_cast<knx::application::PropertyID>(SecurityProperty::ToolKey), 1, 1, out));
        TEST_ASSERT_EQUAL_UINT32(16, out.size());
        for (uint8_t b : out) {
            TEST_ASSERT_EQUAL_UINT8(0xAA, b);
        }
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_InterfaceObjectManager_Persistence_SaveLoadRoundtrip);
    return UNITY_END();
}
