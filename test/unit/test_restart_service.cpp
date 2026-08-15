// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_restart_service.cpp
 * @brief Unit tests for restart service
 */

#include "knx/application/restart_service.hpp"
#include <unity.h>
#include <vector>

using namespace knx::application;

static bool restartCallbackInvoked = false;
static RestartType lastRestartType = RestartType::Basic;
static bool cleanupCallbackInvoked = false;
static bool authorizationCallbackInvoked = false;

knx::util::Result<void> mockRestartCallback(RestartType type) {
    restartCallbackInvoked = true;
    lastRestartType = type;
    return knx::util::Result<void>::ok();
}

void mockCleanupCallback() {
    cleanupCallbackInvoked = true;
}

knx::util::Result<void> mockAuthorizationCallback(const knx::IndividualAddress& source) {
    authorizationCallbackInvoked = true;
    return knx::util::Result<void>::ok();
}

knx::util::Result<void> mockAuthorizationDeniedCallback(const knx::IndividualAddress& source) {
    authorizationCallbackInvoked = true;
    return knx::util::ErrorCode::OperationNotSupported;
}

void setUp(void) {
    restartCallbackInvoked = false;
    cleanupCallbackInvoked = false;
    authorizationCallbackInvoked = false;
    lastRestartType = RestartType::Basic;
}

void tearDown(void) {}

// ============================================================================
// RestartService Tests
// ============================================================================

void test_RestartService_Init(void) {
    RestartService service;
    
    TEST_ASSERT_FALSE(service.isRestartPending());
}

void test_RestartService_HandleBasicRestart(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleRequest(source, RestartType::Basic);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(service.isRestartPending());
    TEST_ASSERT_EQUAL(static_cast<int>(RestartType::Basic), 
                     static_cast<int>(service.getPendingRestartType()));
}

void test_RestartService_HandleMasterReset(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleRequest(source, RestartType::MasterReset);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(service.isRestartPending());
    TEST_ASSERT_EQUAL(static_cast<int>(RestartType::MasterReset), 
                     static_cast<int>(service.getPendingRestartType()));
}

void test_RestartService_ExecutePendingRestart(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    service.setCleanupCallback(mockCleanupCallback);
    
    knx::IndividualAddress source(1, 2, 3);
    (void)service.handleRequest(source, RestartType::Basic);
    
    TEST_ASSERT_TRUE(service.isRestartPending());
    TEST_ASSERT_FALSE(restartCallbackInvoked);
    TEST_ASSERT_FALSE(cleanupCallbackInvoked);
    
    service.executePendingRestart();
    
    TEST_ASSERT_FALSE(service.isRestartPending());
    TEST_ASSERT_TRUE(restartCallbackInvoked);
    TEST_ASSERT_TRUE(cleanupCallbackInvoked);
    TEST_ASSERT_EQUAL(static_cast<int>(RestartType::Basic), 
                     static_cast<int>(lastRestartType));
}

void test_RestartService_ExecuteWithoutPending(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    
    TEST_ASSERT_FALSE(service.isRestartPending());
    
    service.executePendingRestart();
    
    // Should not invoke callback when no restart pending
    TEST_ASSERT_FALSE(restartCallbackInvoked);
}

void test_RestartService_CancelRestart(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    
    knx::IndividualAddress source(1, 2, 3);
    (void)service.handleRequest(source, RestartType::Basic);
    
    TEST_ASSERT_TRUE(service.isRestartPending());
    
    service.cancelRestart();
    
    TEST_ASSERT_FALSE(service.isRestartPending());
}

void test_RestartService_AuthorizationCheck(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    service.setAuthorizationCallback(mockAuthorizationCallback);
    
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleRequest(source, RestartType::Basic);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(authorizationCallbackInvoked);
    TEST_ASSERT_TRUE(service.isRestartPending());
}

void test_RestartService_AuthorizationDenied(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    service.setAuthorizationCallback(mockAuthorizationDeniedCallback);
    
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleRequest(source, RestartType::Basic);
    
    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_TRUE(authorizationCallbackInvoked);
    TEST_ASSERT_FALSE(service.isRestartPending());
}

void test_RestartService_NoRestartCallback(void) {
    RestartService service;
    // Don't set restart callback
    
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleRequest(source, RestartType::Basic);
    
    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_FALSE(service.isRestartPending());
}

void test_RestartService_EncodeBasicRestart(void) {
    std::array<uint8_t, RestartService::kEncodedRequestLength> encoded{};
    auto result = RestartService::encodeRequest(RestartType::Basic, encoded);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(3, encoded.size());
    
    // Check APCI (A_Restart = 0x380)
    TEST_ASSERT_EQUAL(0x03, encoded[0]);
    TEST_ASSERT_EQUAL(0x80, encoded[1]);
    
    // Check restart type
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(RestartType::Basic), encoded[2]);
}

void test_RestartService_EncodeMasterReset(void) {
    std::array<uint8_t, RestartService::kEncodedRequestLength> encoded{};
    auto result = RestartService::encodeRequest(RestartType::MasterReset, encoded);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(3, encoded.size());
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(RestartType::MasterReset), encoded[2]);
}

void test_RestartService_DecodeBasicRestart(void) {
    std::vector<uint8_t> encoded = {0x03, 0x80, 0x00};
    
    RestartType type;
    auto result = RestartService::decodeRequest(encoded, type);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(RestartType::Basic), static_cast<int>(type));
}

void test_RestartService_DecodeMasterReset(void) {
    std::vector<uint8_t> encoded = {0x03, 0x80, 0x01};
    
    RestartType type;
    auto result = RestartService::decodeRequest(encoded, type);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(RestartType::MasterReset), static_cast<int>(type));
}

void test_RestartService_DecodeInvalidType(void) {
    std::vector<uint8_t> encoded = {0x03, 0x80, 0xFF};
    
    RestartType type;
    auto result = RestartService::decodeRequest(encoded, type);
    
    TEST_ASSERT_TRUE(result.isError());
}

void test_RestartService_DecodeInvalidLength(void) {
    std::vector<uint8_t> encoded = {0x03, 0x80};
    
    RestartType type;
    auto result = RestartService::decodeRequest(encoded, type);
    
    TEST_ASSERT_TRUE(result.isError());
}

void test_RestartService_CleanupOnly(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    service.setCleanupCallback(mockCleanupCallback);
    
    knx::IndividualAddress source(1, 2, 3);
    (void)service.handleRequest(source, RestartType::MasterReset);
    
    service.executePendingRestart();
    
    TEST_ASSERT_TRUE(cleanupCallbackInvoked);
    TEST_ASSERT_TRUE(restartCallbackInvoked);
    TEST_ASSERT_EQUAL(static_cast<int>(RestartType::MasterReset), 
                     static_cast<int>(lastRestartType));
}

// ============================================================================
// Test Runner
// ============================================================================

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_RestartService_Init);
    RUN_TEST(test_RestartService_HandleBasicRestart);
    RUN_TEST(test_RestartService_HandleMasterReset);
    RUN_TEST(test_RestartService_ExecutePendingRestart);
    RUN_TEST(test_RestartService_ExecuteWithoutPending);
    RUN_TEST(test_RestartService_CancelRestart);
    RUN_TEST(test_RestartService_AuthorizationCheck);
    RUN_TEST(test_RestartService_AuthorizationDenied);
    RUN_TEST(test_RestartService_NoRestartCallback);
    RUN_TEST(test_RestartService_EncodeBasicRestart);
    RUN_TEST(test_RestartService_EncodeMasterReset);
    RUN_TEST(test_RestartService_DecodeBasicRestart);
    RUN_TEST(test_RestartService_DecodeMasterReset);
    RUN_TEST(test_RestartService_DecodeInvalidType);
    RUN_TEST(test_RestartService_DecodeInvalidLength);
    RUN_TEST(test_RestartService_CleanupOnly);
    
    return UNITY_END();
}
