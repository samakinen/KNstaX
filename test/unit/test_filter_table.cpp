// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_filter_table.cpp
 * @brief Unit tests for FilterTable
 */

#include "knx/network/filter_table.hpp"
#include <unity.h>

using namespace knx::network;

static FilterTable* table = nullptr;

void setUp() {
    table = new FilterTable();
}

void tearDown() {
    delete table;
    table = nullptr;
}

// === Basic Entry Management Tests ===

void test_FilterTable_AddEntry() {
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFFFF), FilterAction::Allow, knx::EntryState::Enabled).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, table->entryCount());
}

void test_FilterTable_RemoveEntry() {
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFFFF), FilterAction::Allow, knx::EntryState::Enabled).isOk());
    TEST_ASSERT_TRUE(table->removeEntry(0).isOk());
    TEST_ASSERT_EQUAL_UINT8(0, table->entryCount());
}

void test_FilterTable_GetEntry() {
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFF00), FilterAction::Block, knx::EntryState::Enabled).isOk());
    const FilterEntry* entry = table->getEntry(0);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT16(0x1234, entry->address.raw);
    TEST_ASSERT_EQUAL_UINT16(0xFF00, entry->mask.raw);
    TEST_ASSERT_EQUAL(FilterAction::Block, entry->action);
    TEST_ASSERT_TRUE(knx::isEnabled(entry->enabled));
}

void test_FilterTable_UpdateEntry() {
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFF00), FilterAction::Allow, knx::EntryState::Enabled).isOk());
    TEST_ASSERT_TRUE(table->updateEntry(0, knx::GroupAddress(0x5678), knx::GroupAddress(0xFFF0), FilterAction::Block, knx::EntryState::Disabled).isOk());
    
    const FilterEntry* entry = table->getEntry(0);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_UINT16(0x5678, entry->address.raw);
    TEST_ASSERT_EQUAL_UINT16(0xFFF0, entry->mask.raw);
    TEST_ASSERT_EQUAL(FilterAction::Block, entry->action);
    TEST_ASSERT_FALSE(knx::isEnabled(entry->enabled));
}

void test_FilterTable_ClearTable() {
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFF00), FilterAction::Allow, knx::EntryState::Enabled).isOk());
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x5678), knx::GroupAddress(0xFFF0), FilterAction::Block, knx::EntryState::Enabled).isOk());
    TEST_ASSERT_EQUAL_UINT8(2, table->entryCount());
    
    table->clear();
    TEST_ASSERT_EQUAL_UINT8(0, table->entryCount());
}

void test_FilterTable_MaxEntriesLimit() {
    // Fill table to max (64 entries)
    for (uint16_t i = 0; i < 64; i++) {
        TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(static_cast<uint16_t>(0x1000 + i)), knx::GroupAddress(0xFFFF), FilterAction::Allow, knx::EntryState::Enabled).isOk());
    }
    TEST_ASSERT_EQUAL_UINT8(64, table->entryCount());
    
    // Try to add one more - should fail
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x2000), knx::GroupAddress(0xFFFF), FilterAction::Allow, knx::EntryState::Enabled).isError());
    TEST_ASSERT_EQUAL_UINT8(64, table->entryCount());
}

// === Default Action Tests ===

void test_FilterTable_DefaultActionAllow() {
    table->setDefaultAction(FilterAction::Allow);
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->defaultAction());
}

void test_FilterTable_DefaultActionBlock() {
    table->setDefaultAction(FilterAction::Block);
    TEST_ASSERT_EQUAL(FilterAction::Block, table->defaultAction());
}

// === Filter Checking Tests ===

void test_FilterTable_CheckFilterWithEmptyTable() {
    // Empty table uses default action
    table->setDefaultAction(FilterAction::Allow);
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->checkFilter(knx::GroupAddress(0x1234)));
    
    table->setDefaultAction(FilterAction::Block);
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x1234)));
}

void test_FilterTable_CheckFilterExactMatch() {
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFFFF), FilterAction::Block, knx::EntryState::Enabled).isOk());
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x1234)));
}

void test_FilterTable_CheckFilterWithMask() {
    // Block all addresses in range 0x1200-0x12FF
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1200), knx::GroupAddress(0xFF00), FilterAction::Block, knx::EntryState::Enabled).isOk());
    
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x1200)));
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x1234)));
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x12FF)));
    
    // Address outside range should use default
    table->setDefaultAction(FilterAction::Allow);
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->checkFilter(knx::GroupAddress(0x1300)));
}

void test_FilterTable_CheckFilterDisabledEntry() {
    table->setDefaultAction(FilterAction::Allow);
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFFFF), FilterAction::Block, knx::EntryState::Disabled).isOk());  // Disabled
    
    // Should use default action, not the disabled entry
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->checkFilter(knx::GroupAddress(0x1234)));
}

void test_FilterTable_CheckFilterFirstMatch() {
    table->setDefaultAction(FilterAction::Allow);
    
    // Add overlapping entries - first match wins
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1200), knx::GroupAddress(0xFF00), FilterAction::Block, knx::EntryState::Enabled).isOk());  // Block 0x1200-0x12FF
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFFFF), FilterAction::Allow, knx::EntryState::Enabled).isOk());  // Allow 0x1234
    
    // First entry matches first, so should block
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x1234)));
}

void test_FilterTable_CheckFilterMultipleRanges() {
    table->setDefaultAction(FilterAction::Allow);
    
    // Block multiple ranges
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1000), knx::GroupAddress(0xF000), FilterAction::Block, knx::EntryState::Enabled).isOk());  // Block area 1
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x2000), knx::GroupAddress(0xF000), FilterAction::Block, knx::EntryState::Enabled).isOk());  // Block area 2
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x3500), knx::GroupAddress(0xFF00), FilterAction::Allow, knx::EntryState::Enabled).isOk());  // Allow 0x3500-0x35FF
    
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x1234)));  // Area 1
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x2567)));  // Area 2
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->checkFilter(knx::GroupAddress(0x3550)));  // Explicit allow
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->checkFilter(knx::GroupAddress(0x4000)));  // Default
}

// === Serialization Tests ===

void test_FilterTable_SaveAndLoadEmptyTable() {
    table->setDefaultAction(FilterAction::Block);
    std::vector<uint8_t> buffer(256);
    auto res = table->saveTable(buffer);
    TEST_ASSERT_TRUE(res.isOk());
    buffer.resize(res.value());
    TEST_ASSERT_EQUAL_UINT32(3, buffer.size()); // 1-byte default + 2-byte count

    FilterTable loaded;
    TEST_ASSERT_TRUE(loaded.loadTable(buffer).isOk());
    TEST_ASSERT_EQUAL_UINT8(0, loaded.entryCount());
    TEST_ASSERT_EQUAL(FilterAction::Block, loaded.defaultAction());
}

void test_FilterTable_SaveAndLoadWithEntries() {
    table->setDefaultAction(FilterAction::Allow);
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFF00), FilterAction::Block, knx::EntryState::Enabled).isOk());
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x5678), knx::GroupAddress(0xFFF0), FilterAction::Allow, knx::EntryState::Disabled).isOk());
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0xABCD), knx::GroupAddress(0xF000), FilterAction::Block, knx::EntryState::Enabled).isOk());
    
    std::vector<uint8_t> buffer(256);
    auto res = table->saveTable(buffer);
    TEST_ASSERT_TRUE(res.isOk());
    buffer.resize(res.value());
    TEST_ASSERT_EQUAL_UINT32(3 + 3 * 5, buffer.size()); // 3-byte header + 3 entries * 5 bytes

    FilterTable loaded;
    TEST_ASSERT_TRUE(loaded.loadTable(buffer).isOk());
    TEST_ASSERT_EQUAL_UINT8(3, loaded.entryCount());
    TEST_ASSERT_EQUAL(FilterAction::Allow, loaded.defaultAction());
    
    // Verify first entry
    const FilterEntry* entry0 = loaded.getEntry(0);
    TEST_ASSERT_NOT_NULL(entry0);
    TEST_ASSERT_EQUAL_UINT16(0x1234, entry0->address.raw);
    TEST_ASSERT_EQUAL_UINT16(0xFF00, entry0->mask.raw);
    TEST_ASSERT_EQUAL(FilterAction::Block, entry0->action);
    TEST_ASSERT_TRUE(knx::isEnabled(entry0->enabled));
}

void test_FilterTable_LoadInvalidData() {
    std::vector<uint8_t> buffer = {0x00}; // Too short
    TEST_ASSERT_FALSE(table->loadTable(buffer).isOk());
}

// === Practical Scenarios ===

void test_FilterTable_WhitelistScenario() {
    // Default deny, allow specific addresses
    table->setDefaultAction(FilterAction::Block);
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFFFF), FilterAction::Allow, knx::EntryState::Enabled).isOk());  // Allow specific device
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x5600), knx::GroupAddress(0xFF00), FilterAction::Allow, knx::EntryState::Enabled).isOk());  // Allow range
    
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->checkFilter(knx::GroupAddress(0x1234)));  // Whitelisted device
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->checkFilter(knx::GroupAddress(0x5678)));  // In whitelisted range
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x9999)));  // Not in whitelist
}

void test_FilterTable_BlacklistScenario() {
    // Default allow, block specific addresses
    table->setDefaultAction(FilterAction::Allow);
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFFFF), FilterAction::Block, knx::EntryState::Enabled).isOk());  // Block specific device
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x2000), knx::GroupAddress(0xF000), FilterAction::Block, knx::EntryState::Enabled).isOk());  // Block entire area
    
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x1234)));  // Blacklisted device
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x2567)));  // In blacklisted area
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->checkFilter(knx::GroupAddress(0x3456)));  // Not blacklisted
}

void test_FilterTable_GroupAddressFiltering() {
    table->setDefaultAction(FilterAction::Allow);
    
    // Block specific group address ranges
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x0800), knx::GroupAddress(0xF800), FilterAction::Block, knx::EntryState::Enabled).isOk());  // Block main group 1
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1000), knx::GroupAddress(0xF800), FilterAction::Block, knx::EntryState::Enabled).isOk());  // Block main group 2
    
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x0801)));  // Main group 1
    TEST_ASSERT_EQUAL(FilterAction::Block, table->checkFilter(knx::GroupAddress(0x1002)));  // Main group 2
    TEST_ASSERT_EQUAL(FilterAction::Allow, table->checkFilter(knx::GroupAddress(0x1801)));  // Main group 3
}

// === Edge Cases ===

void test_FilterTable_InvalidEntryIndex() {
    const FilterEntry* entry = table->getEntry(99);
    TEST_ASSERT_NULL(entry);
}

void test_FilterTable_RemoveInvalidIndex() {
    TEST_ASSERT_FALSE(table->removeEntry(0).isOk());  // Empty table
    TEST_ASSERT_TRUE(table->addEntry(knx::GroupAddress(0x1234), knx::GroupAddress(0xFF00), FilterAction::Allow, knx::EntryState::Enabled).isOk());
    TEST_ASSERT_FALSE(table->removeEntry(5).isOk());  // Out of range
}

int main() {
    UNITY_BEGIN();
    
    // Basic entry management
    RUN_TEST(test_FilterTable_AddEntry);
    RUN_TEST(test_FilterTable_RemoveEntry);
    RUN_TEST(test_FilterTable_GetEntry);
    RUN_TEST(test_FilterTable_UpdateEntry);
    RUN_TEST(test_FilterTable_ClearTable);
    RUN_TEST(test_FilterTable_MaxEntriesLimit);
    
    // Default action
    RUN_TEST(test_FilterTable_DefaultActionAllow);
    RUN_TEST(test_FilterTable_DefaultActionBlock);
    
    // Filter checking
    RUN_TEST(test_FilterTable_CheckFilterWithEmptyTable);
    RUN_TEST(test_FilterTable_CheckFilterExactMatch);
    RUN_TEST(test_FilterTable_CheckFilterWithMask);
    RUN_TEST(test_FilterTable_CheckFilterDisabledEntry);
    RUN_TEST(test_FilterTable_CheckFilterFirstMatch);
    RUN_TEST(test_FilterTable_CheckFilterMultipleRanges);
    
    // Serialization
    RUN_TEST(test_FilterTable_SaveAndLoadEmptyTable);
    RUN_TEST(test_FilterTable_SaveAndLoadWithEntries);
    RUN_TEST(test_FilterTable_LoadInvalidData);
    
    // Practical scenarios
    RUN_TEST(test_FilterTable_WhitelistScenario);
    RUN_TEST(test_FilterTable_BlacklistScenario);
    RUN_TEST(test_FilterTable_GroupAddressFiltering);
    
    // Edge cases
    RUN_TEST(test_FilterTable_InvalidEntryIndex);
    RUN_TEST(test_FilterTable_RemoveInvalidIndex);
    
    return UNITY_END();
}
