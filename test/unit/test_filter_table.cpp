// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_filter_table.cpp
 * @brief Filter Table Realisation Type 3 bit field (03/05/01 §5.3.1).
 */

#include "knx/network/filter_table.hpp"
#include <unity.h>

#include <memory>

using namespace knx;
using namespace knx::network;

static std::unique_ptr<FilterTable> table;

void setUp() { table = std::make_unique<FilterTable>(); }
void tearDown() { table.reset(); }

void test_FilterTable_is_8k_bitfield_over_full_16_bit_space() {
    // §5.3.1.1.1: "65 536 bits and thus 8 192 octets".
    TEST_ASSERT_EQUAL_UINT32(8192u, FilterTable::kOctets);
    TEST_ASSERT_EQUAL_UINT32(8192u, table->octets().size());
}

void test_FilterTable_default_is_cleared() {
    // §4.5.3: "The default Load State shall be Loaded and the Routing Table cleared."
    TEST_ASSERT_EQUAL_UINT32(0u, table->passCount());
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x0801)));
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0xFFFF)));
}

void test_FilterTable_bit_layout_example_44() {
    // §5.3.1.1.1 EXAMPLE 44: GA 7/1879 = 3F57h -> octet 07EAh, bit 7.
    table->set(GroupAddress(0x3F57), true);
    TEST_ASSERT_EQUAL_HEX8(0x80, table->octets()[0x07EA]);
    TEST_ASSERT_EQUAL_UINT32(1u, table->passCount());
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x3F57)));
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x3F56)));
}

void test_FilterTable_bit_layout_example_43() {
    // §5.2.1.3 EXAMPLE 43: 01h at octet 1Dh of the table is GA 0/232 (E8h).
    table->octets()[0x1D] = 0x01;
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x00E8)));
    TEST_ASSERT_EQUAL_UINT32(1u, table->passCount());
}

void test_FilterTable_broadcast_address_is_never_evaluated() {
    // §5.3.1.1.2: bit 0 is "don't care" and "shall not be evaluated".
    table->set(GroupAddress(0x0000), true);
    TEST_ASSERT_EQUAL_HEX8(0x00, table->octets()[0]);
    table->setAll(true);
    TEST_ASSERT_EQUAL_HEX8(0xFE, table->octets()[0]);
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x0000)));
    TEST_ASSERT_EQUAL_UINT32(65535u, table->passCount());
    table->octets()[0] = 0xFF;  // even if written raw
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x0000)));
    TEST_ASSERT_EQUAL_UINT32(65535u, table->passCount());
}

void test_FilterTable_set_then_clear_then_set_same_address() {
    // The old first-match list kept blocking x after Clear(x) then Set(x).
    const GroupAddress x(0x0A05);
    table->setRange(0x0A05, 0x0A05, false);
    table->setRange(0x0A05, 0x0A05, true);
    TEST_ASSERT_TRUE(table->passes(x));
    table->setRange(0x0A00, 0x0AFF, false);
    TEST_ASSERT_FALSE(table->passes(x));
    table->setRange(0x0A05, 0x0A05, true);
    TEST_ASSERT_TRUE(table->passes(x));
}

void test_FilterTable_ranges_like_ets_writes() {
    // A line coupler download: main groups 1 and 3 whole, a few of 2, and the
    // odd-aligned 0/1/3..0/1/12.
    table->setRange(0x0800, 0x0FFF, true);   // 1/0/0 .. 1/7/255
    table->setRange(0x1801, 0x1FFF, true);   // 3/0/1 .. 3/7/255
    table->setRange(0x1003, 0x1007, true);   // 2/0/3 .. 2/0/7
    table->setRange(0x0103, 0x010C, true);   // 0/1/3 .. 0/1/12
    TEST_ASSERT_EQUAL_UINT32(2048u + 2047u + 5u + 10u, table->passCount());
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x07FF)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x0800)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x0FFF)));
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x1000)));
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x1800)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x1801)));
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x1002)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x1003)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x1007)));
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x1008)));
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x0102)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x0103)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x010C)));
    TEST_ASSERT_FALSE(table->passes(GroupAddress(0x010D)));
    TEST_ASSERT_TRUE(table->rangeIs(0x0800, 0x0FFF, true));
    TEST_ASSERT_FALSE(table->rangeIs(0x07FF, 0x0FFF, true));
    TEST_ASSERT_TRUE(table->rangeIs(0x1008, 0x17FF, false));
}

void test_FilterTable_full_address_space() {
    table->setRange(0x0001, 0xFFFF, true);
    TEST_ASSERT_EQUAL_UINT32(65535u, table->passCount());
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x0001)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x6FFF)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0x7000)));
    TEST_ASSERT_TRUE(table->passes(GroupAddress(0xFFFF)));
    TEST_ASSERT_TRUE(table->rangeIs(0x0000, 0xFFFF, true));

    table->setRange(0x7000, 0xFFFF, false);
    TEST_ASSERT_EQUAL_UINT32(0x6FFFu, table->passCount());
    TEST_ASSERT_TRUE(table->rangeIs(0x7000, 0xFFFF, false));
    TEST_ASSERT_TRUE(table->rangeIs(0x0001, 0x6FFF, true));

    table->setAll(false);
    TEST_ASSERT_EQUAL_UINT32(0u, table->passCount());
    TEST_ASSERT_TRUE(table->rangeIs(0x0000, 0xFFFF, false));
}

void test_FilterTable_every_alignment_of_a_short_range() {
    // Start and end at every bit position within and across octets.
    for (uint16_t first = 0x0100; first < 0x0110; ++first) {
        for (uint16_t last = first; last < 0x0120; ++last) {
            table->setAll(false);
            table->setRange(first, last, true);
            TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(last - first + 1u), table->passCount());
            TEST_ASSERT_FALSE(table->passes(GroupAddress(static_cast<uint16_t>(first - 1u))));
            TEST_ASSERT_TRUE(table->passes(GroupAddress(first)));
            TEST_ASSERT_TRUE(table->passes(GroupAddress(last)));
            TEST_ASSERT_FALSE(table->passes(GroupAddress(static_cast<uint16_t>(last + 1u))));
            TEST_ASSERT_TRUE(table->rangeIs(first, last, true));
        }
    }
}

void test_FilterTable_inverted_range_changes_nothing() {
    table->setRange(0x0200, 0x0100, true);
    TEST_ASSERT_EQUAL_UINT32(0u, table->passCount());
    TEST_ASSERT_TRUE(table->rangeIs(0x0200, 0x0100, true));  // vacuous
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_FilterTable_is_8k_bitfield_over_full_16_bit_space);
    RUN_TEST(test_FilterTable_default_is_cleared);
    RUN_TEST(test_FilterTable_bit_layout_example_44);
    RUN_TEST(test_FilterTable_bit_layout_example_43);
    RUN_TEST(test_FilterTable_broadcast_address_is_never_evaluated);
    RUN_TEST(test_FilterTable_set_then_clear_then_set_same_address);
    RUN_TEST(test_FilterTable_ranges_like_ets_writes);
    RUN_TEST(test_FilterTable_full_address_space);
    RUN_TEST(test_FilterTable_every_alignment_of_a_short_range);
    RUN_TEST(test_FilterTable_inverted_range_changes_nothing);
    return UNITY_END();
}
