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
    
    // The acknowledge timeout stays at 3 s (03_03_04 §5, A9); it used to
    // double to 6 s, which this test asserted.
    entry = table.getConnection(idx);
    TEST_ASSERT_EQUAL(3000, entry->retransmitTimeoutMs);
    TEST_ASSERT_EQUAL(1, entry->retransmitCount);
}

void test_AcknowledgeTimeoutIsFixed(void) {
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
    
    // First repetition after 3 s.
    TEST_ASSERT_EQUAL(1, table.processRetransmissions(3500).size());
    (void)table.markDataSent(idx);  // Simulate actual send
    entry = table.getConnection(idx);
    entry->lastTxTimeMs = 3500;  // Update last TX time
    TEST_ASSERT_EQUAL(1, entry->retransmitCount);

    // The second repetition is due 3 s after the first, not 6 s (the old
    // exponential back-off would have waited until 9500).
    TEST_ASSERT_EQUAL(0, table.processRetransmissions(6499).size());
    TEST_ASSERT_EQUAL(1, table.processRetransmissions(6500).size());
    (void)table.markDataSent(idx);  // Simulate actual send
    entry = table.getConnection(idx);
    TEST_ASSERT_EQUAL(3000, entry->retransmitTimeoutMs);
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
    
    // E18: repetitions used up. The connection is closed (A6) and reported so
    // the transport layer can send T_Disconnect. This test used to assert the
    // connection stayed open with its pending frame silently dropped.
    knx::util::FixedVector<ClosedConnection, ConnectionTable::MAX_CONNECTIONS> closed;
    retransmits = table.processRetransmissions(400, &closed);
    TEST_ASSERT_EQUAL(0, retransmits.size()); // No more retransmits
    TEST_ASSERT_EQUAL(1, closed.size());
    TEST_ASSERT_EQUAL(idx.value(), closed[0].index.value());
    TEST_ASSERT_EQUAL_HEX16(0x1234, closed[0].remoteAddress.raw);
    TEST_ASSERT_NULL(table.getConnection(idx));
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
    TEST_ASSERT_EQUAL(idx.value(), timedOut[0].index.value());
    TEST_ASSERT_EQUAL_HEX16(0x1234, timedOut[0].remoteAddress.raw);
    
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
    RUN_TEST(test_AcknowledgeTimeoutIsFixed);
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
