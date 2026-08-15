// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_address_association_tables.cpp
 * @brief Unit tests for AddressTableObject and AssociationTableObject
 */

#include "knx/objects/address_table_object.hpp"
#include "knx/objects/association_table_object.hpp"
#include "knx/objects/property_kernel.hpp"
#include <unity.h>
#include <vector>
#include <span>

using namespace knx;
using namespace knx::objects;

static util::Result<void> readAddressProperty(const AddressTableObject& obj,
                                              application::PropertyID id,
                                              uint16_t elementCount,
                                              std::vector<uint8_t>& out) {
    const auto binding = obj.kernelBinding();
    return readProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, elementCount, out);
}

static util::Result<void> writeAddressProperty(AddressTableObject& obj,
                                               application::PropertyID id,
                                               std::span<const uint8_t> value) {
    const auto binding = obj.kernelBinding();
    return writeProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, value);
}

static util::Result<void> readAssociationProperty(const AssociationTableObject& obj,
                                                  application::PropertyID id,
                                                  uint16_t elementCount,
                                                  std::vector<uint8_t>& out) {
    const auto binding = obj.kernelBinding();
    return readProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, elementCount, out);
}

static util::Result<void> writeAssociationProperty(AssociationTableObject& obj,
                                                   application::PropertyID id,
                                                   std::span<const uint8_t> value) {
    const auto binding = obj.kernelBinding();
    return writeProperty(binding.handlers, binding.handlerCount, binding.context, id, 1, value);
}

static AddressTableObject* addrTable = nullptr;
static AssociationTableObject* assocTable = nullptr;

void setUp() {
    addrTable = new AddressTableObject();
    assocTable = new AssociationTableObject();
}

void tearDown() {
    delete addrTable;
    delete assocTable;
    addrTable = nullptr;
    assocTable = nullptr;
}

// === Address Table Tests ===

void test_AddressTable_DefaultConstruction() {
    TEST_ASSERT_EQUAL_UINT16(0, addrTable->entryCount());
    TEST_ASSERT_TRUE(addrTable->isEmpty());
    TEST_ASSERT_FALSE(addrTable->isFull());
    TEST_ASSERT_EQUAL_UINT16(256, addrTable->maxEntries());
}

void test_AddressTable_AddEntry() {
    GroupAddress addr1(1, 2, 3);
    AddressTableIndex index = addrTable->addEntry(addr1);
    
    TEST_ASSERT_EQUAL_UINT16(1, index.value()); // First entry gets index 1
    TEST_ASSERT_EQUAL_UINT16(1, addrTable->entryCount());
    TEST_ASSERT_FALSE(addrTable->isEmpty());
}

void test_AddressTable_AddDuplicateEntry() {
    GroupAddress addr1(1, 2, 3);
    AddressTableIndex index1 = addrTable->addEntry(addr1);
    AddressTableIndex index2 = addrTable->addEntry(addr1); // Add same address
    
    TEST_ASSERT_EQUAL_UINT16(index1.value(), index2.value()); // Should return same index
    TEST_ASSERT_EQUAL_UINT16(1, addrTable->entryCount()); // No duplicate added
}

void test_AddressTable_AddMultipleEntries() {
    addrTable->addEntry(GroupAddress(1, 2, 3));
    addrTable->addEntry(GroupAddress(4, 5, 6));
    addrTable->addEntry(GroupAddress(7, 8, 9));
    
    TEST_ASSERT_EQUAL_UINT16(3, addrTable->entryCount());
}

void test_AddressTable_GetAddress() {
    GroupAddress addr1(1, 2, 3);
    GroupAddress addr2(4, 5, 6);
    
    addrTable->addEntry(addr1);
    addrTable->addEntry(addr2);
    
    // Index is 1-based
    TEST_ASSERT_EQUAL_UINT16(addr1.raw, addrTable->getAddress(AddressTableIndex(1)).raw);
    TEST_ASSERT_EQUAL_UINT16(addr2.raw, addrTable->getAddress(AddressTableIndex(2)).raw);
    
    // Invalid index returns 0
    TEST_ASSERT_EQUAL_UINT16(0, addrTable->getAddress(AddressTableIndex::invalid()).raw);
    TEST_ASSERT_EQUAL_UINT16(0, addrTable->getAddress(AddressTableIndex(99)).raw);
}

void test_AddressTable_FindIndex() {
    GroupAddress addr1(1, 2, 3);
    GroupAddress addr2(4, 5, 6);
    GroupAddress addr3(7, 8, 9);
    
    addrTable->addEntry(addr1);
    addrTable->addEntry(addr2);
    
    TEST_ASSERT_EQUAL_UINT16(1, addrTable->findIndex(addr1).value());
    TEST_ASSERT_EQUAL_UINT16(2, addrTable->findIndex(addr2).value());
    TEST_ASSERT_EQUAL_UINT16(0, addrTable->findIndex(addr3).value()); // Not found
}

void test_AddressTable_RemoveEntry() {
    addrTable->addEntry(GroupAddress(1, 2, 3));
    addrTable->addEntry(GroupAddress(4, 5, 6));
    addrTable->addEntry(GroupAddress(7, 8, 9));
    
    TEST_ASSERT_EQUAL_UINT16(3, addrTable->entryCount());
    
    TEST_ASSERT_TRUE(addrTable->removeEntry(AddressTableIndex(2)).isOk()); // Remove middle entry (1-based)
    TEST_ASSERT_EQUAL_UINT16(2, addrTable->entryCount());
    
    TEST_ASSERT_FALSE(addrTable->removeEntry(AddressTableIndex::invalid()).isOk()); // Invalid index
    TEST_ASSERT_FALSE(addrTable->removeEntry(AddressTableIndex(99)).isOk()); // Invalid index
}

void test_AddressTable_SetEntry() {
    addrTable->addEntry(GroupAddress(1, 2, 3));
    addrTable->addEntry(GroupAddress(4, 5, 6));
    
    GroupAddress newAddr(7, 8, 9);
    TEST_ASSERT_TRUE(addrTable->setEntry(AddressTableIndex(1), newAddr).isOk()); // Update first entry
    TEST_ASSERT_EQUAL_UINT16(newAddr.raw, addrTable->getAddress(AddressTableIndex(1)).raw);
    
    TEST_ASSERT_FALSE(addrTable->setEntry(AddressTableIndex::invalid(), newAddr).isOk()); // Invalid index
}

void test_AddressTable_ClearEntries() {
    addrTable->addEntry(GroupAddress(1, 2, 3));
    addrTable->addEntry(GroupAddress(4, 5, 6));
    TEST_ASSERT_EQUAL_UINT16(2, addrTable->entryCount());
    
    addrTable->clearEntries();
    TEST_ASSERT_EQUAL_UINT16(0, addrTable->entryCount());
    TEST_ASSERT_TRUE(addrTable->isEmpty());
}

void test_AddressTable_LoadTable() {
    std::vector<GroupAddress> addresses = {
        GroupAddress(1, 2, 3),
        GroupAddress(4, 5, 6),
        GroupAddress(7, 8, 9)
    };
    
    TEST_ASSERT_TRUE(addrTable->loadTable(addresses).isOk());
    TEST_ASSERT_EQUAL_UINT16(3, addrTable->entryCount());
    TEST_ASSERT_EQUAL_UINT16(addresses[0].raw, addrTable->getAddress(AddressTableIndex(1)).raw);
}

void test_AddressTable_PropertyAccess_TableSize() {
    // Non-standard PID 8 (TableSize) was removed from the table objects; a
    // read of it must now fail cleanly instead of answering.
    addrTable->addEntry(GroupAddress(1, 2, 3));

    std::vector<uint8_t> value;
    TEST_ASSERT_FALSE(readAddressProperty(*addrTable,
                                          static_cast<application::PropertyID>(8),
                                          1,
                                          value)
                          .isOk());
}

void test_AddressTable_PropertyAccess_TableData() {
    addrTable->addEntry(GroupAddress(0x1122));
    addrTable->addEntry(GroupAddress(0x3344));
    
    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readAddressProperty(*addrTable,
                                         static_cast<application::PropertyID>(AddressTableProperty::TableData),
                                         2,
                                         value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(4, value.size()); // 2 entries × 2 bytes
    TEST_ASSERT_EQUAL_UINT8(0x11, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x22, value[1]);
    TEST_ASSERT_EQUAL_UINT8(0x33, value[2]);
    TEST_ASSERT_EQUAL_UINT8(0x44, value[3]);
}

void test_AddressTable_PropertyAccess_SetTableData() {
    std::vector<uint8_t> data = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    TEST_ASSERT_TRUE(writeAddressProperty(*addrTable,
                                          static_cast<application::PropertyID>(AddressTableProperty::TableData),
                                          data)
                         .isOk());
    TEST_ASSERT_EQUAL_UINT16(3, addrTable->entryCount());
    TEST_ASSERT_EQUAL_UINT16(0x1122, addrTable->getAddress(AddressTableIndex(1)).raw);
    TEST_ASSERT_EQUAL_UINT16(0x3344, addrTable->getAddress(AddressTableIndex(2)).raw);
    TEST_ASSERT_EQUAL_UINT16(0x5566, addrTable->getAddress(AddressTableIndex(3)).raw);
}

void test_AddressTable_IsValid() {
    TEST_ASSERT_TRUE(addrTable->isValid()); // Empty table is valid
    
    addrTable->addEntry(GroupAddress(1, 2, 3));
    TEST_ASSERT_TRUE(addrTable->isValid());
}

void test_AddressTable_GetAllAddresses_BufferHandling() {
    addrTable->addEntry(GroupAddress(0x1122));
    addrTable->addEntry(GroupAddress(0x3344));
    addrTable->addEntry(GroupAddress(0x5566));

    auto neededRes = addrTable->getAllAddresses(std::span<GroupAddress>{});
    TEST_ASSERT_TRUE(neededRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(3, neededRes.value());

    std::vector<GroupAddress> tooSmall(2);
    auto tooSmallRes = addrTable->getAllAddresses(std::span<GroupAddress>(tooSmall.data(), tooSmall.size()));
    TEST_ASSERT_TRUE(tooSmallRes.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::BufferTooSmall, tooSmallRes.error());

    std::vector<GroupAddress> all(neededRes.value());
    auto fillRes = addrTable->getAllAddresses(std::span<GroupAddress>(all.data(), all.size()));
    TEST_ASSERT_TRUE(fillRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(3, fillRes.value());
    TEST_ASSERT_EQUAL_UINT16(0x1122, all[0].raw);
    TEST_ASSERT_EQUAL_UINT16(0x3344, all[1].raw);
    TEST_ASSERT_EQUAL_UINT16(0x5566, all[2].raw);
}

// === Association Table Tests ===

void test_AssociationTable_DefaultConstruction() {
    TEST_ASSERT_EQUAL_UINT16(0, assocTable->entryCount());
    TEST_ASSERT_TRUE(assocTable->isEmpty());
    TEST_ASSERT_FALSE(assocTable->isFull());
    TEST_ASSERT_EQUAL_UINT16(512, assocTable->maxEntries());
}

void test_AssociationTable_AddEntry() {
    AssociationEntry entry(AddressTableIndex(1), GroupObjectIndex(10)); // Address index 1 → Group object 10
    TEST_ASSERT_TRUE(assocTable->addEntry(entry).isOk());
    TEST_ASSERT_EQUAL_UINT16(1, assocTable->entryCount());
}

void test_AssociationTable_AddInvalidEntry() {
    AssociationEntry invalidEntry(AddressTableIndex::invalid(), GroupObjectIndex(10)); // Address index 0 is invalid
    TEST_ASSERT_FALSE(assocTable->addEntry(invalidEntry).isOk());
    TEST_ASSERT_EQUAL_UINT16(0, assocTable->entryCount());
}

void test_AssociationTable_AddDuplicateEntry() {
    AssociationEntry entry(AddressTableIndex(1), GroupObjectIndex(10));
    TEST_ASSERT_TRUE(assocTable->addEntry(entry).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(entry).isOk()); // Duplicate
    TEST_ASSERT_EQUAL_UINT16(1, assocTable->entryCount()); // No duplicate added
}

void test_AssociationTable_FindGroupObjects() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(11))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(2), GroupObjectIndex(12))).isOk());

    auto sizeRes = assocTable->findGroupObjects(AddressTableIndex(1), {});
    TEST_ASSERT_TRUE(sizeRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(2, sizeRes.value());

    GroupObjectIndex tooSmall[1]{};
    auto smallRes = assocTable->findGroupObjects(AddressTableIndex(1), tooSmall);
    TEST_ASSERT_TRUE(smallRes.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::BufferTooSmall), static_cast<int>(smallRes.error()));

    GroupObjectIndex objects[2]{};
    auto objectsRes = assocTable->findGroupObjects(AddressTableIndex(1), objects);
    TEST_ASSERT_TRUE(objectsRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(2, objectsRes.value());
    TEST_ASSERT_EQUAL_UINT16(10, objects[0].value());
    TEST_ASSERT_EQUAL_UINT16(11, objects[1].value());

    GroupObjectIndex objects2[1]{};
    auto objectsRes2 = assocTable->findGroupObjects(AddressTableIndex(2), objects2);
    TEST_ASSERT_TRUE(objectsRes2.isOk());
    TEST_ASSERT_EQUAL_UINT32(1, objectsRes2.value());
    TEST_ASSERT_EQUAL_UINT16(12, objects2[0].value());
}

void test_AssociationTable_FindAddressIndices() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(2), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(3), GroupObjectIndex(11))).isOk());

    auto sizeRes = assocTable->findAddressIndices(GroupObjectIndex(10), {});
    TEST_ASSERT_TRUE(sizeRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(2, sizeRes.value());

    AddressTableIndex tooSmall[1]{};
    auto smallRes = assocTable->findAddressIndices(GroupObjectIndex(10), tooSmall);
    TEST_ASSERT_TRUE(smallRes.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::BufferTooSmall), static_cast<int>(smallRes.error()));

    AddressTableIndex indices[2]{};
    auto indicesRes = assocTable->findAddressIndices(GroupObjectIndex(10), indices);
    TEST_ASSERT_TRUE(indicesRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(2, indicesRes.value());
    TEST_ASSERT_EQUAL_UINT16(1, indices[0].value());
    TEST_ASSERT_EQUAL_UINT16(2, indices[1].value());

    AddressTableIndex indices2[1]{};
    auto indicesRes2 = assocTable->findAddressIndices(GroupObjectIndex(11), indices2);
    TEST_ASSERT_TRUE(indicesRes2.isOk());
    TEST_ASSERT_EQUAL_UINT32(1, indicesRes2.value());
    TEST_ASSERT_EQUAL_UINT16(3, indices2[0].value());
}

void test_AssociationTable_HasAssociation() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    
    TEST_ASSERT_TRUE(assocTable->hasAssociation(AddressTableIndex(1), GroupObjectIndex(10)));
    TEST_ASSERT_FALSE(assocTable->hasAssociation(AddressTableIndex(1), GroupObjectIndex(11)));
    TEST_ASSERT_FALSE(assocTable->hasAssociation(AddressTableIndex(2), GroupObjectIndex(10)));
}

void test_AssociationTable_GetEntry() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(2), GroupObjectIndex(20))).isOk());
    
    const AssociationEntry* entry = assocTable->getEntry(0);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT16(1, entry->addressIndex.value());
    TEST_ASSERT_EQUAL_UINT16(10, entry->groupObjectNumber.value());
    
    const AssociationEntry* invalid = assocTable->getEntry(99);
    TEST_ASSERT_NULL(invalid);
}

void test_AssociationTable_RemoveEntry() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(2), GroupObjectIndex(20))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(3), GroupObjectIndex(30))).isOk());
    
    TEST_ASSERT_TRUE(assocTable->removeEntry(1).isOk()); // Remove middle entry
    TEST_ASSERT_EQUAL_UINT16(2, assocTable->entryCount());
}

void test_AssociationTable_RemoveEntriesForAddress() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(11))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(2), GroupObjectIndex(12))).isOk());
    
    uint16_t removed = assocTable->removeEntriesForAddress(AddressTableIndex(1));
    TEST_ASSERT_EQUAL_UINT16(2, removed);
    TEST_ASSERT_EQUAL_UINT16(1, assocTable->entryCount());
}

void test_AssociationTable_RemoveEntriesForGroupObject() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(2), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(3), GroupObjectIndex(11))).isOk());
    
    uint16_t removed = assocTable->removeEntriesForGroupObject(GroupObjectIndex(10));
    TEST_ASSERT_EQUAL_UINT16(2, removed);
    TEST_ASSERT_EQUAL_UINT16(1, assocTable->entryCount());
}

void test_AssociationTable_ClearEntries() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(2), GroupObjectIndex(20))).isOk());
    
    assocTable->clearEntries();
    TEST_ASSERT_EQUAL_UINT16(0, assocTable->entryCount());
    TEST_ASSERT_TRUE(assocTable->isEmpty());
}

void test_AssociationTable_LoadTable() {
    std::vector<AssociationEntry> entries = {
        AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10)),
        AssociationEntry(AddressTableIndex(2), GroupObjectIndex(20)),
        AssociationEntry(AddressTableIndex(3), GroupObjectIndex(30))
    };
    
    TEST_ASSERT_TRUE(assocTable->loadTable(entries).isOk());
    TEST_ASSERT_EQUAL_UINT16(3, assocTable->entryCount());
}

void test_AssociationTable_PropertyAccess_TableData() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(0x0001), GroupObjectIndex(0x000A))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(0x0002), GroupObjectIndex(0x0014))).isOk());
    
    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readAssociationProperty(*assocTable,
                                             static_cast<application::PropertyID>(AssociationTableProperty::TableData),
                                             2,
                                             value)
                         .isOk());
    TEST_ASSERT_EQUAL_INT(8, value.size()); // 2 entries × 4 bytes
    
    // First entry: addressIndex=1, groupObjectNumber=10
    TEST_ASSERT_EQUAL_UINT8(0x00, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, value[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, value[2]);
    TEST_ASSERT_EQUAL_UINT8(0x0A, value[3]);
}

void test_AssociationTable_PropertyAccess_SetTableData() {
    std::vector<uint8_t> data = {
        0x00, 0x01, 0x00, 0x0A,  // Entry 1: addrIdx=1, goNum=10
        0x00, 0x02, 0x00, 0x14   // Entry 2: addrIdx=2, goNum=20
    };
    
    TEST_ASSERT_TRUE(writeAssociationProperty(*assocTable,
                                              static_cast<application::PropertyID>(AssociationTableProperty::TableData),
                                              data)
                         .isOk());
    TEST_ASSERT_EQUAL_UINT16(2, assocTable->entryCount());
    
    const AssociationEntry* entry = assocTable->getEntry(0);
    TEST_ASSERT_EQUAL_UINT16(1, entry->addressIndex.value());
    TEST_ASSERT_EQUAL_UINT16(10, entry->groupObjectNumber.value());
}

void test_AssociationTable_IsValid() {
    TEST_ASSERT_TRUE(assocTable->isValid()); // Empty table is valid
    
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->isValid());
}

void test_AssociationTable_GetAllEntries_BufferHandling() {
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(10))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(2), GroupObjectIndex(20))).isOk());
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(AddressTableIndex(3), GroupObjectIndex(30))).isOk());

    auto neededRes = assocTable->getAllEntries(std::span<AssociationEntry>{});
    TEST_ASSERT_TRUE(neededRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(3, neededRes.value());

    std::vector<AssociationEntry> tooSmall(2);
    auto tooSmallRes = assocTable->getAllEntries(std::span<AssociationEntry>(tooSmall.data(), tooSmall.size()));
    TEST_ASSERT_TRUE(tooSmallRes.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::BufferTooSmall, tooSmallRes.error());

    std::vector<AssociationEntry> all(neededRes.value());
    auto fillRes = assocTable->getAllEntries(std::span<AssociationEntry>(all.data(), all.size()));
    TEST_ASSERT_TRUE(fillRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(3, fillRes.value());
    TEST_ASSERT_EQUAL_UINT16(1, all[0].addressIndex.value());
    TEST_ASSERT_EQUAL_UINT16(10, all[0].groupObjectNumber.value());
    TEST_ASSERT_EQUAL_UINT16(3, all[2].addressIndex.value());
    TEST_ASSERT_EQUAL_UINT16(30, all[2].groupObjectNumber.value());
}

// === Integration Tests ===

void test_Integration_AddressAndAssociation() {
    // Build address table
    GroupAddress addr1(1, 2, 3);
    GroupAddress addr2(4, 5, 6);
    AddressTableIndex idx1 = addrTable->addEntry(addr1);
    AddressTableIndex idx2 = addrTable->addEntry(addr2);
    
    // Build association table
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(idx1, GroupObjectIndex(10))).isOk()); // addr1 → GO 10
    TEST_ASSERT_TRUE(assocTable->addEntry(AssociationEntry(idx2, GroupObjectIndex(20))).isOk()); // addr2 → GO 20
    
    // Verify mappings
    GroupObjectIndex objects1[1]{};
    auto objectsRes1 = assocTable->findGroupObjects(idx1, objects1);
    TEST_ASSERT_TRUE(objectsRes1.isOk());
    TEST_ASSERT_EQUAL_UINT32(1, objectsRes1.value());
    TEST_ASSERT_EQUAL_UINT16(10, objects1[0].value());

    GroupObjectIndex objects2[1]{};
    auto objectsRes2 = assocTable->findGroupObjects(idx2, objects2);
    TEST_ASSERT_TRUE(objectsRes2.isOk());
    TEST_ASSERT_EQUAL_UINT32(1, objectsRes2.value());
    TEST_ASSERT_EQUAL_UINT16(20, objects2[0].value());
}

// === Main Test Runner ===

int main(int argc, char** argv) {
    UNITY_BEGIN();
    
    // Address Table tests
    RUN_TEST(test_AddressTable_DefaultConstruction);
    RUN_TEST(test_AddressTable_AddEntry);
    RUN_TEST(test_AddressTable_AddDuplicateEntry);
    RUN_TEST(test_AddressTable_AddMultipleEntries);
    RUN_TEST(test_AddressTable_GetAddress);
    RUN_TEST(test_AddressTable_FindIndex);
    RUN_TEST(test_AddressTable_RemoveEntry);
    RUN_TEST(test_AddressTable_SetEntry);
    RUN_TEST(test_AddressTable_ClearEntries);
    RUN_TEST(test_AddressTable_LoadTable);
    RUN_TEST(test_AddressTable_PropertyAccess_TableSize);
    RUN_TEST(test_AddressTable_PropertyAccess_TableData);
    RUN_TEST(test_AddressTable_PropertyAccess_SetTableData);
    RUN_TEST(test_AddressTable_IsValid);
    RUN_TEST(test_AddressTable_GetAllAddresses_BufferHandling);
    
    // Association Table tests
    RUN_TEST(test_AssociationTable_DefaultConstruction);
    RUN_TEST(test_AssociationTable_AddEntry);
    RUN_TEST(test_AssociationTable_AddInvalidEntry);
    RUN_TEST(test_AssociationTable_AddDuplicateEntry);
    RUN_TEST(test_AssociationTable_FindGroupObjects);
    RUN_TEST(test_AssociationTable_FindAddressIndices);
    RUN_TEST(test_AssociationTable_HasAssociation);
    RUN_TEST(test_AssociationTable_GetEntry);
    RUN_TEST(test_AssociationTable_RemoveEntry);
    RUN_TEST(test_AssociationTable_RemoveEntriesForAddress);
    RUN_TEST(test_AssociationTable_RemoveEntriesForGroupObject);
    RUN_TEST(test_AssociationTable_ClearEntries);
    RUN_TEST(test_AssociationTable_LoadTable);
    RUN_TEST(test_AssociationTable_PropertyAccess_TableData);
    RUN_TEST(test_AssociationTable_PropertyAccess_SetTableData);
    RUN_TEST(test_AssociationTable_IsValid);
    RUN_TEST(test_AssociationTable_GetAllEntries_BufferHandling);
    
    // Integration tests
    RUN_TEST(test_Integration_AddressAndAssociation);
    
    return UNITY_END();
}
