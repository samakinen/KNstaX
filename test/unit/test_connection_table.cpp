// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_connection_table.cpp
 * @brief Unit tests for connection table
 */

#include "unity.h"
#include "knx/transport/connection_table.hpp"

using namespace knx::transport;
using namespace knx;

static ConnectionTable table;

void setUp(void) {
    TEST_ASSERT_TRUE(table.init(16).isOk());
}

void tearDown(void) {
    table.clear();
}

void test_CanCreateConnection(void) {
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x5678);
    params.timeoutMs = 3000;
    params.maxRetries = 3;
    
    ConnectionIndex index = table.createConnection(IndividualAddress(0x5678), params);
    TEST_ASSERT_EQUAL_INT8(0, index.value());
    ConnectionEntry* entry = table.getConnection(index);
    TEST_ASSERT_NOT_NULL(entry);
    TEST_ASSERT_EQUAL_HEX16(0x5678, entry->remoteAddress.raw);
}

void test_RejectsInvalidAddress(void) {
    ConnectionParams params;
    params.timeoutMs = 3000;
    
    ConnectionIndex index = table.createConnection(IndividualAddress(0xFFFF), params);
    TEST_ASSERT_FALSE(index.isValid());
}

void test_RejectsDuplicates(void) {
    ConnectionParams params;
    params.timeoutMs = 3000;
    
    ConnectionIndex idx1 = table.createConnection(IndividualAddress(0x1234), params);
    TEST_ASSERT_EQUAL_INT8(0, idx1.value());
    
    ConnectionIndex idx2 = table.createConnection(IndividualAddress(0x1234), params);
    TEST_ASSERT_FALSE(idx2.isValid());
}

void test_RetrievalAndLookup(void) {
    ConnectionParams params;
    params.timeoutMs = 3000;
    
    ConnectionIndex index = table.createConnection(IndividualAddress(0x1234), params);
    TEST_ASSERT_EQUAL_INT8(0, index.value());
    
    ConnectionEntry* entry = table.getConnection(index);
    TEST_ASSERT(entry != nullptr);
    TEST_ASSERT_EQUAL(0x1234, entry->remoteAddress.raw);
    
    const ConnectionEntry* found = table.findConnection(IndividualAddress(0x1234));
    TEST_ASSERT(found != nullptr);
    TEST_ASSERT_EQUAL(0x1234, found->remoteAddress.raw);
}

void test_RemovalOperations(void) {
    ConnectionParams params;
    params.timeoutMs = 3000;
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    TEST_ASSERT_EQUAL_INT8(0, idx.value());
    
    // Remove by index
    TEST_ASSERT_TRUE(table.removeConnection(idx).isOk());
    TEST_ASSERT(table.getConnection(idx) == nullptr);
    
    // Create again for address-based removal
    idx = table.createConnection(IndividualAddress(0x1234), params);
    TEST_ASSERT_EQUAL_INT8(1, idx.value());
    
    // Remove by address
    TEST_ASSERT_TRUE(table.removeConnectionByAddress(IndividualAddress(0x1234)).isOk());
    TEST_ASSERT(table.findConnection(IndividualAddress(0x1234)) == nullptr);
}

void test_MultipleConnections(void) {
    ConnectionParams params;
    params.timeoutMs = 3000;
    
    ConnectionIndex idx1 = table.createConnection(IndividualAddress(0x1000), params);
    ConnectionIndex idx2 = table.createConnection(IndividualAddress(0x2000), params);
    ConnectionIndex idx3 = table.createConnection(IndividualAddress(0x3000), params);
    
    TEST_ASSERT_EQUAL_INT8(0, idx1.value());
    TEST_ASSERT_EQUAL_INT8(1, idx2.value());
    TEST_ASSERT_EQUAL_INT8(2, idx3.value());
    TEST_ASSERT_NOT_EQUAL(idx1.value(), idx2.value());
    TEST_ASSERT_NOT_EQUAL(idx2.value(), idx3.value());
}

void test_TimeoutDetection(void) {
    ConnectionParams params;
    params.timeoutMs = 1000;
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    TEST_ASSERT_EQUAL_INT8(0, idx.value());
    
    // Set activity time to 100ms, check at time 2000ms (elapsed = 1900ms > 1000ms timeout)
    table.updateActivityTime(idx, 100);
    size_t timedOut = table.checkTimeouts(2000, 1000);
    
    TEST_ASSERT_EQUAL(1, timedOut);
    TEST_ASSERT(table.findConnection(IndividualAddress(0x1234)) == nullptr);
}

void test_ActivityTracking(void) {
    ConnectionParams params;
    params.timeoutMs = 3000;
    params.initialSequence = 10;
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    TEST_ASSERT_EQUAL_INT8(0, idx.value());
    
    table.updateActivityTime(idx, 500);
    
    ConnectionEntry* entry = table.getConnection(idx);
    TEST_ASSERT(entry != nullptr);
    TEST_ASSERT_EQUAL(500, entry->lastActivityTimeMs);
    TEST_ASSERT_EQUAL(10, entry->sequenceNumber);
}

// --- Device connection bound ------------------------------------------------

void test_DefaultsToASingleConnection(void) {
    // KNX 03/03/04: "This state machine is designed for only one connection at
    // a time." An end device has one. Accepting a second would let two
    // management clients interleave downloads into the same device, each
    // believing it had exclusive access — commissioning corrupted in silence.
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init().isOk());
    TEST_ASSERT_EQUAL(1u, table.maxConnections());

    ConnectionParams params{};
    const auto first = table.createConnection(IndividualAddress(1, 1, 1), params);
    TEST_ASSERT_TRUE(first.isValid());

    // A different peer while the first still holds the device.
    const auto second = table.createConnection(IndividualAddress(1, 1, 2), params);
    TEST_ASSERT_FALSE(second.isValid());
    TEST_ASSERT_EQUAL(1u, table.activeConnectionCount());
}

void test_CouplersCanRaiseTheBound(void) {
    // Couplers and IP interfaces legitimately multiplex, so the bound is a
    // default rather than a hard limit.
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());

    ConnectionParams params{};
    for (uint8_t device = 1; device <= 4; ++device) {
        TEST_ASSERT_TRUE(table.createConnection(IndividualAddress(1, 1, device), params).isValid());
    }
    TEST_ASSERT_EQUAL(4u, table.activeConnectionCount());
    TEST_ASSERT_FALSE(table.createConnection(IndividualAddress(1, 1, 5), params).isValid());
}

void test_SlotIsReusableAfterRelease(void) {
    // The bound must not be a one-shot: once the first client disconnects, the
    // next one has to be able to commission the device.
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init().isOk());

    ConnectionParams params{};
    const auto first = table.createConnection(IndividualAddress(1, 1, 1), params);
    TEST_ASSERT_TRUE(first.isValid());
    TEST_ASSERT_TRUE(table.removeConnection(first).isOk());

    TEST_ASSERT_TRUE(table.createConnection(IndividualAddress(1, 1, 2), params).isValid());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_DefaultsToASingleConnection);
    RUN_TEST(test_CouplersCanRaiseTheBound);
    RUN_TEST(test_SlotIsReusableAfterRelease);
    
    RUN_TEST(test_CanCreateConnection);
    RUN_TEST(test_RejectsInvalidAddress);
    RUN_TEST(test_RejectsDuplicates);
    RUN_TEST(test_RetrievalAndLookup);
    RUN_TEST(test_RemovalOperations);
    RUN_TEST(test_MultipleConnections);
    RUN_TEST(test_TimeoutDetection);
    RUN_TEST(test_ActivityTracking);
    
    return UNITY_END();
}

