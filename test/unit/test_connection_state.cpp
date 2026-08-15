// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_connection_state.cpp
 * @brief Unit tests for connection state machine
 */

#include "unity.h"
#include "knx/transport/connection_state.hpp"

using namespace knx::transport;
using namespace knx;

static ConnectionStateMachine machine;

void setUp(void) {
}

void tearDown(void) {
}

void test_InitializeAndCheck(void) {
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.timeoutMs = 3000;
    params.maxRetries = 3;
    
    TEST_ASSERT_TRUE(machine.init(params).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ConnectionState::Idle), static_cast<int>(machine.state()));
    TEST_ASSERT_EQUAL(0, machine.sequenceNumber());
}

void test_RejectsInvalidRemoteAddress(void) {
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0xFFFF);
    
    TEST_ASSERT_FALSE(machine.init(params).isOk());
}

void test_StateTransitions(void) {
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.timeoutMs = 3000;
    
    TEST_ASSERT_TRUE(machine.init(params).isOk());
    TEST_ASSERT_FALSE(machine.isConnected());
    
    // Idle -> Connecting
    TEST_ASSERT_TRUE(machine.handleEvent(ConnectionEvent::ConnectRequest).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ConnectionState::Connecting), static_cast<int>(machine.state()));
    
    // Connecting -> Connected
    TEST_ASSERT_TRUE(machine.handleEvent(ConnectionEvent::ConnectResponse).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ConnectionState::Connected), static_cast<int>(machine.state()));
    TEST_ASSERT_TRUE(machine.isConnected());
    
    // Connected -> Disconnecting
    TEST_ASSERT_TRUE(machine.handleEvent(ConnectionEvent::DisconnectRequest).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ConnectionState::Disconnecting), static_cast<int>(machine.state()));
    
    // Disconnecting -> Idle
    TEST_ASSERT_TRUE(machine.handleEvent(ConnectionEvent::DisconnectResponse).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ConnectionState::Idle), static_cast<int>(machine.state()));
}

void test_SequenceNumberHandling(void) {
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.timeoutMs = 3000;
    params.initialSequence = 100;
    
    TEST_ASSERT_TRUE(machine.init(params).isOk());
    TEST_ASSERT_EQUAL(100, machine.sequenceNumber());
    
    // Test sequence increment - each call increments and returns current value
    uint8_t seq1 = machine.nextSequence();
    uint8_t seq2 = machine.nextSequence();
    uint8_t seq3 = machine.nextSequence();
    
    // Verify they're incrementing
    TEST_ASSERT(seq2 > seq1);
    TEST_ASSERT(seq3 > seq2);
}

void test_RetryCounter(void) {
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.timeoutMs = 3000;
    
    TEST_ASSERT_TRUE(machine.init(params).isOk());
    TEST_ASSERT_EQUAL(0, machine.retryCount());
    
    machine.incrementRetry();
    TEST_ASSERT_EQUAL(1, machine.retryCount());
    
    machine.incrementRetry();
    TEST_ASSERT_EQUAL(2, machine.retryCount());
}

void test_InvalidEventsRejected(void) {
    ConnectionParams params;
    params.remoteAddress = IndividualAddress(0x1234);
    params.timeoutMs = 3000;
    
    TEST_ASSERT_TRUE(machine.init(params).isOk());
    
    // Can't receive data in Idle state
    TEST_ASSERT_FALSE(machine.handleEvent(ConnectionEvent::DataIndicate).isOk());
    
    // Can't disconnect while idle
    TEST_ASSERT_FALSE(machine.handleEvent(ConnectionEvent::DisconnectRequest).isOk());
}

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_InitializeAndCheck);
    RUN_TEST(test_RejectsInvalidRemoteAddress);
    RUN_TEST(test_StateTransitions);
    RUN_TEST(test_SequenceNumberHandling);
    RUN_TEST(test_RetryCounter);
    RUN_TEST(test_InvalidEventsRejected);
    
    return UNITY_END();
}

