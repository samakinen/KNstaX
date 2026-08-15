// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_group_object_table.cpp
 * @brief Group Object Table Object tests
 */

#include "unity.h"
#include "knx/objects/group_object_table_object.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/application/group_object.hpp"
#include <memory>

using namespace knx;
using namespace knx::objects;

static util::Result<void> readGroupTableProperty(const GroupObjectTableObject& obj,
                                                 application::PropertyID id,
                                                 uint16_t elementCount,
                                                 std::vector<uint8_t>& out) {
    const auto binding = obj.kernelBinding();
    return readProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, elementCount, out);
}

static util::Result<void> writeGroupTableProperty(GroupObjectTableObject& obj,
                                                  application::PropertyID id,
                                                  std::span<const uint8_t> value) {
    const auto binding = obj.kernelBinding();
    return writeProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, value);
}
using namespace knx::application;

void setUp(void) {
    // Set up before each test
}

void tearDown(void) {
    // Clean up after each test
}

// === Construction Tests ===

void test_GroupObjectTable_DefaultConstruction() {
    GroupObjectTableObject table;
    
    TEST_ASSERT_EQUAL_UINT16(0, table.objectCount());
    TEST_ASSERT_EQUAL_UINT16(256, table.maxObjects());
    TEST_ASSERT_TRUE(table.isValid());
}

// === Object Management Tests ===

void test_GroupObjectTable_AddObject() {
    GroupObjectTableObject table;
    
    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 3);
    config.dpt = application::dptids::Bool;
    config.flags.read = true;
    config.flags.write = true;
    config.flags.transmit = false;
    config.flags.update = true;
    
    auto obj = std::make_unique<GroupObject>(config);
    GroupObjectIndex index = table.addGroupObject(std::move(obj));
    
    TEST_ASSERT_TRUE(index.isValid());
    TEST_ASSERT_EQUAL_UINT16(0, index.value());
    TEST_ASSERT_EQUAL_UINT16(1, table.objectCount());
}

void test_GroupObjectTable_AddNullObject() {
    GroupObjectTableObject table;
    
    GroupObjectIndex index = table.addGroupObject(nullptr);
    
    TEST_ASSERT_FALSE(index.isValid());
    TEST_ASSERT_EQUAL_UINT16(0, table.objectCount());
}

void test_GroupObjectTable_AddMultipleObjects() {
    GroupObjectTableObject table;
    
    for (uint16_t i = 0; i < 10; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i);
        config.dpt = application::dptids::Bool;
        
        auto obj = std::make_unique<GroupObject>(config);
        GroupObjectIndex index = table.addGroupObject(std::move(obj));
        
        TEST_ASSERT_EQUAL_UINT16(i, index.value());
    }
    
    TEST_ASSERT_EQUAL_UINT16(10, table.objectCount());
}

void test_GroupObjectTable_GetObject() {
    GroupObjectTableObject table;
    
    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 3);
    config.dpt = application::dptids::Bool;
    
    auto obj = std::make_unique<GroupObject>(config);
    GroupObjectIndex index = table.addGroupObject(std::move(obj));
    
    auto* retrieved = table.getGroupObject(index);
    TEST_ASSERT_NOT_NULL(retrieved);
    TEST_ASSERT_EQUAL_UINT16(GroupAddress(1, 2, 3).raw, retrieved->getAddress().raw);
}

void test_GroupObjectTable_GetObjectInvalidIndex() {
    GroupObjectTableObject table;
    
    auto* obj = table.getGroupObject(GroupObjectIndex(0));
    TEST_ASSERT_NULL(obj);
    
    obj = table.getGroupObject(GroupObjectIndex(100));
    TEST_ASSERT_NULL(obj);
}

void test_GroupObjectTable_RemoveObject() {
    GroupObjectTableObject table;
    
    // Add 3 objects
    for (uint16_t i = 0; i < 3; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i);
        auto obj = std::make_unique<GroupObject>(config);
        table.addGroupObject(std::move(obj));
    }
    
    TEST_ASSERT_EQUAL_UINT16(3, table.objectCount());
    
    // Remove middle object
    TEST_ASSERT_TRUE(table.removeObject(GroupObjectIndex(1)).isOk());
    TEST_ASSERT_EQUAL_UINT16(2, table.objectCount());
}

void test_GroupObjectTable_RemoveInvalidIndex() {
    GroupObjectTableObject table;
    
    TEST_ASSERT_FALSE(table.removeObject(GroupObjectIndex(0)).isOk());
    
    TEST_ASSERT_FALSE(table.removeObject(GroupObjectIndex(100)).isOk());
}

void test_GroupObjectTable_ClearObjects() {
    GroupObjectTableObject table;
    
    // Add objects
    for (uint16_t i = 0; i < 5; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i);
        auto obj = std::make_unique<GroupObject>(config);
        table.addGroupObject(std::move(obj));
    }
    
    TEST_ASSERT_EQUAL_UINT16(5, table.objectCount());
    
    table.clear();
    TEST_ASSERT_EQUAL_UINT16(0, table.objectCount());
}

// === Address-Based Access Tests ===

void test_GroupObjectTable_FindByAddress() {
    GroupObjectTableObject table;
    
    GroupAddress target(1, 2, 3);
    
    GroupObjectConfig config{};
    config.address = target;
    auto obj = std::make_unique<GroupObject>(config);
    table.addGroupObject(std::move(obj));
    
    auto* found = table.findByAddress(target);
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL_UINT16(target.raw, found->getAddress().raw);
}

void test_GroupObjectTable_FindByAddressNotFound() {
    GroupObjectTableObject table;
    
    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 3);
    auto obj = std::make_unique<GroupObject>(config);
    table.addGroupObject(std::move(obj));
    
    auto* found = table.findByAddress(GroupAddress(5, 6, 7));
    TEST_ASSERT_NULL(found);
}

void test_GroupObjectTable_FindAllByAddress() {
    GroupObjectTableObject table;
    
    GroupAddress target(1, 2, 3);
    
    // Add 3 objects with same address
    for (int i = 0; i < 3; ++i) {
        GroupObjectConfig config{};
        config.address = target;
        auto obj = std::make_unique<GroupObject>(config);
        table.addGroupObject(std::move(obj));
    }
    
    // Add 1 object with different address
    GroupObjectConfig config{};
    config.address = GroupAddress(5, 6, 7);
    auto obj = std::make_unique<GroupObject>(config);
    table.addGroupObject(std::move(obj));
    
    auto sizeRes = table.findAllByAddress(target, {});
    TEST_ASSERT_TRUE(sizeRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(3, sizeRes.value());

    GroupObjectIndex tooSmall[2]{};
    auto smallRes = table.findAllByAddress(target, tooSmall);
    TEST_ASSERT_TRUE(smallRes.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::BufferTooSmall), static_cast<int>(smallRes.error()));

    GroupObjectIndex indices[3]{};
    auto indicesRes = table.findAllByAddress(target, indices);
    TEST_ASSERT_TRUE(indicesRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(3, indicesRes.value());
    TEST_ASSERT_EQUAL_UINT16(0, indices[0].value());
    TEST_ASSERT_EQUAL_UINT16(1, indices[1].value());
    TEST_ASSERT_EQUAL_UINT16(2, indices[2].value());
}

void test_GroupObjectTable_HasAddress() {
    GroupObjectTableObject table;
    
    GroupAddress addr(1, 2, 3);
    
    TEST_ASSERT_FALSE(table.hasAddress(addr));
    
    GroupObjectConfig config{};
    config.address = addr;
    auto obj = std::make_unique<GroupObject>(config);
    table.addGroupObject(std::move(obj));
    
    TEST_ASSERT_TRUE(table.hasAddress(addr));
    TEST_ASSERT_FALSE(table.hasAddress(GroupAddress(5, 6, 7)));
}

// === Bulk Operations Tests ===

void test_GroupObjectTable_LoadTable() {
    GroupObjectTableObject table;
    
    std::vector<std::unique_ptr<GroupObject>> objects;
    for (uint16_t i = 0; i < 5; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i);
        objects.push_back(std::make_unique<GroupObject>(config));
    }
    
    TEST_ASSERT_TRUE(table.loadTable(std::move(objects)).isOk());
    TEST_ASSERT_EQUAL_UINT16(5, table.objectCount());
}

void test_GroupObjectTable_LoadTableTooMany() {
    GroupObjectTableObject table;
    
    std::vector<std::unique_ptr<GroupObject>> objects;
    for (uint16_t i = 0; i < 300; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i % 256);
        objects.push_back(std::make_unique<GroupObject>(config));
    }
    
    TEST_ASSERT_FALSE(table.loadTable(std::move(objects)).isOk());
}

void test_GroupObjectTable_Reserve() {
    GroupObjectTableObject table;
    
    // Should not crash
    table.reserve(100);
    table.reserve(256);
    table.reserve(300); // Over max, should be capped
    
    TEST_ASSERT_EQUAL_UINT16(0, table.objectCount());
}

// === Property Access Tests ===

void test_GroupObjectTable_PropertyAccess_ObjectType() {
    GroupObjectTableObject table;
    std::vector<uint8_t> value;
    
    TEST_ASSERT_TRUE(readGroupTableProperty(table,
                                            static_cast<application::PropertyID>(GroupObjectTableProperty::ObjectType),
                                            1,
                                            value)
                         .isOk());
    TEST_ASSERT_EQUAL_UINT16(2, value.size());
    TEST_ASSERT_EQUAL_UINT8(0x00, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x09, value[1]); // Interface Object Type 9
}

void test_GroupObjectTable_PropertyAccess_TableSize() {
    GroupObjectTableObject table;
    
    // Add 3 objects
    for (uint16_t i = 0; i < 3; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i);
        auto obj = std::make_unique<GroupObject>(config);
        table.addGroupObject(std::move(obj));
    }
    
    // Non-standard PID 8 (TableSize) was removed from the table objects; a
    // read of it must now fail cleanly instead of answering.
    std::vector<uint8_t> value;
    TEST_ASSERT_FALSE(readGroupTableProperty(table,
                                             static_cast<application::PropertyID>(8),
                                             1,
                                             value)
                          .isOk());
}

void test_GroupObjectTable_PropertyAccess_TableData() {
    GroupObjectTableObject table;

    // Two DPT 1 objects with distinguishable flag sets.
    GroupObjectConfig first{};
    first.address = GroupAddress(1, 2, 0);
    first.dpt = application::dptids::Bool;
    first.flags.communication = true;
    first.flags.write = true;
    first.flags.priority = Priority::Low;
    table.addGroupObject(std::make_unique<GroupObject>(first));

    GroupObjectConfig second{};
    second.address = GroupAddress(1, 2, 1);
    second.dpt = application::dptids::Bool;
    second.flags.communication = true;
    second.flags.read = true;
    second.flags.transmit = true;
    second.flags.readOnInit = true;
    second.flags.priority = Priority::Urgent;
    table.addGroupObject(std::make_unique<GroupObject>(second));

    // KNX 03/05/01 Table 51: 2-octet length field followed by one 2-octet
    // Group Object Descriptor per object.
    const uint16_t expectedBytes = 2 + 2 * 2;
    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readGroupTableProperty(table,
                                            static_cast<application::PropertyID>(GroupObjectTableProperty::TableData),
                                            expectedBytes,
                                            value)
                         .isOk());
    TEST_ASSERT_EQUAL(expectedBytes, value.size());

    // Length field.
    TEST_ASSERT_EQUAL(0u, value[0]);
    TEST_ASSERT_EQUAL(2u, value[1]);

    // Table 52 bit layout: U(15) T(14) I(13) W(12) R(11) C(10) prio(9..8) type(7..0).
    // DPT 1 is 1 bit wide → Value Field Type code 0 (Table 53).
    const uint16_t d0 = static_cast<uint16_t>((value[2] << 8) | value[3]);
    TEST_ASSERT_EQUAL(0x1400u | (static_cast<uint16_t>(Priority::Low) << 8),
                      d0);  // C + W, priority Low, type 0

    const uint16_t d1 = static_cast<uint16_t>((value[4] << 8) | value[5]);
    TEST_ASSERT_EQUAL(0x6C00u | (static_cast<uint16_t>(Priority::Urgent) << 8),
                      d1);  // T + I + R + C, priority Urgent, type 0
}

void test_GroupObjectTable_DescriptorRoundTrip() {
    // encode/decode must be exact inverses, otherwise an ETS download would
    // silently rewrite flags it did not intend to change.
    application::GroupObjectFlags flags{};
    flags.communication = true;
    flags.read = true;
    flags.write = false;
    flags.transmit = true;
    flags.update = true;
    flags.readOnInit = true;
    flags.priority = Priority::Normal;

    const uint16_t descriptor = application::encodeGroupObjectDescriptor(flags, 8u);

    uint8_t valueFieldType = 0;
    const auto decoded = application::decodeGroupObjectDescriptor(descriptor, valueFieldType);

    TEST_ASSERT_EQUAL(8u, valueFieldType);
    TEST_ASSERT_EQUAL(flags.communication, decoded.communication);
    TEST_ASSERT_EQUAL(flags.read, decoded.read);
    TEST_ASSERT_EQUAL(flags.write, decoded.write);
    TEST_ASSERT_EQUAL(flags.transmit, decoded.transmit);
    TEST_ASSERT_EQUAL(flags.update, decoded.update);
    TEST_ASSERT_EQUAL(flags.readOnInit, decoded.readOnInit);
    TEST_ASSERT_EQUAL(static_cast<int>(flags.priority), static_cast<int>(decoded.priority));
}

void test_GroupObjectTable_ValueFieldTypeCoding() {
    // Table 53 spot checks, including the entries that are deliberately out of
    // sequence (5/7/9/11 octets) and the sequential tail.
    TEST_ASSERT_EQUAL(0u, application::valueFieldTypeForBits(1));
    TEST_ASSERT_EQUAL(1u, application::valueFieldTypeForBits(2));
    TEST_ASSERT_EQUAL(3u, application::valueFieldTypeForBits(4));
    TEST_ASSERT_EQUAL(6u, application::valueFieldTypeForBits(7));

    TEST_ASSERT_EQUAL(7u, application::valueFieldTypeForOctets(1));
    TEST_ASSERT_EQUAL(8u, application::valueFieldTypeForOctets(2));
    TEST_ASSERT_EQUAL(15u, application::valueFieldTypeForOctets(5));
    TEST_ASSERT_EQUAL(11u, application::valueFieldTypeForOctets(6));
    TEST_ASSERT_EQUAL(16u, application::valueFieldTypeForOctets(7));
    TEST_ASSERT_EQUAL(14u, application::valueFieldTypeForOctets(14));
    TEST_ASSERT_EQUAL(22u, application::valueFieldTypeForOctets(16));
    TEST_ASSERT_EQUAL(23u, application::valueFieldTypeForOctets(17));
    TEST_ASSERT_EQUAL(254u, application::valueFieldTypeForOctets(248));
    TEST_ASSERT_EQUAL(255u, application::valueFieldTypeForOctets(252));
}

void test_GroupObjectTable_PropertyAccess_TableDataWriteAppliesFlags() {
    GroupObjectTableObject table;

    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 0);
    config.dpt = application::dptids::Bool;
    config.flags.communication = true;
    config.flags.write = true;
    table.addGroupObject(std::make_unique<GroupObject>(config));

    // ETS writes descriptors starting after the 2-octet length field.  Turn
    // the object into a read+transmit object with read-on-init.
    application::GroupObjectFlags desired{};
    desired.communication = true;
    desired.read = true;
    desired.transmit = true;
    desired.readOnInit = true;
    desired.priority = Priority::Normal;
    const uint16_t descriptor = application::encodeGroupObjectDescriptor(desired, 0u);

    std::vector<uint8_t> value = {
        0x00, 0x02,  // length field (ignored on write)
        static_cast<uint8_t>(descriptor >> 8),
        static_cast<uint8_t>(descriptor & 0xFF),
    };
    TEST_ASSERT_TRUE(writeGroupTableProperty(table,
                                             static_cast<application::PropertyID>(GroupObjectTableProperty::TableData),
                                             value)
                         .isOk());

    const auto* obj = table.getGroupObject(GroupObjectIndex(0));
    TEST_ASSERT_NOT_NULL(obj);
    TEST_ASSERT_TRUE(obj->readable());
    TEST_ASSERT_TRUE(obj->transmit());
    TEST_ASSERT_TRUE(obj->readOnInit());
    TEST_ASSERT_FALSE(obj->writable());
    TEST_ASSERT_EQUAL(static_cast<int>(Priority::Normal), static_cast<int>(obj->priority()));
}

void test_GroupObjectTable_PropertyAccess_UnsupportedRead() {
    GroupObjectTableObject table;
    std::vector<uint8_t> value;
    
    TEST_ASSERT_FALSE(readGroupTableProperty(table,
                                             static_cast<application::PropertyID>(GroupObjectTableProperty::ErrorCode),
                                             1,
                                             value)
                          .isOk());
}

void test_GroupObjectTable_PropertyAccess_MisalignedWriteRejected() {
    GroupObjectTableObject table;

    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 0);
    config.dpt = application::dptids::Bool;
    table.addGroupObject(std::make_unique<GroupObject>(config));

    // Three octets starting at 0 leaves the cursor mid-descriptor.  Applying
    // half a descriptor would corrupt a flag set, so it must be rejected
    // rather than partially applied.
    std::vector<uint8_t> value = {0x00, 0x02, 0x03};
    TEST_ASSERT_FALSE(writeGroupTableProperty(table,
                                              static_cast<application::PropertyID>(GroupObjectTableProperty::TableData),
                                              value)
                          .isOk());
}

// ETS discovers Group Object Diagnostics by reading the description of
// PID_GO_DIAGNOSTICS: 03/03/07 §3.4.8.3 requires a PDT_FUNCTION property to be
// described with type = PDT_FUNCTION and max_nr_of_elem = 1. The function
// itself is invoked through the Function Property services (implemented in the
// BAU), so the property must be describable without being value-readable.
void test_GroupObjectTable_PropertyAccess_GoDiagnosticsIsDescribedAsFunction() {
    GroupObjectTableObject table;
    const auto binding = table.kernelBinding();

    application::PropertyID resolved{};
    application::PropertyDataType type{};
    uint16_t maxElements = 0;
    PropertyCapability capability = PropertyCapability::ReadOnly;

    TEST_ASSERT_TRUE(describeProperty(binding.handlers,
                                      binding.handlerCount,
                                      static_cast<application::PropertyID>(GroupObjectTableProperty::GoDiagnostics),
                                      PropertyIndex(0),
                                      resolved,
                                      type,
                                      maxElements,
                                      capability)
                         .isOk());

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(GroupObjectTableProperty::GoDiagnostics),
                            static_cast<uint8_t>(resolved));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(application::PropertyDataType::Function),
                            static_cast<uint8_t>(type));
    TEST_ASSERT_EQUAL_UINT16(1u, maxElements);

    // A function property holds no value: an A_PropertyValue_Read of it is a
    // client error, not a property that happens to be empty.
    std::vector<uint8_t> value;
    TEST_ASSERT_FALSE(readGroupTableProperty(table,
                                             static_cast<application::PropertyID>(GroupObjectTableProperty::GoDiagnostics),
                                             1,
                                             value)
                          .isOk());
}

// === Runtime State Tests ===

void test_GroupObjectTable_GetAllAddresses() {
    GroupObjectTableObject table;
    
    // Add objects with various addresses
    std::vector<GroupAddress> expected;
    for (uint16_t i = 0; i < 5; ++i) {
        GroupAddress addr(1, 2, i);
        expected.push_back(addr);
        
        GroupObjectConfig config{};
        config.address = addr;
        auto obj = std::make_unique<GroupObject>(config);
        table.addGroupObject(std::move(obj));
    }
    
    auto neededRes = table.getAllAddresses(std::span<GroupAddress>{});
    TEST_ASSERT_TRUE(neededRes.isOk());
    const size_t needed = neededRes.value();
    std::vector<GroupAddress> addresses(needed);
    auto fillRes = table.getAllAddresses(std::span<GroupAddress>(addresses.data(), addresses.size()));
    TEST_ASSERT_TRUE(fillRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(needed, fillRes.value());

    TEST_ASSERT_EQUAL_UINT16(5, addresses.size());
    for (size_t i = 0; i < addresses.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT16(expected[i].raw, addresses[i].raw);
    }
}

void test_GroupObjectTable_GetAllAddresses_BufferTooSmall() {
    GroupObjectTableObject table;

    for (uint16_t i = 0; i < 3; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i);
        auto obj = std::make_unique<GroupObject>(config);
        table.addGroupObject(std::move(obj));
    }

    std::vector<GroupAddress> addresses(2);
    auto result = table.getAllAddresses(std::span<GroupAddress>(addresses.data(), addresses.size()));
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::BufferTooSmall, result.error());
}

void test_GroupObjectTable_IsValid() {
    GroupObjectTableObject table;
    
    // Empty table is valid
    TEST_ASSERT_TRUE(table.isValid());
    
    // Add object and set value to make it valid
    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 3);
    config.dpt = application::dptids::Bool;
    config.flags.write = true;
    auto obj = std::make_unique<GroupObject>(config);
    (void)obj->setValue(true); // Make object valid
    table.addGroupObject(std::move(obj));
    
    TEST_ASSERT_TRUE(table.isValid());
}

void test_GroupObjectTable_GetStatistics() {
    GroupObjectTableObject table;
    
    // Add 3 objects with values to make them valid
    for (uint16_t i = 0; i < 3; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i);
        config.dpt = application::dptids::Bool;
        config.flags.write = true;
        auto obj = std::make_unique<GroupObject>(config);
        (void)obj->setValue(true); // Make valid and active
        table.addGroupObject(std::move(obj));
    }
    
    auto stats = table.getStatistics();
    
    TEST_ASSERT_EQUAL_UINT16(3, stats.totalObjects);
    TEST_ASSERT_EQUAL_UINT16(3, stats.validObjects);
    TEST_ASSERT_EQUAL_UINT16(3, stats.activeObjects); // All have data now
    TEST_ASSERT_EQUAL_UINT16(3, stats.uniqueAddresses);
}

void test_GroupObjectTable_GetStatisticsWithDuplicateAddresses() {
    GroupObjectTableObject table;
    
    GroupAddress sharedAddr(1, 2, 3);
    
    // Add 3 objects with same address
    for (int i = 0; i < 3; ++i) {
        GroupObjectConfig config{};
        config.address = sharedAddr;
        auto obj = std::make_unique<GroupObject>(config);
        table.addGroupObject(std::move(obj));
    }
    
    // Add 1 object with different address
    GroupObjectConfig config{};
    config.address = GroupAddress(5, 6, 7);
    auto obj = std::make_unique<GroupObject>(config);
    table.addGroupObject(std::move(obj));
    
    auto stats = table.getStatistics();
    
    TEST_ASSERT_EQUAL_UINT16(4, stats.totalObjects);
    TEST_ASSERT_EQUAL_UINT16(2, stats.uniqueAddresses); // Only 2 unique
}

void test_GroupObjectTable_GetStatisticsWithActiveObjects() {
    GroupObjectTableObject table;
    
    // Add objects and set values
    for (uint16_t i = 0; i < 3; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i);
        config.dpt = application::dptids::Bool;
        config.flags.write = true; // Make writable so setValue works
        auto obj = std::make_unique<GroupObject>(config);
        
        if (i < 2) {
            (void)obj->setValue(true); // Set value for first 2 objects
        }
        
        table.addGroupObject(std::move(obj));
    }
    
    auto stats = table.getStatistics();
    
    TEST_ASSERT_EQUAL_UINT16(3, stats.totalObjects);
    TEST_ASSERT_EQUAL_UINT16(2, stats.validObjects); // Only 2 have values
    TEST_ASSERT_EQUAL_UINT16(2, stats.activeObjects); // 2 with data
}

// === Integration Tests ===

void test_GroupObjectTable_FullWorkflow() {
    GroupObjectTableObject table;
    
    // 1. Add objects
    std::vector<GroupObjectIndex> indices;
    for (uint16_t i = 0; i < 5; ++i) {
        GroupObjectConfig config{};
        config.address = GroupAddress(1, 2, i);
        config.dpt = application::dptids::Bool;
        auto obj = std::make_unique<GroupObject>(config);
        GroupObjectIndex idx = table.addGroupObject(std::move(obj));
        indices.push_back(idx);
    }
    
    // 2. Verify count
    TEST_ASSERT_EQUAL_UINT16(5, table.objectCount());
    
    // 3. Find by address
    auto* obj = table.findByAddress(GroupAddress(1, 2, 2));
    TEST_ASSERT_NOT_NULL(obj);
    
    // 4. Get statistics
    auto stats = table.getStatistics();
    TEST_ASSERT_EQUAL_UINT16(5, stats.totalObjects);
    
    // 5. Remove one object
    TEST_ASSERT_TRUE(table.removeObject(GroupObjectIndex(2)).isOk());
    TEST_ASSERT_EQUAL_UINT16(4, table.objectCount());
    
    // 6. Clear all
    table.clear();
    TEST_ASSERT_EQUAL_UINT16(0, table.objectCount());
}

// === Main Test Runner ===

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Construction
    RUN_TEST(test_GroupObjectTable_DefaultConstruction);
    
    // Object management
    RUN_TEST(test_GroupObjectTable_AddObject);
    RUN_TEST(test_GroupObjectTable_AddNullObject);
    RUN_TEST(test_GroupObjectTable_AddMultipleObjects);
    RUN_TEST(test_GroupObjectTable_GetObject);
    RUN_TEST(test_GroupObjectTable_GetObjectInvalidIndex);
    RUN_TEST(test_GroupObjectTable_RemoveObject);
    RUN_TEST(test_GroupObjectTable_RemoveInvalidIndex);
    RUN_TEST(test_GroupObjectTable_ClearObjects);
    
    // Address-based access
    RUN_TEST(test_GroupObjectTable_FindByAddress);
    RUN_TEST(test_GroupObjectTable_FindByAddressNotFound);
    RUN_TEST(test_GroupObjectTable_FindAllByAddress);
    RUN_TEST(test_GroupObjectTable_HasAddress);
    
    // Bulk operations
    RUN_TEST(test_GroupObjectTable_LoadTable);
    RUN_TEST(test_GroupObjectTable_LoadTableTooMany);
    RUN_TEST(test_GroupObjectTable_Reserve);
    
    // Property access
    RUN_TEST(test_GroupObjectTable_PropertyAccess_ObjectType);
    RUN_TEST(test_GroupObjectTable_PropertyAccess_TableSize);
    RUN_TEST(test_GroupObjectTable_PropertyAccess_TableData);
    RUN_TEST(test_GroupObjectTable_DescriptorRoundTrip);
    RUN_TEST(test_GroupObjectTable_ValueFieldTypeCoding);
    RUN_TEST(test_GroupObjectTable_PropertyAccess_TableDataWriteAppliesFlags);
    RUN_TEST(test_GroupObjectTable_PropertyAccess_UnsupportedRead);
    RUN_TEST(test_GroupObjectTable_PropertyAccess_MisalignedWriteRejected);
    RUN_TEST(test_GroupObjectTable_PropertyAccess_GoDiagnosticsIsDescribedAsFunction);
    
    // Runtime state
    RUN_TEST(test_GroupObjectTable_GetAllAddresses);
    RUN_TEST(test_GroupObjectTable_GetAllAddresses_BufferTooSmall);
    RUN_TEST(test_GroupObjectTable_IsValid);
    RUN_TEST(test_GroupObjectTable_GetStatistics);
    RUN_TEST(test_GroupObjectTable_GetStatisticsWithDuplicateAddresses);
    RUN_TEST(test_GroupObjectTable_GetStatisticsWithActiveObjects);
    
    // Integration
    RUN_TEST(test_GroupObjectTable_FullWorkflow);
    
    return UNITY_END();
}
