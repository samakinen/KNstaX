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
#include <algorithm>
#include <cstddef>
#include <utility>
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
    TEST_ASSERT_EQUAL_UINT16(knx::config::ADDRESS_TABLE_ENTRIES, addrTable->maxEntries());
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
    TEST_ASSERT_EQUAL_UINT16(knx::config::ASSOCIATION_TABLE_ENTRIES, assocTable->maxEntries());
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
    
    const auto entry = assocTable->getEntry(0);
    TEST_ASSERT_TRUE(entry.has_value());
    TEST_ASSERT_EQUAL_UINT16(1, entry->addressIndex.value());
    TEST_ASSERT_EQUAL_UINT16(10, entry->groupObjectNumber.value());
    
    TEST_ASSERT_FALSE(assocTable->getEntry(99).has_value());
    TEST_ASSERT_FALSE(assocTable->getEntry(2).has_value());  // past the entries in force
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
    
    const auto entry = assocTable->getEntry(0);
    TEST_ASSERT_TRUE(entry.has_value());
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

// === Lookup indices (binary search, GA → group object index) ===

void test_AddressTable_SortedTableFindsEveryEntryAndRejectsGaps(void) {
    AddressTableObject table;
    std::vector<GroupAddress> addresses;
    for (uint16_t raw = 0x0100; raw < 0x0100 + 2 * AddressTableDomain::kMaxEntries; raw += 2) {
        addresses.push_back(GroupAddress(raw));
    }
    TEST_ASSERT_TRUE(table.loadTable(addresses).isOk());

    for (size_t i = 0; i < addresses.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT16(i + 1, table.findIndex(addresses[i]).raw);
        TEST_ASSERT_FALSE(table.findIndex(GroupAddress(addresses[i].raw + 1)).isValid());
    }
    TEST_ASSERT_FALSE(table.findIndex(GroupAddress(0x00FF)).isValid());
}

void test_AddressTable_UnsortedTableStillFindsEntries(void) {
    // The firmware may build a table out of order with addEntry(); a half-
    // written download also leaves placeholders. Lookups must not depend on order.
    AddressTableObject table;
    TEST_ASSERT_TRUE(table.addEntry(GroupAddress(0x0905)).isValid());
    TEST_ASSERT_TRUE(table.addEntry(GroupAddress(0x0102)).isValid());
    TEST_ASSERT_TRUE(table.addEntry(GroupAddress(0x0500)).isValid());
    TEST_ASSERT_EQUAL_UINT16(2, table.findIndex(GroupAddress(0x0102)).raw);
    TEST_ASSERT_EQUAL_UINT16(3, table.findIndex(GroupAddress(0x0500)).raw);
    TEST_ASSERT_EQUAL_UINT16(1, table.findIndex(GroupAddress(0x0905)).raw);

    // Rewriting it into order switches to the binary search, and it still works.
    TEST_ASSERT_TRUE(table.setEntry(AddressTableIndex(1), GroupAddress(0x0001)).isOk());
    TEST_ASSERT_TRUE(table.setEntry(AddressTableIndex(2), GroupAddress(0x0102)).isOk());
    TEST_ASSERT_EQUAL_UINT16(3, table.findIndex(GroupAddress(0x0500)).raw);
    TEST_ASSERT_EQUAL_UINT16(1, table.findIndex(GroupAddress(0x0001)).raw);
    TEST_ASSERT_FALSE(table.findIndex(GroupAddress(0x0905)).isValid());
}

void test_AssociationTable_LookupsFollowEveryMutationAndHaveNoPerAddressLimit(void) {
    AssociationTableObject table;
    // 40 objects on ASAP 3, interleaved with others, deliberately unsorted.
    for (uint16_t go = 0; go < 40; ++go) {
        TEST_ASSERT_TRUE(table.addEntry(AssociationEntry(AddressTableIndex(3), GroupObjectIndex(go))).isOk());
        TEST_ASSERT_TRUE(table.addEntry(AssociationEntry(AddressTableIndex(static_cast<uint16_t>(go % 2 ? 1 : 7)),
                                                         GroupObjectIndex(static_cast<uint16_t>(100 + go)))).isOk());
    }

    std::vector<uint16_t> seen;
    table.forEachGroupObject(AddressTableIndex(3), [&](GroupObjectIndex go) { seen.push_back(go.value()); });
    TEST_ASSERT_EQUAL_UINT32(40, seen.size());
    for (uint16_t go = 0; go < 40; ++go) {
        TEST_ASSERT_EQUAL_UINT16(go, seen[go]);  // table order is kept
    }
    TEST_ASSERT_TRUE(table.hasGroupObjects(AddressTableIndex(7)));
    TEST_ASSERT_FALSE(table.hasGroupObjects(AddressTableIndex(4)));

    // Removing entries shifts positions; the lookups must follow.
    TEST_ASSERT_EQUAL_UINT16(20, table.removeEntriesForAddress(AddressTableIndex(1)));
    seen.clear();
    table.forEachGroupObject(AddressTableIndex(3), [&](GroupObjectIndex go) { seen.push_back(go.value()); });
    TEST_ASSERT_EQUAL_UINT32(40, seen.size());
    TEST_ASSERT_FALSE(table.hasGroupObjects(AddressTableIndex(1)));

    std::vector<GroupObjectIndex> buffer(40);
    auto found = table.findGroupObjects(AddressTableIndex(3), buffer);
    TEST_ASSERT_TRUE(found.isOk());
    TEST_ASSERT_EQUAL_UINT32(40, found.value());

    table.clearEntries();
    TEST_ASSERT_FALSE(table.hasGroupObjects(AddressTableIndex(3)));
}

// === Capacity: a full table refuses the next entry ===

void test_AddressTable_CapacityIsAPromise(void) {
    AddressTableObject table;
    const uint16_t capacity = table.maxEntries();
    TEST_ASSERT_EQUAL_UINT16(knx::config::ADDRESS_TABLE_ENTRIES, capacity);
    for (uint16_t i = 0; i < capacity; ++i) {
        TEST_ASSERT_TRUE(table.addEntry(GroupAddress(static_cast<uint16_t>(0x0800 + i))).isValid());
    }
    TEST_ASSERT_TRUE(table.isFull());
    // The next one is refused (invalid index), and nothing was overwritten.
    TEST_ASSERT_FALSE(table.addEntry(GroupAddress(0x7000)).isValid());
    TEST_ASSERT_EQUAL_UINT16(capacity, table.entryCount());
    TEST_ASSERT_EQUAL_UINT16(0x0800, table.getAddress(AddressTableIndex(1)).raw);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(0x0800 + capacity - 1),
                             table.getAddress(AddressTableIndex(capacity)).raw);

    // The same limit applies to bulk loads and to explicit-index writes.
    std::vector<GroupAddress> tooMany;
    for (uint32_t i = 0; i <= capacity; ++i) {
        tooMany.push_back(GroupAddress(static_cast<uint16_t>(0x0800 + i)));
    }
    AddressTableObject other;
    TEST_ASSERT_TRUE(other.loadTable(tooMany).isError());
    TEST_ASSERT_EQUAL_UINT16(0, other.entryCount());
    TEST_ASSERT_TRUE(table.setEntryExpand(AddressTableIndex(static_cast<uint16_t>(capacity + 1)),
                                          GroupAddress(0x7001)).isError());
    TEST_ASSERT_EQUAL_UINT16(capacity, table.entryCount());
}

void test_AssociationTable_CapacityIsAPromise(void) {
    AssociationTableObject table;
    const uint16_t capacity = table.maxEntries();
    TEST_ASSERT_EQUAL_UINT16(knx::config::ASSOCIATION_TABLE_ENTRIES, capacity);
    for (uint16_t i = 0; i < capacity; ++i) {
        TEST_ASSERT_TRUE(table.addEntry(AssociationEntry(AddressTableIndex(static_cast<uint16_t>(1 + i)),
                                                         GroupObjectIndex(i))).isOk());
    }
    TEST_ASSERT_TRUE(table.isFull());
    const auto refused = table.addEntry(AssociationEntry(AddressTableIndex(1),
                                                         GroupObjectIndex(static_cast<uint16_t>(capacity))));
    TEST_ASSERT_TRUE(refused.isError());
    TEST_ASSERT_EQUAL_UINT16(capacity, table.entryCount());

    std::vector<AssociationEntry> tooMany;
    for (uint32_t i = 0; i <= capacity; ++i) {
        tooMany.push_back(AssociationEntry(AddressTableIndex(static_cast<uint16_t>(1 + i)),
                                           GroupObjectIndex(static_cast<uint16_t>(i))));
    }
    AssociationTableObject other;
    TEST_ASSERT_TRUE(other.loadTable(tooMany).isError());
    TEST_ASSERT_EQUAL_UINT16(0, other.entryCount());
}

// === The load segments ===
//
// What ETS does with the SystemB procedure (test_ets_connected_session.cpp,
// downloadLikeEts; the size gate's traffic.hpp): PID_LOAD_STATE_CONTROL Unload
// and StartLoading, the segment by A_Memory_Write in 12-octet chunks (which
// straddle the 4-octet association entries), one A_Memory_Read of the first
// chunk, LoadCompleted. The memory service copies into and out of
// loadSegment(); DeviceModel calls applyLoadSegment() after the kernel took
// the LoadCompleted event, as below.

namespace {

void loadEvent(AssociationTableObject& table, uint8_t event) {
    const uint8_t data[] = {event};
    TEST_ASSERT_TRUE(writeAssociationProperty(table,
                                              static_cast<application::PropertyID>(AssociationTableProperty::LoadState),
                                              data).isOk());
}

void loadEvent(AddressTableObject& table, uint8_t event) {
    const uint8_t data[] = {event};
    TEST_ASSERT_TRUE(writeAddressProperty(table,
                                          static_cast<application::PropertyID>(AddressTableProperty::LoadState),
                                          data).isOk());
}

/// The memory service's write: a chunk copied into the segment at @p offset.
void memoryWrite(std::span<uint8_t> segment, size_t offset, std::span<const uint8_t> chunk) {
    TEST_ASSERT_TRUE(offset + chunk.size() <= segment.size());
    std::copy(chunk.begin(), chunk.end(), segment.begin() + static_cast<std::ptrdiff_t>(offset));
}

/// A segment image as ETS writes it: count, then 16-bit words.
std::vector<uint8_t> segmentImage(uint16_t count, const std::vector<uint16_t>& words) {
    std::vector<uint8_t> image{static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count & 0xFFu)};
    for (const uint16_t word : words) {
        image.push_back(static_cast<uint8_t>(word >> 8));
        image.push_back(static_cast<uint8_t>(word & 0xFFu));
    }
    return image;
}

void streamInChunks(std::span<uint8_t> segment, const std::vector<uint8_t>& image) {
    constexpr size_t kChunk = 12;
    for (size_t offset = 0; offset < image.size(); offset += kChunk) {
        const size_t length = std::min(kChunk, image.size() - offset);
        memoryWrite(segment, offset, std::span<const uint8_t>(image.data() + offset, length));
    }
}

bool sameOctets(const uint8_t* expected, const uint8_t* actual, size_t length) {
    return std::equal(expected, expected + length, actual);
}

std::vector<uint16_t> groupObjectsOf(const AssociationTableObject& table, uint16_t tsap) {
    std::vector<uint16_t> seen;
    table.forEachGroupObject(AddressTableIndex(tsap), [&](GroupObjectIndex go) { seen.push_back(go.value()); });
    return seen;
}

} // namespace

void test_AssociationTable_DownloadLikeEtsTakesEffectAtLoadCompleted(void) {
    AssociationTableObject table;
    // A table from an earlier download, in force.
    TEST_ASSERT_TRUE(table.addEntry(AssociationEntry(AddressTableIndex(9), GroupObjectIndex(9))).isOk());

    loadEvent(table, loadstate::kEventUnload);
    TEST_ASSERT_EQUAL_UINT16(0, table.entryCount());
    TEST_ASSERT_FALSE(table.hasGroupObjects(AddressTableIndex(9)));
    loadEvent(table, loadstate::kEventStartLoading);
    TEST_ASSERT_EQUAL_UINT8(loadstate::kLoading, table.loadState());

    // 7 entries (30 octets): the 12-octet chunks end mid-entry.
    const auto image = segmentImage(7, {1, 0,  1, 8,  2, 1,  3, 2,  3, 10,  4, 3,  5, 4});
    streamInChunks(table.loadSegment(), image);

    // Not in force before LoadCompleted...
    TEST_ASSERT_EQUAL_UINT16(0, table.entryCount());
    TEST_ASSERT_FALSE(table.hasGroupObjects(AddressTableIndex(1)));
    TEST_ASSERT_TRUE(table.persistenceRecord().empty());
    // ...but the verification read returns what was written, every octet.
    const auto segment = std::as_const(table).loadSegment();
    TEST_ASSERT_TRUE(sameOctets(image.data(), segment.data(), image.size()));

    loadEvent(table, loadstate::kEventLoadCompleted);
    const auto applied = table.applyLoadSegment();
    TEST_ASSERT_TRUE(applied.isOk());
    TEST_ASSERT_EQUAL_UINT16(7, applied.value());
    TEST_ASSERT_EQUAL_UINT8(loadstate::kLoaded, table.loadState());
    TEST_ASSERT_EQUAL_UINT16(7, table.entryCount());
    TEST_ASSERT_TRUE((groupObjectsOf(table, 1) == std::vector<uint16_t>{0, 8}));
    TEST_ASSERT_TRUE((groupObjectsOf(table, 3) == std::vector<uint16_t>{2, 10}));
    TEST_ASSERT_TRUE((groupObjectsOf(table, 5) == std::vector<uint16_t>{4}));
    TEST_ASSERT_FALSE(table.hasGroupObjects(AddressTableIndex(9)));
    AddressTableIndex addresses[2]{};
    TEST_ASSERT_EQUAL_UINT32(1, table.findAddressIndices(GroupObjectIndex(10), addresses).value());
    TEST_ASSERT_EQUAL_UINT16(3, addresses[0].value());

    // A read after LoadCompleted still returns the download.
    TEST_ASSERT_TRUE(sameOctets(image.data(), segment.data(), image.size()));

    // PID_TABLE reads what ETS wrote.
    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readAssociationProperty(table,
                                             static_cast<application::PropertyID>(AssociationTableProperty::TableData),
                                             7, value).isOk());
    TEST_ASSERT_EQUAL_UINT32(28, value.size());
    TEST_ASSERT_TRUE(sameOctets(image.data() + 2, value.data(), value.size()));
}

void test_AssociationTable_LoadDropsInvalidEntriesAndRepeats(void) {
    // The table a load leaves is the one adding the entries one by one built:
    // a repeat and an entry with an invalid TSAP (0) or ASAP (0xFFFF) dropped,
    // the order kept.
    AssociationTableObject table;
    loadEvent(table, loadstate::kEventStartLoading);
    streamInChunks(table.loadSegment(),
                   segmentImage(6, {1, 0,  0, 1,  2, 1,  1, 0,  3, 0xFFFF,  4, 3}));
    loadEvent(table, loadstate::kEventLoadCompleted);
    TEST_ASSERT_EQUAL_UINT16(3, table.applyLoadSegment().value());
    TEST_ASSERT_EQUAL_UINT16(3, table.entryCount());
    TEST_ASSERT_EQUAL_UINT16(1, table.getEntry(0)->addressIndex.value());
    TEST_ASSERT_EQUAL_UINT16(2, table.getEntry(1)->addressIndex.value());
    TEST_ASSERT_EQUAL_UINT16(4, table.getEntry(2)->addressIndex.value());
    TEST_ASSERT_EQUAL_UINT16(3, table.getEntry(2)->groupObjectNumber.value());
    const auto segment = std::as_const(table).loadSegment();
    TEST_ASSERT_EQUAL_UINT8(0, segment[0]);
    TEST_ASSERT_EQUAL_UINT8(3, segment[1]);  // the count follows the table in force
}

void test_AssociationTable_LoadOverCapacityIsRefused(void) {
    AssociationTableObject table;
    TEST_ASSERT_TRUE(table.addEntry(AssociationEntry(AddressTableIndex(1), GroupObjectIndex(1))).isOk());
    loadEvent(table, loadstate::kEventStartLoading);
    const auto tooMany = static_cast<uint16_t>(table.maxEntries() + 1u);
    memoryWrite(table.loadSegment(), 0, segmentImage(tooMany, {}));
    loadEvent(table, loadstate::kEventLoadCompleted);
    TEST_ASSERT_TRUE(table.applyLoadSegment().isError());
    TEST_ASSERT_EQUAL_UINT16(0, table.entryCount());
}

void test_AssociationTable_FirmwareAppendIsRefusedWhileLoading(void) {
    AssociationTableObject table;
    loadEvent(table, loadstate::kEventStartLoading);
    const auto image = segmentImage(2, {1, 0,  2, 1});
    streamInChunks(table.loadSegment(), image);

    // An append now would land on what ETS wrote.
    const auto refused = table.addEntry(AssociationEntry(AddressTableIndex(7), GroupObjectIndex(7)));
    TEST_ASSERT_TRUE(refused.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::Busy, refused.error());
    const AssociationEntry one[] = {AssociationEntry(AddressTableIndex(7), GroupObjectIndex(7))};
    TEST_ASSERT_TRUE(table.loadTable(one).isError());
    TEST_ASSERT_TRUE(sameOctets(image.data(), std::as_const(table).loadSegment().data(), image.size()));

    loadEvent(table, loadstate::kEventLoadCompleted);
    TEST_ASSERT_EQUAL_UINT16(2, table.applyLoadSegment().value());
    TEST_ASSERT_TRUE(table.addEntry(AssociationEntry(AddressTableIndex(7), GroupObjectIndex(7))).isOk());
    TEST_ASSERT_EQUAL_UINT16(3, table.entryCount());
}

void test_AssociationTable_PersistenceRecordIsThePidTableAndRestores(void) {
    AssociationTableObject table;
    for (uint16_t i = 0; i < 20; ++i) {
        TEST_ASSERT_TRUE(table.addEntry(AssociationEntry(AddressTableIndex(static_cast<uint16_t>(1 + i / 2)),
                                                         GroupObjectIndex(i))).isOk());
    }
    TEST_ASSERT_TRUE(table.removeEntry(3).isOk());
    TEST_ASSERT_EQUAL_UINT16(2, table.removeEntriesForGroupObject(GroupObjectIndex(0)) +
                                    table.removeEntriesForGroupObject(GroupObjectIndex(19)));

    // The record is what saving PID_TABLE through the kernel produced.
    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readAssociationProperty(table,
                                             static_cast<application::PropertyID>(AssociationTableProperty::TableData),
                                             table.entryCount(), value).isOk());
    const auto record = table.persistenceRecord();
    TEST_ASSERT_EQUAL_UINT32(value.size(), record.size());
    TEST_ASSERT_TRUE(sameOctets(value.data(), record.data(), value.size()));

    // The restore writes it back at index 1, as InterfaceObjectManager does.
    AssociationTableObject restored;
    TEST_ASSERT_TRUE(writeAssociationProperty(restored,
                                              static_cast<application::PropertyID>(AssociationTableProperty::TableData),
                                              record).isOk());
    TEST_ASSERT_EQUAL_UINT16(table.entryCount(), restored.entryCount());
    for (uint16_t i = 0; i < table.entryCount(); ++i) {
        TEST_ASSERT_TRUE(*table.getEntry(i) == *restored.getEntry(i));
    }
    // After a restore the segment reads as the table in force.
    const auto segment = std::as_const(restored).loadSegment();
    TEST_ASSERT_EQUAL_UINT8(table.entryCount(), segment[1]);
    TEST_ASSERT_TRUE(sameOctets(record.data(), segment.data() + 2, record.size()));

    // An empty table is an empty record (the save erases the stored copy).
    restored.clearEntries();
    TEST_ASSERT_TRUE(restored.persistenceRecord().empty());
}

void test_AddressTable_DownloadLikeEtsLeavesTheStagingAlone(void) {
    AddressTableObject table;
    TEST_ASSERT_TRUE(table.addEntry(GroupAddress(0x0901)).isValid());
    loadEvent(table, loadstate::kEventUnload);
    loadEvent(table, loadstate::kEventStartLoading);
    TEST_ASSERT_EQUAL_UINT16(0, table.entryCount());

    std::vector<uint16_t> addresses;
    for (uint16_t i = 0; i < 9; ++i) {
        addresses.push_back(static_cast<uint16_t>(0x0801 + i));
    }
    const auto image = segmentImage(9, addresses);
    streamInChunks(table.loadSegment(), image);

    // A save while ETS is loading saves the (empty) table and does not touch
    // what ETS wrote: the verification read still returns it.
    TEST_ASSERT_TRUE(table.persistenceRecord().empty());
    TEST_ASSERT_TRUE(sameOctets(image.data(), std::as_const(table).loadSegment().data(), image.size()));
}

void test_AddressTable_PersistenceRecordIsThePidTableAndRestores(void) {
    AddressTableObject table;
    for (uint16_t i = 0; i < 12; ++i) {
        TEST_ASSERT_TRUE(table.addEntry(GroupAddress(static_cast<uint16_t>(0x1100 + 3 * i))).isValid());
    }
    std::vector<uint8_t> value;
    TEST_ASSERT_TRUE(readAddressProperty(table,
                                         static_cast<application::PropertyID>(AddressTableProperty::TableData),
                                         table.entryCount(), value).isOk());
    const auto record = table.persistenceRecord();
    TEST_ASSERT_EQUAL_UINT32(value.size(), record.size());
    TEST_ASSERT_TRUE(sameOctets(value.data(), record.data(), value.size()));
    // The record lives in the segment, behind the count.
    const auto segment = std::as_const(table).loadSegment();
    TEST_ASSERT_EQUAL_UINT8(12, segment[1]);
    TEST_ASSERT_TRUE(segment.data() + 2 == record.data());

    AddressTableObject restored;
    TEST_ASSERT_TRUE(writeAddressProperty(restored,
                                          static_cast<application::PropertyID>(AddressTableProperty::TableData),
                                          record).isOk());
    TEST_ASSERT_EQUAL_UINT16(12, restored.entryCount());
    for (uint16_t i = 1; i <= 12; ++i) {
        TEST_ASSERT_EQUAL_UINT16(table.getAddress(AddressTableIndex(i)).raw,
                                 restored.getAddress(AddressTableIndex(i)).raw);
    }
}

// === Main Test Runner ===

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_AddressTable_CapacityIsAPromise);
    RUN_TEST(test_AssociationTable_CapacityIsAPromise);
    RUN_TEST(test_AddressTable_SortedTableFindsEveryEntryAndRejectsGaps);
    RUN_TEST(test_AddressTable_UnsortedTableStillFindsEntries);
    RUN_TEST(test_AssociationTable_LookupsFollowEveryMutationAndHaveNoPerAddressLimit);

    // The load segments: an ETS download, verification reads, persistence
    RUN_TEST(test_AssociationTable_DownloadLikeEtsTakesEffectAtLoadCompleted);
    RUN_TEST(test_AssociationTable_LoadDropsInvalidEntriesAndRepeats);
    RUN_TEST(test_AssociationTable_LoadOverCapacityIsRefused);
    RUN_TEST(test_AssociationTable_FirmwareAppendIsRefusedWhileLoading);
    RUN_TEST(test_AssociationTable_PersistenceRecordIsThePidTableAndRestores);
    RUN_TEST(test_AddressTable_DownloadLikeEtsLeavesTheStagingAlone);
    RUN_TEST(test_AddressTable_PersistenceRecordIsThePidTableAndRestores);

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
