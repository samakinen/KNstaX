// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_routing_table.cpp
 * @brief Unit tests for RoutingTable
 */

#include "knx/network/routing_table.hpp"
#include <unity.h>

using namespace knx::network;
using namespace knx;

static RoutingTable* table = nullptr;

void setUp() {
    table = new RoutingTable();
}

void tearDown() {
    delete table;
    table = nullptr;
}

// === Basic Entry Management Tests ===

void test_RoutingTable_AddEntry() {
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x1234), IndividualAddress(0xFF00), 6, EntryState::Enabled).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, table->getEntryCount());
}

void test_RoutingTable_RemoveEntry() {
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x1234), IndividualAddress(0xFF00), 6, EntryState::Enabled).isOk());
    TEST_ASSERT_TRUE(table->removeEntry(0).isOk());
    TEST_ASSERT_EQUAL_UINT8(0, table->getEntryCount());
}

void test_RoutingTable_GetEntry() {
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x1234), IndividualAddress(0xFF00), 6, EntryState::Enabled).isOk());
    const RoutingEntry* entry = table->getEntry(0);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT16(0x1234, entry->destination.raw);
    TEST_ASSERT_EQUAL_UINT16(0xFF00, entry->mask.raw);
    TEST_ASSERT_EQUAL_UINT8(6, entry->hopCount);
    TEST_ASSERT_TRUE(isEnabled(entry->enabled));
}

void test_RoutingTable_UpdateEntry() {
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x1234), IndividualAddress(0xFF00), 6, EntryState::Enabled).isOk());
    TEST_ASSERT_TRUE(table->updateEntry(0, IndividualAddress(0x5678), IndividualAddress(0xFFF0), 4, EntryState::Disabled).isOk());
    
    const RoutingEntry* entry = table->getEntry(0);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT16(0x5678, entry->destination.raw);
    TEST_ASSERT_EQUAL_UINT16(0xFFF0, entry->mask.raw);
    TEST_ASSERT_EQUAL_UINT8(4, entry->hopCount);
    TEST_ASSERT_FALSE(isEnabled(entry->enabled));
}

void test_RoutingTable_ClearTable() {
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x1234), IndividualAddress(0xFF00), 6, EntryState::Enabled).isOk());
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x5678), IndividualAddress(0xFFF0), 4, EntryState::Enabled).isOk());
    TEST_ASSERT_EQUAL_UINT8(2, table->getEntryCount());
    
    table->clear();
    TEST_ASSERT_EQUAL_UINT8(0, table->getEntryCount());
}

void test_RoutingTable_MaxEntriesLimit() {
    // Fill table to max
    for (uint16_t i = 0; i < 32; i++) {
        TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(static_cast<uint16_t>(0x1000 + i)), IndividualAddress(0xFFFF), 6, EntryState::Enabled).isOk());
    }
    TEST_ASSERT_EQUAL_UINT8(32, table->getEntryCount());
    
    // Try to add one more - should fail
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x2000), IndividualAddress(0xFFFF), 6, EntryState::Enabled).isError());
    TEST_ASSERT_EQUAL_UINT8(32, table->getEntryCount());
}

// === Routing Decision Tests ===

void test_RoutingTable_RouteWithZeroHopCount() {
    // Use actual API: route(source, dest, isGroup, hopCount, ownAddr)
    IndividualAddress source(0x1200);
    GroupAddress dest(0x1234);
    IndividualAddress ownAddr(0x1100);
    
    auto result = table->route(source, dest, AddressType::Individual, 0, ownAddr);
    TEST_ASSERT_EQUAL(RoutingDecision::Drop, result);
}

void test_RoutingTable_RouteGroupAddressAlwaysForward() {
    // Group addresses are always forwarded (filter table handles filtering)
    IndividualAddress source(0x1000);
    GroupAddress dest(0x0800);
    IndividualAddress ownAddr(0x0100);
    
    auto result = table->route(source, dest, AddressType::Group, 5, ownAddr);
    TEST_ASSERT_EQUAL(RoutingDecision::Forward, result);
}

void test_RoutingTable_RouteToSelf() {
    // Routing to own address should drop
    IndividualAddress source(0x1200);
    GroupAddress dest(0x1100);  // Same as ownAddr
    IndividualAddress ownAddr(0x1100);
    
    auto result = table->route(source, dest, AddressType::Individual, 5, ownAddr);
    TEST_ASSERT_EQUAL(RoutingDecision::Drop, result);
}

void test_RoutingTable_RouteIndividualAddressSameLine() {
    // Individual address on same line (area=1, line=1)
    IndividualAddress source(0x1200);
    GroupAddress dest(0x1105);  // area=1, line=1, device=5
    IndividualAddress ownAddr(0x1100);  // area=1, line=1, device=0
    
    auto result = table->route(source, dest, AddressType::Individual, 5, ownAddr);
    TEST_ASSERT_EQUAL(RoutingDecision::ForwardLocal, result);
}

void test_RoutingTable_RouteIndividualAddressSameArea() {
    // Individual address on same area but different line
    IndividualAddress source(0x1000);
    GroupAddress dest(0x1205);  // area=1, line=2, device=5
    IndividualAddress ownAddr(0x1100);  // area=1, line=1, device=0
    
    auto result = table->route(source, dest, AddressType::Individual, 5, ownAddr);
    TEST_ASSERT_EQUAL(RoutingDecision::Forward, result);
}

void test_RoutingTable_RouteCrossAreaWithEntry() {
    // Add routing entry for area 2
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x2000), IndividualAddress(0xF000), 6, EntryState::Enabled).isOk());
    
    // Route from area 1 to area 2 with sufficient hop count
    IndividualAddress source(0x1000);
    GroupAddress dest(0x2234);  // area=2, line=2, device=34
    IndividualAddress ownAddr(0x1100);  // area=1, line=1, device=0
    
    auto result = table->route(source, dest, AddressType::Individual, 5, ownAddr);
    TEST_ASSERT_EQUAL(RoutingDecision::Forward, result);
}

void test_RoutingTable_RouteCrossAreaBlockedByHopCount() {
    // Add routing entry with low hop count limit
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x2000), IndividualAddress(0xF000), 2, EntryState::Enabled).isOk());
    
    // Route with hop count exceeding limit
    IndividualAddress source(0x1000);
    GroupAddress dest(0x2234);
    IndividualAddress ownAddr(0x1100);
    
    auto result = table->route(source, dest, AddressType::Individual, 3, ownAddr);  // hopCount > entry->hopCount
    TEST_ASSERT_EQUAL(RoutingDecision::Drop, result);
}

void test_RoutingTable_RouteCrossAreaNoEntry() {
    // No routing entry for area 3 - should block
    IndividualAddress source(0x1000);
    GroupAddress dest(0x3234);  // area=3
    IndividualAddress ownAddr(0x1100);  // area=1
    
    auto result = table->route(source, dest, AddressType::Individual, 5, ownAddr);
    TEST_ASSERT_EQUAL(RoutingDecision::Block, result);
}

// === Helper Function Tests ===

void test_RoutingTable_IsOnSameLine() {
    TEST_ASSERT_TRUE(RoutingTable::isOnSameLine(IndividualAddress(0x1105), IndividualAddress(0x1100)));  // Same line
    TEST_ASSERT_FALSE(RoutingTable::isOnSameLine(IndividualAddress(0x1105), IndividualAddress(0x2100))); // Different line
    TEST_ASSERT_TRUE(RoutingTable::isOnSameLine(IndividualAddress(0x1105), IndividualAddress(0x1105)));  // Same address
}

void test_RoutingTable_IsOnSameArea() {
    TEST_ASSERT_TRUE(RoutingTable::isOnSameArea(IndividualAddress(0x1234), IndividualAddress(0x1500)));  // Same area (1)
    TEST_ASSERT_FALSE(RoutingTable::isOnSameArea(IndividualAddress(0x1234), IndividualAddress(0x2500))); // Different area
    TEST_ASSERT_TRUE(RoutingTable::isOnSameArea(IndividualAddress(0x1234), IndividualAddress(0x1234)));  // Same address
}

void test_RoutingTable_GroupAddressSameLineDetection() {
    // For group addresses, isOnSameLine checks individual address bits
    // This test verifies the helper function works correctly
    TEST_ASSERT_TRUE(RoutingTable::isOnSameLine(IndividualAddress(0x1105), IndividualAddress(0x1100)));  // Same area/line
    TEST_ASSERT_FALSE(RoutingTable::isOnSameLine(IndividualAddress(0x1205), IndividualAddress(0x1100))); // Different line
}

// === Serialization Tests ===

void test_RoutingTable_SaveAndLoadEmptyTable() {
    std::vector<uint8_t> buffer(256);
    auto res = table->saveTable(buffer);
    TEST_ASSERT_TRUE(res.isOk());
    buffer.resize(res.value());
    TEST_ASSERT_EQUAL_UINT32(2, buffer.size()); // Just entry count

    RoutingTable loaded;
    TEST_ASSERT_TRUE(loaded.loadTable(buffer).isOk());
    TEST_ASSERT_EQUAL_UINT8(0, loaded.getEntryCount());
}

void test_RoutingTable_SaveAndLoadWithEntries() {
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x1234), IndividualAddress(0xFF00), 6, EntryState::Enabled).isOk());
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x5678), IndividualAddress(0xFFF0), 4, EntryState::Disabled).isOk());
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0xABCD), IndividualAddress(0xF000), 2, EntryState::Enabled).isOk());
    
    std::vector<uint8_t> buffer(256);
    auto res = table->saveTable(buffer);
    TEST_ASSERT_TRUE(res.isOk());
    buffer.resize(res.value());
    TEST_ASSERT_EQUAL_UINT32(2 + 3 * 6, buffer.size()); // 2-byte count + 3 entries * 6 bytes

    RoutingTable loaded;
    TEST_ASSERT_TRUE(loaded.loadTable(buffer).isOk());
    TEST_ASSERT_EQUAL_UINT8(3, loaded.getEntryCount());
    
    // Verify first entry
    const RoutingEntry* entry0 = loaded.getEntry(0);
    TEST_ASSERT_NOT_NULL(entry0);
    TEST_ASSERT_EQUAL_UINT16(0x1234, entry0->destination.raw);
    TEST_ASSERT_EQUAL_UINT16(0xFF00, entry0->mask.raw);
    TEST_ASSERT_EQUAL_UINT8(6, entry0->hopCount);
    TEST_ASSERT_TRUE(isEnabled(entry0->enabled));
    
    // Verify second entry
    const RoutingEntry* entry1 = loaded.getEntry(1);
    TEST_ASSERT_NOT_NULL(entry1);
    TEST_ASSERT_EQUAL_UINT16(0x5678, entry1->destination.raw);
    TEST_ASSERT_EQUAL_UINT16(0xFFF0, entry1->mask.raw);
    TEST_ASSERT_EQUAL_UINT8(4, entry1->hopCount);
    TEST_ASSERT_FALSE(isEnabled(entry1->enabled));
}

void test_RoutingTable_LoadInvalidData() {
    std::vector<uint8_t> buffer = {0x00}; // Too short
    TEST_ASSERT_FALSE(table->loadTable(buffer).isOk());
}

// === Edge Cases ===

void test_RoutingTable_InvalidEntryIndex() {
    const RoutingEntry* entry = table->getEntry(99);
    TEST_ASSERT_NULL(entry);
}

void test_RoutingTable_RemoveInvalidIndex() {
    TEST_ASSERT_FALSE(table->removeEntry(0).isOk());  // Empty table
    TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x1234), IndividualAddress(0xFF00), 6, EntryState::Enabled).isOk());
    TEST_ASSERT_FALSE(table->removeEntry(5).isOk());  // Out of range
}

void test_RoutingTable_ComplexRoutingScenario() {
    // Set up routing table for a line coupler at area 1, line 0
        TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x1000), IndividualAddress(0xF000), 6, EntryState::Enabled).isOk());  // Area 1, max 6 hops
        TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x2000), IndividualAddress(0xF000), 4, EntryState::Enabled).isOk());  // Area 2, max 4 hops
        TEST_ASSERT_TRUE(table->addEntry(IndividualAddress(0x3000), IndividualAddress(0xF000), 2, EntryState::Enabled).isOk());  // Area 3, max 2 hops (restricted)
    
    IndividualAddress source(0x0100);
    IndividualAddress ownAddr(0x1000);  // Own address in area 1
    
    // Route within same area - should forward
    GroupAddress dest1(0x1234);
    TEST_ASSERT_EQUAL(RoutingDecision::Forward, 
                     table->route(source, dest1, AddressType::Individual, 5, ownAddr));
    
    // Route to area 2 with sufficient hops
    GroupAddress dest2(0x2234);
    TEST_ASSERT_EQUAL(RoutingDecision::Forward, 
                     table->route(source, dest2, AddressType::Individual, 4, ownAddr));
    
    // Route to area 3 with hop count exceeding limit - should drop
    GroupAddress dest3(0x3234);
    TEST_ASSERT_EQUAL(RoutingDecision::Drop, 
                     table->route(source, dest3, AddressType::Individual, 3, ownAddr));
    
    // Route to area 3 with acceptable hop count
    TEST_ASSERT_EQUAL(RoutingDecision::Forward, 
                     table->route(source, dest3, AddressType::Individual, 2, ownAddr));
}

int main() {
    UNITY_BEGIN();
    
    // Basic entry management
    RUN_TEST(test_RoutingTable_AddEntry);
    RUN_TEST(test_RoutingTable_RemoveEntry);
    RUN_TEST(test_RoutingTable_GetEntry);
    RUN_TEST(test_RoutingTable_UpdateEntry);
    RUN_TEST(test_RoutingTable_ClearTable);
    RUN_TEST(test_RoutingTable_MaxEntriesLimit);
    
    // Routing decisions
    RUN_TEST(test_RoutingTable_RouteWithZeroHopCount);
    RUN_TEST(test_RoutingTable_RouteGroupAddressAlwaysForward);
    RUN_TEST(test_RoutingTable_RouteToSelf);
    RUN_TEST(test_RoutingTable_RouteIndividualAddressSameLine);
    RUN_TEST(test_RoutingTable_RouteIndividualAddressSameArea);
    RUN_TEST(test_RoutingTable_RouteCrossAreaWithEntry);
    RUN_TEST(test_RoutingTable_RouteCrossAreaBlockedByHopCount);
    RUN_TEST(test_RoutingTable_RouteCrossAreaNoEntry);
    
    // Helper functions
    RUN_TEST(test_RoutingTable_IsOnSameLine);
    RUN_TEST(test_RoutingTable_IsOnSameArea);
    RUN_TEST(test_RoutingTable_GroupAddressSameLineDetection);
    
    // Serialization
    RUN_TEST(test_RoutingTable_SaveAndLoadEmptyTable);
    RUN_TEST(test_RoutingTable_SaveAndLoadWithEntries);
    RUN_TEST(test_RoutingTable_LoadInvalidData);
    
    // Edge cases
    RUN_TEST(test_RoutingTable_InvalidEntryIndex);
    RUN_TEST(test_RoutingTable_RemoveInvalidIndex);
    RUN_TEST(test_RoutingTable_ComplexRoutingScenario);
    
    return UNITY_END();
}

