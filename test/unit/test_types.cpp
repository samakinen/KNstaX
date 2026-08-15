// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_types.cpp
 * @brief Unit tests for KNX type system
 */

#include "unity.h"
#include "knx/types.hpp"

using namespace knx;

void setUp(void) {
    // Set up code here
}

void tearDown(void) {
    // Clean up code here
}

// IndividualAddress tests
void test_individual_address_creation(void) {
    IndividualAddress addr(1, 2, 3);
    TEST_ASSERT_EQUAL(1, addr.area());
    TEST_ASSERT_EQUAL(2, addr.line());
    TEST_ASSERT_EQUAL(3, addr.device());
}

void test_individual_address_from_raw(void) {
    uint16_t raw = (1 << 12) | (2 << 8) | 3;  // 0x1203
    IndividualAddress addr(raw);
    TEST_ASSERT_EQUAL(1, addr.area());
    TEST_ASSERT_EQUAL(2, addr.line());
    TEST_ASSERT_EQUAL(3, addr.device());
}

void test_individual_address_raw_value(void) {
    IndividualAddress addr(1, 2, 3);
    uint16_t expected = (1 << 12) | (2 << 8) | 3;
    TEST_ASSERT_EQUAL(expected, addr.raw);
}

void test_individual_address_comparison(void) {
    IndividualAddress addr1(1, 2, 3);
    IndividualAddress addr2(1, 2, 3);
    IndividualAddress addr3(1, 2, 4);
    
    TEST_ASSERT_TRUE(addr1 == addr2);
    TEST_ASSERT_FALSE(addr1 == addr3);
}

void test_individual_address_classification(void) {
    IndividualAddress defaultAddr;
    TEST_ASSERT_EQUAL_UINT16(0x0000, defaultAddr.raw);
    TEST_ASSERT_FALSE(defaultAddr.isValid());
    TEST_ASSERT_EQUAL(static_cast<int>(IndividualAddressKind::Invalid),
                      static_cast<int>(classifyIndividualAddress(IndividualAddress(0x0000))));
    TEST_ASSERT_EQUAL(static_cast<int>(IndividualAddressKind::Initial),
                      static_cast<int>(classifyIndividualAddress(initialIndividualAddress())));
    TEST_ASSERT_EQUAL(static_cast<int>(IndividualAddressKind::Operational),
                      static_cast<int>(classifyIndividualAddress(IndividualAddress(1, 2, 3))));
    TEST_ASSERT_TRUE(isOperationalIndividualAddress(IndividualAddress(1, 2, 3)));
    TEST_ASSERT_FALSE(isOperationalIndividualAddress(initialIndividualAddress()));
}

// GroupAddress tests (3-level)
void test_group_address_3level_creation(void) {
    GroupAddress addr(5, 3, 7);
    TEST_ASSERT_EQUAL(5, addr.main());
    TEST_ASSERT_EQUAL(3, addr.middle());
    TEST_ASSERT_EQUAL(7, addr.sub());
}

void test_group_address_3level_from_raw(void) {
    uint16_t raw = (5 << 11) | (3 << 8) | 7;  // 0xA807
    GroupAddress addr(raw);
    TEST_ASSERT_EQUAL(5, addr.main());
    TEST_ASSERT_EQUAL(3, addr.middle());
    TEST_ASSERT_EQUAL(7, addr.sub());
}

void test_group_address_3level_raw_value(void) {
    GroupAddress addr(5, 3, 7);
    uint16_t expected = (5 << 11) | (3 << 8) | 7;
    TEST_ASSERT_EQUAL(expected, addr.raw);
}

void test_strong_value_invalid_support(void) {
    auto invalidIndex = GroupObjectIndex::invalid();
    auto validIndex = GroupObjectIndex(7);

    TEST_ASSERT_FALSE(invalidIndex.isValid());
    TEST_ASSERT_TRUE(validIndex.isValid());
    TEST_ASSERT_TRUE(validIndex < invalidIndex);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, invalidIndex.raw);
}

// Priority tests
void test_priority_enum(void) {
    // Values are the 2-bit wire codes (03_02_02 §2.3.2 / cEMI ctrl1):
    // 00=system, 01=normal, 10=urgent, 11=low.
    TEST_ASSERT_EQUAL(0, static_cast<int>(Priority::System));
    TEST_ASSERT_EQUAL(1, static_cast<int>(Priority::Normal));
    TEST_ASSERT_EQUAL(2, static_cast<int>(Priority::Urgent));
    TEST_ASSERT_EQUAL(3, static_cast<int>(Priority::Low));
}

// MessageCode tests
void test_message_code_enum(void) {
    TEST_ASSERT_EQUAL_HEX8(0x11, static_cast<uint8_t>(MessageCode::LData));
    TEST_ASSERT_EQUAL_HEX8(0x12, static_cast<uint8_t>(MessageCode::LRaw));
}

// Test runner
int run_all_tests(void) {
    UNITY_BEGIN();
    
    // IndividualAddress tests
    RUN_TEST(test_individual_address_creation);
    RUN_TEST(test_individual_address_from_raw);
    RUN_TEST(test_individual_address_raw_value);
    RUN_TEST(test_individual_address_comparison);
    RUN_TEST(test_individual_address_classification);
    
    // GroupAddress tests
    RUN_TEST(test_group_address_3level_creation);
    RUN_TEST(test_group_address_3level_from_raw);
    RUN_TEST(test_group_address_3level_raw_value);
    RUN_TEST(test_strong_value_invalid_support);
    
    // Enum tests
    RUN_TEST(test_priority_enum);
    RUN_TEST(test_message_code_enum);
    
    return UNITY_END();
}

int main() {
    return run_all_tests();
}
