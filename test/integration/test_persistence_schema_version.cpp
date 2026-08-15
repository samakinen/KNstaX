// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_persistence_schema_version.cpp
 * @brief Persisted commissioned state must not survive a layout change.
 *
 * Persisted state is a positional byte layout: the parameter block is decoded
 * in declaration order and interface-object blobs are written back into
 * properties without a length negotiation. A firmware update that inserts,
 * removes or reorders anything therefore cannot safely read what the previous
 * firmware wrote — the bytes land in the wrong fields and the device comes up
 * mis-parameterised with nothing logged and nothing failing.
 *
 * `PersistencePolicy::schemaVersion` exists to prevent exactly that. It was
 * declared and documented but never stored or compared, which is why products
 * ended up carrying their own "Parameter Layout Version (do not change)" ETS
 * parameter as a manual workaround. These tests pin the guard shut.
 */

#include "unity.h"

#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/object_persistence.hpp"

#include <array>
#include <cstdio>
#include <memory>
#include <vector>

using namespace knx;
using namespace knx::objects;

namespace {

constexpr const char* kNamespace = "knx_objects";

/// A device property ETS genuinely writes, so persisting it exercises the same
/// path a real commissioning does.
constexpr auto kProgModeProperty = static_cast<application::PropertyID>(DeviceProperty::ProgMode);

void clearStore() {
    auto persistence = createPersistence();
    TEST_ASSERT_TRUE(persistence->init(kNamespace));
    (void)persistence->eraseAll();
    persistence->close();
}

/// Boot on `schemaVersion`, write a device property, and persist it.
void bootAndPersist(uint16_t schemaVersion, uint8_t markerValue) {
    InterfaceObjectManager manager;
    TEST_ASSERT_TRUE(manager.init(true, kNamespace, schemaVersion).isOk());

    const std::vector<uint8_t> value{markerValue};
    TEST_ASSERT_EQUAL(static_cast<int>(PropertyAccessResult::Success),
                      static_cast<int>(manager.writeProperty(InterfaceObjectType::device(),
                                                             InterfaceObjectInstance(1),
                                                             kProgModeProperty, 1, value)));
    TEST_ASSERT_TRUE(manager.saveToPersistence() > 0u);
}

/// Boot on `schemaVersion` and report how many properties were restored.
size_t bootAndLoad(uint16_t schemaVersion) {
    InterfaceObjectManager manager;
    TEST_ASSERT_TRUE(manager.init(true, kNamespace, schemaVersion).isOk());
    return manager.loadFromPersistence();
}

uint16_t storedSchemaVersion() {
    auto persistence = createPersistence();
    TEST_ASSERT_TRUE(persistence->init(kNamespace));
    std::vector<uint8_t> marker;
    const auto res =
        persistence->loadById(InterfaceObjectManager::kPersistenceSchemaVersionKeyId, marker);
    persistence->close();
    TEST_ASSERT_EQUAL(static_cast<int>(PersistenceResult::Success), static_cast<int>(res));
    TEST_ASSERT_EQUAL(2u, marker.size());
    return static_cast<uint16_t>((static_cast<uint16_t>(marker[0]) << 8) | marker[1]);
}

} // namespace

void setUp(void) { clearStore(); }
void tearDown(void) { clearStore(); }

void test_state_survives_a_reboot_on_the_same_schema(void) {
    // The guard must not be so eager that it throws away good state: an
    // ordinary power cycle has to keep the commissioning.
    bootAndPersist(7, 0x01);
    TEST_ASSERT_TRUE(bootAndLoad(7) > 0u);
}

void test_state_is_discarded_when_the_schema_changes(void) {
    // The case this exists for: a firmware update reorders the layout, so the
    // stored bytes no longer mean what they used to.
    bootAndPersist(7, 0x01);
    TEST_ASSERT_EQUAL(0u, bootAndLoad(8));
}

void test_downgrade_also_discards(void) {
    // Rolling back is exactly as unsafe as rolling forward — the layouts differ
    // either way, so "older" does not imply "compatible".
    bootAndPersist(9, 0x01);
    TEST_ASSERT_EQUAL(0u, bootAndLoad(4));
}

void test_new_version_is_recorded_so_the_discard_happens_once(void) {
    // After a discard the new version must be stored, otherwise every
    // subsequent boot would wipe freshly downloaded commissioning and the
    // device could never stay commissioned at all.
    bootAndPersist(1, 0x01);
    TEST_ASSERT_EQUAL(0u, bootAndLoad(2));   // discards v1 state, records v2
    TEST_ASSERT_EQUAL(2u, storedSchemaVersion());

    bootAndPersist(2, 0x01);                 // ETS re-downloads
    TEST_ASSERT_TRUE(bootAndLoad(2) > 0u);   // and it now sticks
    TEST_ASSERT_TRUE(bootAndLoad(2) > 0u);
}

void test_unversioned_legacy_state_is_discarded(void) {
    // State written before versioning existed carries no marker. It cannot be
    // trusted for the same reason a mismatched version cannot, so the absence
    // of a marker alongside existing keys must also trigger a discard.
    {
        auto persistence = createPersistence();
        TEST_ASSERT_TRUE(persistence->init(kNamespace));
        const std::array<uint8_t, 4> legacyBlob{0xDE, 0xAD, 0xBE, 0xEF};
        TEST_ASSERT_EQUAL(static_cast<int>(PersistenceResult::Success),
                          static_cast<int>(persistence->saveById(0x0102, legacyBlob)));
        persistence->close();
    }

    {
        InterfaceObjectManager manager;
        TEST_ASSERT_TRUE(manager.init(true, kNamespace, 3).isOk());
    }

    auto check = createPersistence();
    TEST_ASSERT_TRUE(check->init(kNamespace));
    std::vector<uint8_t> data;
    TEST_ASSERT_EQUAL(static_cast<int>(PersistenceResult::NotFound),
                      static_cast<int>(check->loadById(0x0102, data)));
    check->close();

    // ...and the marker is now present, so the next boot is an ordinary one.
    TEST_ASSERT_EQUAL(3u, storedSchemaVersion());
}

void test_first_boot_records_the_version_without_complaining(void) {
    // An empty namespace is not a layout change; nothing should be logged as
    // discarded, and the marker should simply be laid down.
    {
        InterfaceObjectManager manager;
        TEST_ASSERT_TRUE(manager.init(true, kNamespace, 5).isOk());
    }
    TEST_ASSERT_EQUAL(5u, storedSchemaVersion());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_state_survives_a_reboot_on_the_same_schema);
    RUN_TEST(test_state_is_discarded_when_the_schema_changes);
    RUN_TEST(test_downgrade_also_discards);
    RUN_TEST(test_new_version_is_recorded_so_the_discard_happens_once);
    RUN_TEST(test_unversioned_legacy_state_is_discarded);
    RUN_TEST(test_first_boot_records_the_version_without_complaining);
    return UNITY_END();
}
