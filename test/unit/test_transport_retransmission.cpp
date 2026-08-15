// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_transport_retransmission.cpp
 * @brief Unit tests for transport layer retransmission
 */

#include "knx/transport/connection_table.hpp"
#include "knx/transport/connection_state.hpp"
#include <unity.h>
#include <vector>

using namespace knx::transport;
using namespace knx;

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// Retransmission timing and backoff
// ============================================================================

void test_RetransmissionTimeout(void) {
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());
    
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.initialSequence = 0;
    params.maxRetries = 3;
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    TEST_ASSERT_TRUE(idx.isValid());
    
    auto* entry = table.getConnection(idx);
    TEST_ASSERT_NOT_NULL(entry);
    
    // Set up pending retransmission
    entry->pendingRetransmission = true;
    entry->lastTxTimeMs = 1000;
    entry->retransmitTimeoutMs = 3000;
    entry->retransmitCount = 0;
    
    // Check at 2000ms - should not trigger (timeout at 4000ms)
    auto retransmits = table.processRetransmissions(2000);
    TEST_ASSERT_EQUAL(0, retransmits.size());
    
    // Check at 4500ms - should trigger retransmission
    retransmits = table.processRetransmissions(4500);
    TEST_ASSERT_EQUAL(1, retransmits.size());
    TEST_ASSERT_EQUAL(idx.value(), retransmits[0].value());
    
    // Simulate actually sending the retransmission
    (void)table.markDataSent(idx);
    entry->lastTxTimeMs = 4500;  // Update last TX time
    
    // Timeout should have doubled (3000 -> 6000)
    entry = table.getConnection(idx);
    TEST_ASSERT_EQUAL(6000, entry->retransmitTimeoutMs);
    TEST_ASSERT_EQUAL(1, entry->retransmitCount);
}

void test_ExponentialBackoff(void) {
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());
    
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.initialSequence = 0;
    params.maxRetries = 3;
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    auto* entry = table.getConnection(idx);
    
    entry->pendingRetransmission = true;
    entry->lastTxTimeMs = 0;
    entry->retransmitTimeoutMs = 3000;
    entry->retransmitCount = 0;
    
    // First retry: 3000ms -> 6000ms
    table.processRetransmissions(3500);
    (void)table.markDataSent(idx);  // Simulate actual send
    entry = table.getConnection(idx);
    entry->lastTxTimeMs = 3500;  // Update last TX time
    TEST_ASSERT_EQUAL(6000, entry->retransmitTimeoutMs);
    TEST_ASSERT_EQUAL(1, entry->retransmitCount);
    
    // Second retry: should cap at 6000ms
    entry->lastTxTimeMs = 4000;
    table.processRetransmissions(10500);
    (void)table.markDataSent(idx);  // Simulate actual send
    entry = table.getConnection(idx);
    TEST_ASSERT_EQUAL(6000, entry->retransmitTimeoutMs); // Capped
    TEST_ASSERT_EQUAL(2, entry->retransmitCount);
}

void test_MaxRetries(void) {
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());
    
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.initialSequence = 0;
    params.maxRetries = 3;
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    auto* entry = table.getConnection(idx);
    
    entry->pendingRetransmission = true;
    entry->lastTxTimeMs = 0;
    entry->retransmitTimeoutMs = 100; // Short timeout for test
    entry->retransmitCount = 2; // Already at 2 retries
    
    // Third retry should succeed
    auto retransmits = table.processRetransmissions(200);
    TEST_ASSERT_EQUAL(1, retransmits.size());
    (void)table.markDataSent(idx);  // Simulate actual send
    
    entry = table.getConnection(idx);
    entry->lastTxTimeMs = 200;  // Update last TX time
    TEST_ASSERT_EQUAL(3, entry->retransmitCount);
    
    // Fourth retry should fail and NOT remove connection (processRetransmissions just stops)
    retransmits = table.processRetransmissions(400);
    TEST_ASSERT_EQUAL(0, retransmits.size()); // No more retransmits
    
    // Connection should still exist (cleanup happens elsewhere)
    entry = table.getConnection(idx);
    TEST_ASSERT_NOT_NULL(entry);
    // But retransmit state should be reset
    TEST_ASSERT_FALSE(entry->pendingRetransmission);
}

void test_ResetRetransmitState(void) {
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());
    
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    auto* entry = table.getConnection(idx);
    
    // Set up retransmission state
    entry->pendingRetransmission = true;
    entry->retransmitCount = 2;
    entry->retransmitTimeoutMs = 6000;
    entry->lastTxTimeMs = 1000;
    entry->pendingData = {1, 2, 3};
    
    // Reset state
    TEST_ASSERT_TRUE(table.resetRetransmitState(idx).isOk());
    
    entry = table.getConnection(idx);
    TEST_ASSERT_FALSE(entry->pendingRetransmission);
    TEST_ASSERT_EQUAL(0, entry->retransmitCount);
    TEST_ASSERT_EQUAL(3000, entry->retransmitTimeoutMs); // Reset to initial
    TEST_ASSERT_EQUAL(0, entry->pendingData.size());
}

void test_ConnectionTimeout(void) {
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());
    
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    auto* entry = table.getConnection(idx);
    
    entry->lastActivityTimeMs = 1000;
    
    // Check at 5000ms - not timed out yet (6000ms timeout)
    auto timedOut = table.processTimeouts(5000, 6000);
    TEST_ASSERT_EQUAL(0, timedOut.size());
    
    entry = table.getConnection(idx);
    TEST_ASSERT_NOT_NULL(entry);
    
    // Check at 8000ms - should timeout
    timedOut = table.processTimeouts(8000, 6000);
    TEST_ASSERT_EQUAL(1, timedOut.size());
    TEST_ASSERT_EQUAL(idx.value(), timedOut[0].value());
    
    // Connection should be removed
    entry = table.getConnection(idx);
    TEST_ASSERT_NULL(entry);
}

// ============================================================================
// Sequence number handling and duplicate detection
// ============================================================================

void test_SequenceNumberWraparound(void) {
    ConnectionStateMachine sm;
    
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.initialSequence = 14; // Near wraparound
    
    TEST_ASSERT_TRUE(sm.init(params).isOk());
    TEST_ASSERT_EQUAL(14, sm.sequenceNumber());
    
    // Increment to 15
    uint8_t seq = sm.nextSequence();
    TEST_ASSERT_EQUAL(15, seq);
    
    // Should wrap to 0 (4-bit)
    seq = sm.nextSequence();
    TEST_ASSERT_EQUAL(0, seq);
    
    // Continue
    seq = sm.nextSequence();
    TEST_ASSERT_EQUAL(1, seq);
}

void test_DuplicateDetection(void) {
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());
    
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.initialSequence = 0;
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    auto* entry = table.getConnection(idx);
    
    // First packet with seq 5 - not a duplicate
    TEST_ASSERT_FALSE(table.isDuplicate(idx, 5).value());
    TEST_ASSERT_EQUAL(5, entry->lastReceivedSeq);
    
    // Same packet again - duplicate
    TEST_ASSERT_TRUE(table.isDuplicate(idx, 5).value());
    TEST_ASSERT_EQUAL(5, entry->lastReceivedSeq);
    
    // New packet with seq 6 - not duplicate
    TEST_ASSERT_FALSE(table.isDuplicate(idx, 6).value());
    TEST_ASSERT_EQUAL(6, entry->lastReceivedSeq);
}

void test_DuplicateDetectionWithWraparound(void) {
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());
    
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    auto* entry = table.getConnection(idx);
    
    // Receive sequence 15
    TEST_ASSERT_FALSE(table.isDuplicate(idx, 15).value());
    
    // Receive sequence 0 (wrapped) - not duplicate
    TEST_ASSERT_FALSE(table.isDuplicate(idx, 0).value());
    
    // Receive sequence 0 again - duplicate
    TEST_ASSERT_TRUE(table.isDuplicate(idx, 0).value());
}

void test_SequenceMasking(void) {
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());
    
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    
    ConnectionIndex idx = table.createConnection(IndividualAddress(0x1234), params);
    
    // Send value with upper bits set - should be masked to 4-bit
    TEST_ASSERT_FALSE(table.isDuplicate(idx, 0xF5).value()); // Should become 0x05
    
    auto* entry = table.getConnection(idx);
    TEST_ASSERT_EQUAL(0x05, entry->lastReceivedSeq);
    
    // Check duplicate with masked value
    TEST_ASSERT_TRUE(table.isDuplicate(idx, 0x05).value());
}

// ============================================================================
// Test Runner
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Retransmission timing and backoff
    RUN_TEST(test_RetransmissionTimeout);
    RUN_TEST(test_ExponentialBackoff);
    RUN_TEST(test_MaxRetries);
    RUN_TEST(test_ResetRetransmitState);
    RUN_TEST(test_ConnectionTimeout);
    
    // Sequence number handling and duplicate detection
    RUN_TEST(test_SequenceNumberWraparound);
    RUN_TEST(test_DuplicateDetection);
    RUN_TEST(test_DuplicateDetectionWithWraparound);
    RUN_TEST(test_SequenceMasking);
    
    return UNITY_END();
}
