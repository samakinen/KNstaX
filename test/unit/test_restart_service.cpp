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

// ============================================================================
// Master Reset erase codes and error codes (03/05/02 Tables 4 and 5)
// ============================================================================

void test_RestartService_MasterResetWithoutHandlerSupportsOnlyConfirmedRestart(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    knx::IndividualAddress source(1, 2, 3);

    // Without a Master Reset callback nothing could actually be reset, so
    // claiming success for 02h…07h would be a lie.
    for (uint8_t code = 0x02; code <= 0x07; ++code) {
        TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(RestartErrorCode::UnsupportedEraseCode),
                               static_cast<uint8_t>(service.handleMasterResetRequest(source, code, 0)));
        TEST_ASSERT_FALSE(service.isRestartPending());
    }
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(RestartErrorCode::NoError),
                           static_cast<uint8_t>(service.handleMasterResetRequest(source, 0x01, 0)));
    TEST_ASSERT_TRUE(service.isRestartPending());
}

void test_RestartService_MasterResetErrorCodes(void) {
    RestartService service;
    service.setRestartCallback(mockRestartCallback);
    service.setMasterResetCallback([](EraseCode) { return knx::util::Result<void>::ok(); });
    knx::IndividualAddress source(1, 2, 3);

    // Reserved and unimplemented codes, whatever the channel says.
    for (const uint8_t code : {uint8_t{0x00}, uint8_t{0x08}, uint8_t{0x09}, uint8_t{0xFF}}) {
        TEST_ASSERT_EQUAL_HEX8(0x02, static_cast<uint8_t>(service.handleMasterResetRequest(source, code, 0x05)));
    }
    // A channel on a device without channels.
    TEST_ASSERT_EQUAL_HEX8(0x03, static_cast<uint8_t>(service.handleMasterResetRequest(source, 0x05, 0x01)));
    TEST_ASSERT_FALSE(service.isRestartPending());

    // Authorisation refused.
    service.setAuthorizationCallback(mockAuthorizationDeniedCallback);
    TEST_ASSERT_EQUAL_HEX8(0x01, static_cast<uint8_t>(service.handleMasterResetRequest(source, 0x02, 0)));
    TEST_ASSERT_FALSE(service.isRestartPending());
}

void test_RestartService_MasterResetRunsEraseBeforeRestart(void) {
    RestartService service;
    std::vector<int> order;
    EraseCode erased = EraseCode::ConfirmedRestart;
    service.setMasterResetCallback([&](EraseCode code) {
        order.push_back(1);
        erased = code;
        return knx::util::Result<void>::ok();
    });
    service.setRestartCallback([&](RestartType) {
        order.push_back(2);
        return knx::util::Result<void>::ok();
    });
    knx::IndividualAddress source(1, 2, 3);

    TEST_ASSERT_EQUAL_HEX8(0x00, static_cast<uint8_t>(service.handleMasterResetRequest(source, 0x02, 0)));
    TEST_ASSERT_EQUAL_HEX8(0x02, static_cast<uint8_t>(service.getPendingEraseCode()));
    service.executePendingRestart();

    TEST_ASSERT_EQUAL_UINT32(2u, order.size());
    TEST_ASSERT_EQUAL_INT(1, order[0]);
    TEST_ASSERT_EQUAL_INT(2, order[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02, static_cast<uint8_t>(erased));

    // A Confirmed Restart resets nothing.
    order.clear();
    TEST_ASSERT_EQUAL_HEX8(0x00, static_cast<uint8_t>(service.handleMasterResetRequest(source, 0x01, 0)));
    service.executePendingRestart();
    TEST_ASSERT_EQUAL_UINT32(1u, order.size());
    TEST_ASSERT_EQUAL_INT(2, order[0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_RestartService_MasterResetWithoutHandlerSupportsOnlyConfirmedRestart);
    RUN_TEST(test_RestartService_MasterResetErrorCodes);
    RUN_TEST(test_RestartService_MasterResetRunsEraseBeforeRestart);

    RUN_TEST(test_RestartService_Init);
    RUN_TEST(test_RestartService_HandleBasicRestart);
    RUN_TEST(test_RestartService_HandleMasterReset);
    RUN_TEST(test_RestartService_ExecutePendingRestart);
    RUN_TEST(test_RestartService_ExecuteWithoutPending);
    RUN_TEST(test_RestartService_CancelRestart);
    RUN_TEST(test_RestartService_AuthorizationCheck);
    RUN_TEST(test_RestartService_AuthorizationDenied);
    RUN_TEST(test_RestartService_NoRestartCallback);
    RUN_TEST(test_RestartService_CleanupOnly);
    
    return UNITY_END();
}
