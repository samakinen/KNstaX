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
#include "knx/objects/generic_interface_object.hpp"
#include <algorithm>
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
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                  mgr.writeProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(DeviceProperty::ProgMode), 1, progMode).error());

        std::vector<uint8_t> subnet = {0x12};
        std::vector<uint8_t> device = {0x34};
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
            mgr.writeProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                static_cast<knx::application::PropertyID>(DeviceProperty::SubnetAddress), 1, subnet).error());
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
            mgr.writeProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                static_cast<knx::application::PropertyID>(DeviceProperty::DeviceAddress), 1, device).error());

        // Address table: 2 entries at indices 1..2
        std::vector<uint8_t> addrTable = {
            0x00, 0x11,
            0x00, 0x22,
        };
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                  mgr.writeProperty(InterfaceObjectType::addressTable(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(AddressTableProperty::TableData), 1, addrTable).error());

        // Association table: one entry at index 1
        std::vector<uint8_t> assocTable = {
            0x00, 0x01,
            0x12, 0x34,
        };
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                  mgr.writeProperty(InterfaceObjectType::associationTable(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(AssociationTableProperty::TableData), 1, assocTable).error());

        // Application Program. PID_PROGRAM_VERSION is PDT_GENERIC_05:
        // manufacturer (2 B) + application number (2 B) + version (1 B).
        std::vector<uint8_t> progVersion = {0x00, 0xFA, 0x00, 0x01, 0x42};
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                  mgr.writeProperty(InterfaceObjectType::applicationProgram(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(AppProgramProperty::ProgramVersion), 1, progVersion).error());

        std::vector<uint8_t> appId = {0xBE, 0xEF};
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                  mgr.writeProperty(InterfaceObjectType::applicationProgram(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(AppProgramProperty::ApplicationId), 1, appId).error());

#if KNX_SECURE_ENABLED
        // Security
        std::vector<uint8_t> secMode = {static_cast<uint8_t>(SecurityMode::Enabled)};
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                  mgr.writeProperty(InterfaceObjectType::security(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(SecurityProperty::SecurityMode), 1, secMode).error());

        std::vector<uint8_t> toolKey(16, 0xAA);
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                  mgr.writeProperty(InterfaceObjectType::security(), InterfaceObjectInstance(1),
                                            static_cast<knx::application::PropertyID>(SecurityProperty::ToolKey), 1, toolKey).error());
#endif

        const size_t saved = mgr.saveToPersistence().value();
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
        // Verify that each expected property was persisted using the compact key
        // format (dXXXX). A save writes the bank the marker does not name, so
        // the first save from an empty namespace lands in bank 1 (bit 15 set).
        TEST_ASSERT_TRUE(has(makeCompactKey(InterfaceObjectManager::kPersistenceBankMarkerKeyId)));
        auto hasProperty = [&](InterfaceObjectType objType, auto pid) {
            const auto keyId = makePersistenceKeyId(objType, static_cast<application::PropertyID>(pid));
            return has(makeCompactKey(keyId | InterfaceObjectManager::kPersistenceBank1KeyBit))
                && !has(makeCompactKey(keyId));
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
#if KNX_SECURE_ENABLED
        TEST_ASSERT_TRUE(hasProperty(InterfaceObjectType::security(), SecurityProperty::SecurityMode));
#endif

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
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                    mgr.readProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                        static_cast<knx::application::PropertyID>(DeviceProperty::SubnetAddress), 1, 1, out).error());
        TEST_ASSERT_EQUAL_UINT32(1, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x12, out[0]);

        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                    mgr.readProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                        static_cast<knx::application::PropertyID>(DeviceProperty::DeviceAddress), 1, 1, out).error());
        TEST_ASSERT_EQUAL_UINT32(1, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x34, out[0]);

        // ProgMode must come back OFF after a reload — it is never restored
        // from persistence (KNX devices power up out of programming mode).
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                            mgr.readProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                                                                static_cast<knx::application::PropertyID>(DeviceProperty::ProgMode), 1, 1, out).error());
        TEST_ASSERT_EQUAL_UINT32(1, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);

        // Verify address table slice
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
              mgr.readProperty(InterfaceObjectType::addressTable(), InterfaceObjectInstance(1),
                           static_cast<knx::application::PropertyID>(AddressTableProperty::TableData), 1, 2, out).error());
        TEST_ASSERT_EQUAL_UINT32(4, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
        TEST_ASSERT_EQUAL_UINT8(0x11, out[1]);
        TEST_ASSERT_EQUAL_UINT8(0x00, out[2]);
        TEST_ASSERT_EQUAL_UINT8(0x22, out[3]);

        // Verify association table slice
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
              mgr.readProperty(InterfaceObjectType::associationTable(), InterfaceObjectInstance(1),
                           static_cast<knx::application::PropertyID>(AssociationTableProperty::TableData), 1, 1, out).error());
        TEST_ASSERT_EQUAL_UINT32(4, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
        TEST_ASSERT_EQUAL_UINT8(0x01, out[1]);
        TEST_ASSERT_EQUAL_UINT8(0x12, out[2]);
        TEST_ASSERT_EQUAL_UINT8(0x34, out[3]);

        // Verify app program (5-byte PDT_GENERIC_05 version block)
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
              mgr.readProperty(InterfaceObjectType::applicationProgram(), InterfaceObjectInstance(1),
                           static_cast<knx::application::PropertyID>(AppProgramProperty::ProgramVersion), 1, 1, out).error());
        TEST_ASSERT_EQUAL_UINT32(5, out.size());
        TEST_ASSERT_EQUAL_UINT8(0x00, out[0]);
        TEST_ASSERT_EQUAL_UINT8(0xFA, out[1]);
        TEST_ASSERT_EQUAL_UINT8(0x42, out[4]);

        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
              mgr.readProperty(InterfaceObjectType::applicationProgram(), InterfaceObjectInstance(1),
                           static_cast<knx::application::PropertyID>(AppProgramProperty::ApplicationId), 1, 1, out).error());
        TEST_ASSERT_EQUAL_UINT32(2, out.size());
        TEST_ASSERT_EQUAL_UINT8(0xBE, out[0]);
        TEST_ASSERT_EQUAL_UINT8(0xEF, out[1]);

#if KNX_SECURE_ENABLED
        // Verify security
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                  mgr.readProperty(InterfaceObjectType::security(), InterfaceObjectInstance(1),
                                           static_cast<knx::application::PropertyID>(SecurityProperty::SecurityMode), 1, 1, out).error());
        TEST_ASSERT_EQUAL_UINT32(1, out.size());
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SecurityMode::Enabled), out[0]);

        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                  mgr.readProperty(InterfaceObjectType::security(), InterfaceObjectInstance(1),
                                           static_cast<knx::application::PropertyID>(SecurityProperty::ToolKey), 1, 1, out).error());
        TEST_ASSERT_EQUAL_UINT32(16, out.size());
        for (uint8_t b : out) {
            TEST_ASSERT_EQUAL_UINT8(0xAA, b);
        }
#endif
    }
}

// ---------------------------------------------------------------------------
// Atomic download: banks and the marker (kPersistenceBankMarkerKeyId)
// ---------------------------------------------------------------------------

static const auto kSubnetPid = static_cast<application::PropertyID>(DeviceProperty::SubnetAddress);
static const auto kAddrTablePid = static_cast<application::PropertyID>(AddressTableProperty::TableData);

static uint16_t bankKey(InterfaceObjectType type, application::PropertyID pid, uint8_t bank) {
    const uint16_t keyId = makePersistenceKeyId(type, pid);
    return bank == 0 ? keyId : static_cast<uint16_t>(keyId | InterfaceObjectManager::kPersistenceBank1KeyBit);
}

static uint8_t readSubnet(InterfaceObjectManager& mgr) {
    knx::application::PropertyServiceDataBuffer out;
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                      mgr.readProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1), kSubnetPid, 1, 1, out).error());
    TEST_ASSERT_EQUAL_UINT32(1, out.size());
    return out[0];
}

static uint16_t readFirstGroupAddress(InterfaceObjectManager& mgr) {
    knx::application::PropertyServiceDataBuffer out;
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                      mgr.readProperty(InterfaceObjectType::addressTable(), InterfaceObjectInstance(1),
                                       kAddrTablePid, 1, 1, out).error());
    TEST_ASSERT_EQUAL_UINT32(2, out.size());
    return static_cast<uint16_t>((out[0] << 8) | out[1]);
}

static bool rawExists(ObjectPersistence& p, uint16_t keyId) {
    auto res = p.existsById(keyId);
    return res.isOk() && res.value();
}

/// A device in the field holds its download in the layout from before banks:
/// plain keys, no marker. It must load unchanged, and the next save must move
/// it to bank 1 and leave no plain keys behind.
void test_Persistence_LegacyUnmarkedLayoutIsLoadedAndMigrated(void) {
    clearPersistenceNamespace();
    {
        auto p = createPersistence();
        TEST_ASSERT_TRUE(p->init("knx_objects"));
        const std::vector<uint8_t> schema = {0x00, 0x01};  // init()'s default schema version
        TEST_ASSERT_EQUAL(util::ErrorCode::Success,
                          p->saveById(InterfaceObjectManager::kPersistenceSchemaVersionKeyId, schema).error());
        const std::vector<uint8_t> subnet = {0x12};
        const std::vector<uint8_t> table = {0x0A, 0x01};
        TEST_ASSERT_EQUAL(util::ErrorCode::Success,
                          p->saveById(makePersistenceKeyId(InterfaceObjectType::device(), kSubnetPid), subnet).error());
        TEST_ASSERT_EQUAL(util::ErrorCode::Success,
                          p->saveById(makePersistenceKeyId(InterfaceObjectType::addressTable(), kAddrTablePid), table).error());
        p->close();
    }

    {
        InterfaceObjectManager mgr;
        TEST_ASSERT_TRUE(mgr.init(true).isOk());
        TEST_ASSERT_EQUAL_UINT8(0, mgr.activePersistenceBank());
        TEST_ASSERT_TRUE(mgr.loadFromPersistence() >= 2u);
        TEST_ASSERT_EQUAL_UINT8(0x12, readSubnet(mgr));
        TEST_ASSERT_EQUAL_HEX16(0x0A01, readFirstGroupAddress(mgr));

        TEST_ASSERT_TRUE(mgr.saveToPersistence().value() > 0u);
        TEST_ASSERT_EQUAL_UINT8(1, mgr.activePersistenceBank());
    }

    {
        auto p = createPersistence();
        TEST_ASSERT_TRUE(p->init("knx_objects"));
        std::vector<uint8_t> marker;
        TEST_ASSERT_EQUAL(util::ErrorCode::Success,
                          p->loadById(InterfaceObjectManager::kPersistenceBankMarkerKeyId, marker).error());
        TEST_ASSERT_EQUAL_UINT32(1, marker.size());
        TEST_ASSERT_EQUAL_UINT8(1, marker[0]);
        TEST_ASSERT_FALSE(rawExists(*p, bankKey(InterfaceObjectType::device(), kSubnetPid, 0)));
        TEST_ASSERT_FALSE(rawExists(*p, bankKey(InterfaceObjectType::addressTable(), kAddrTablePid, 0)));
        TEST_ASSERT_TRUE(rawExists(*p, bankKey(InterfaceObjectType::device(), kSubnetPid, 1)));
        TEST_ASSERT_TRUE(rawExists(*p, bankKey(InterfaceObjectType::addressTable(), kAddrTablePid, 1)));
        p->close();
    }

    InterfaceObjectManager mgr;
    TEST_ASSERT_TRUE(mgr.init(true).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, mgr.activePersistenceBank());
    TEST_ASSERT_TRUE(mgr.loadFromPersistence() >= 2u);
    TEST_ASSERT_EQUAL_UINT8(0x12, readSubnet(mgr));
    TEST_ASSERT_EQUAL_HEX16(0x0A01, readFirstGroupAddress(mgr));
}

/// A reset part-way through saving a new download (some properties written,
/// the marker not yet switched) must boot with the previous download whole,
/// not a mixture, and clear the half-written bank.
void test_Persistence_InterruptedSaveKeepsPreviousDownload(void) {
    clearPersistenceNamespace();
    uint8_t savedBank = 0;
    {
        InterfaceObjectManager mgr;
        TEST_ASSERT_TRUE(mgr.init(true).isOk());
        const std::vector<uint8_t> subnet = {0x12};
        const std::vector<uint8_t> table = {0x0A, 0x01};
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                          mgr.writeProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1), kSubnetPid, 1, subnet).error());
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                          mgr.writeProperty(InterfaceObjectType::addressTable(), InterfaceObjectInstance(1),
                                            kAddrTablePid, 1, table).error());
        TEST_ASSERT_TRUE(mgr.saveToPersistence().value() > 0u);
        savedBank = mgr.activePersistenceBank();
    }

    // What the interrupted save of the next download left: its first
    // properties in the other bank, and no marker switch.
    const uint8_t otherBank = static_cast<uint8_t>(savedBank ^ 1u);
    {
        auto p = createPersistence();
        TEST_ASSERT_TRUE(p->init("knx_objects"));
        const std::vector<uint8_t> newSubnet = {0x77};
        const std::vector<uint8_t> newTable = {0x0B, 0x02};
        TEST_ASSERT_EQUAL(util::ErrorCode::Success,
                          p->saveById(bankKey(InterfaceObjectType::device(), kSubnetPid, otherBank), newSubnet).error());
        TEST_ASSERT_EQUAL(util::ErrorCode::Success,
                          p->saveById(bankKey(InterfaceObjectType::addressTable(), kAddrTablePid, otherBank), newTable).error());
        p->close();
    }

    {
        InterfaceObjectManager mgr;
        TEST_ASSERT_TRUE(mgr.init(true).isOk());
        TEST_ASSERT_EQUAL_UINT8(savedBank, mgr.activePersistenceBank());
        TEST_ASSERT_TRUE(mgr.loadFromPersistence() > 0u);
        TEST_ASSERT_EQUAL_UINT8(0x12, readSubnet(mgr));
        TEST_ASSERT_EQUAL_HEX16(0x0A01, readFirstGroupAddress(mgr));
    }

    auto p = createPersistence();
    TEST_ASSERT_TRUE(p->init("knx_objects"));
    TEST_ASSERT_FALSE(rawExists(*p, bankKey(InterfaceObjectType::device(), kSubnetPid, otherBank)));
    TEST_ASSERT_FALSE(rawExists(*p, bankKey(InterfaceObjectType::addressTable(), kAddrTablePid, otherBank)));
    p->close();
}

/// The Secure Routing backbone key ETS downloads into the KNXnet/IP Parameter
/// Object must survive the restart that ends the download.
void test_Persistence_KnxNetIpBackboneKeySurvivesRestart(void) {
    clearPersistenceNamespace();
    const auto kBackboneKeyPid = static_cast<application::PropertyID>(91);  // PID_BACKBONE_KEY
    const std::vector<uint8_t> key = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                      0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    {
        InterfaceObjectManager mgr;
        GenericInterfaceObject ipObject(InterfaceObjectType::knxNetIpParameter());
        TEST_ASSERT_TRUE(mgr.registerReferenceObject(ipObject).isOk());
        TEST_ASSERT_TRUE(mgr.init(true).isOk());
        TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                          mgr.writeProperty(InterfaceObjectType::knxNetIpParameter(), InterfaceObjectInstance(1),
                                            kBackboneKeyPid, 1, key).error());
        TEST_ASSERT_TRUE(mgr.saveToPersistence().value() > 0u);
    }

    InterfaceObjectManager mgr;
    GenericInterfaceObject ipObject(InterfaceObjectType::knxNetIpParameter());
    TEST_ASSERT_TRUE(mgr.registerReferenceObject(ipObject).isOk());
    TEST_ASSERT_TRUE(mgr.init(true).isOk());
    TEST_ASSERT_TRUE(mgr.loadFromPersistence() > 0u);
    knx::application::PropertyServiceDataBuffer out;
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::Success,
                      mgr.readProperty(InterfaceObjectType::knxNetIpParameter(), InterfaceObjectInstance(1),
                                       kBackboneKeyPid, 1, 1, out).error());
    TEST_ASSERT_EQUAL_UINT32(key.size(), out.size());
    for (size_t i = 0; i < key.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(key[i], out[i]);
    }
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    UNITY_BEGIN();
    RUN_TEST(test_InterfaceObjectManager_Persistence_SaveLoadRoundtrip);
    RUN_TEST(test_Persistence_LegacyUnmarkedLayoutIsLoadedAndMigrated);
    RUN_TEST(test_Persistence_InterruptedSaveKeepsPreviousDownload);
    RUN_TEST(test_Persistence_KnxNetIpBackboneKeySurvivesRestart);
    return UNITY_END();
}
