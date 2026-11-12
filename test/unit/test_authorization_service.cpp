// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_authorization_service.cpp
 * @brief Unit tests for authorization service
 */

#include "knx/application/authorization_service.hpp"
#include <unity.h>
#include <vector>

using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// Authorization Service Tests
// ============================================================================

void test_AuthorizationService_Init(void) {
    AuthorizationService service;
    
    knx::IndividualAddress addr(1, 2, 3);
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::None), 
                     static_cast<int>(service.getCurrentLevel(addr)));
}

void test_AuthorizationService_SetKeys(void) {
    AuthorizationService service;
    
    AuthorizationKey mgmtKey = {0x11, 0x22, 0x33, 0x44};
    AuthorizationKey configKey = {0x55, 0x66, 0x77, 0x88};
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    
    service.setKeys(mgmtKey, configKey, maxKey);

    AuthorizationLevel level = AuthorizationLevel::None;
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationResult::Success), static_cast<int>(service.validateKey(mgmtKey, level)));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Management), static_cast<int>(level));

    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationResult::Success), static_cast<int>(service.validateKey(configKey, level)));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Configuration), static_cast<int>(level));

    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationResult::Success), static_cast<int>(service.validateKey(maxKey, level)));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum), static_cast<int>(level));
}

void test_AuthorizationService_ValidateManagementKey(void) {
    AuthorizationService service;
    
    AuthorizationKey mgmtKey = {0x11, 0x22, 0x33, 0x44};
    AuthorizationKey configKey = {0x55, 0x66, 0x77, 0x88};
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    
    service.setKeys(mgmtKey, configKey, maxKey);
    
    AuthorizationLevel level;
    auto result = service.validateKey(mgmtKey, level);
    
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationResult::Success), static_cast<int>(result));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Management), static_cast<int>(level));
}

void test_AuthorizationService_ValidateConfigurationKey(void) {
    AuthorizationService service;
    
    AuthorizationKey mgmtKey = {0x11, 0x22, 0x33, 0x44};
    AuthorizationKey configKey = {0x55, 0x66, 0x77, 0x88};
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    
    service.setKeys(mgmtKey, configKey, maxKey);
    
    AuthorizationLevel level;
    auto result = service.validateKey(configKey, level);
    
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationResult::Success), static_cast<int>(result));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Configuration), static_cast<int>(level));
}

void test_AuthorizationService_ValidateMaximumKey(void) {
    AuthorizationService service;
    
    AuthorizationKey mgmtKey = {0x11, 0x22, 0x33, 0x44};
    AuthorizationKey configKey = {0x55, 0x66, 0x77, 0x88};
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    
    service.setKeys(mgmtKey, configKey, maxKey);
    
    AuthorizationLevel level;
    auto result = service.validateKey(maxKey, level);
    
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationResult::Success), static_cast<int>(result));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum), static_cast<int>(level));
}

void test_AuthorizationService_InvalidKey(void) {
    AuthorizationService service;
    
    AuthorizationKey mgmtKey = {0x11, 0x22, 0x33, 0x44};
    AuthorizationKey configKey = {0x55, 0x66, 0x77, 0x88};
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    
    service.setKeys(mgmtKey, configKey, maxKey);
    
    AuthorizationKey wrongKey = {0x99, 0x99, 0x99, 0x99};
    AuthorizationLevel level;
    auto result = service.validateKey(wrongKey, level);
    
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationResult::Denied), static_cast<int>(result));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::None), static_cast<int>(level));
}

void test_AuthorizationService_GrantWithoutKeysConfigured(void) {
    // Unsecured device (no keys configured) grants free access: ETS
    // authorizes with the default key FF FF FF FF and expects access
    // level 0 (maximum). Denying here would abort every download.
    AuthorizationService service;

    AuthorizationLevel level = AuthorizationLevel::None;
    AuthorizationKey zeroKey = {0x00, 0x00, 0x00, 0x00};
    auto result = service.validateKey(zeroKey, level);

    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationResult::Success), static_cast<int>(result));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum), static_cast<int>(level));
}

void test_AuthorizationService_HandleRequest(void) {
    AuthorizationService service;
    
    AuthorizationKey mgmtKey = {0x11, 0x22, 0x33, 0x44};
    AuthorizationKey configKey = {0x55, 0x66, 0x77, 0x88};
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    
    service.setKeys(mgmtKey, configKey, maxKey);
    
    bool responseSent = false;
    AuthorizationLevel responseLevel = AuthorizationLevel::None;
    
    service.setResponseCallback([&](const knx::IndividualAddress& dest, AuthorizationLevel level) {
        responseSent = true;
        responseLevel = level;
    });
    
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleRequest(source, configKey);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(responseSent);
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Configuration), 
                     static_cast<int>(responseLevel));
}

void test_AuthorizationService_StoreAuthorization(void) {
    AuthorizationService service;
    
    AuthorizationKey mgmtKey = {0x11, 0x22, 0x33, 0x44};
    AuthorizationKey configKey = {0x55, 0x66, 0x77, 0x88};
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    
    service.setKeys(mgmtKey, configKey, maxKey);
    
    knx::IndividualAddress source(1, 2, 3);
    (void)service.handleRequest(source, configKey);
    
    // Check that authorization is stored
    auto level = service.getCurrentLevel(source);
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Configuration), 
                     static_cast<int>(level));
}

void test_AuthorizationService_ClearAuthorization(void) {
    AuthorizationService service;
    
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    service.setKeys(maxKey, maxKey, maxKey);
    
    knx::IndividualAddress source(1, 2, 3);
    (void)service.handleRequest(source, maxKey);
    
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum), 
                     static_cast<int>(service.getCurrentLevel(source)));
    
    service.clearAuthorization(source);
    
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::None), 
                     static_cast<int>(service.getCurrentLevel(source)));
}

void test_AuthorizationService_ClearAllAuthorizations(void) {
    AuthorizationService service;
    
    AuthorizationKey key = {0x11, 0x22, 0x33, 0x44};
    service.setKeys(key, key, key);
    
    knx::IndividualAddress addr1(1, 1, 1);
    knx::IndividualAddress addr2(2, 2, 2);
    
    (void)service.handleRequest(addr1, key);
    (void)service.handleRequest(addr2, key);
    
    // When all keys are identical, Maximum level is granted (checked first)
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum), 
                     static_cast<int>(service.getCurrentLevel(addr1)));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum), 
                     static_cast<int>(service.getCurrentLevel(addr2)));
    
    service.clearAllAuthorizations();
    
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::None), 
                     static_cast<int>(service.getCurrentLevel(addr1)));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::None), 
                     static_cast<int>(service.getCurrentLevel(addr2)));
}

void test_AuthorizationService_CustomValidationCallback(void) {
    AuthorizationService service;
    
    bool callbackInvoked = false;
    
    service.setValidationCallback([&](const knx::IndividualAddress& source, 
                                      const AuthorizationKey& key) -> knx::util::Result<AuthorizationLevel> {
        callbackInvoked = true;
        // Custom logic: grant maximum level if key is all 0xFF
        if (key[0] == 0xFF && key[1] == 0xFF && key[2] == 0xFF && key[3] == 0xFF) {
            return AuthorizationLevel::Maximum;
        }
        return knx::util::ErrorCode::OperationNotSupported;
    });
    
    AuthorizationKey customKey = {0xFF, 0xFF, 0xFF, 0xFF};
    knx::IndividualAddress source(1, 2, 3);
    
    auto result = service.handleRequest(source, customKey);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(callbackInvoked);
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum), 
                     static_cast<int>(service.getCurrentLevel(source)));
}

void test_AuthorizationService_UpdateExistingAuthorization(void) {
    AuthorizationService service;
    
    AuthorizationKey mgmtKey = {0x11, 0x22, 0x33, 0x44};
    AuthorizationKey configKey = {0x55, 0x66, 0x77, 0x88};
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    
    service.setKeys(mgmtKey, configKey, maxKey);
    
    knx::IndividualAddress source(1, 2, 3);
    
    // First authorization at management level
    (void)service.handleRequest(source, mgmtKey);
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Management), 
                     static_cast<int>(service.getCurrentLevel(source)));
    
    // Upgrade to maximum level
    (void)service.handleRequest(source, maxKey);
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum), 
                     static_cast<int>(service.getCurrentLevel(source)));
}

void test_AuthorizationService_ExpiresAuthorizations(void) {
    AuthorizationService service;

    uint32_t currentTick = 10;
    service.setTimeSource([&]() { return currentTick; });
    service.setAuthorizationTimeout(5);

    AuthorizationKey key = {0x11, 0x22, 0x33, 0x44};
    service.setKeys(key, key, key);

    knx::IndividualAddress source(1, 2, 3);
    TEST_ASSERT_TRUE(service.handleRequest(source, key).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum),
                      static_cast<int>(service.getCurrentLevel(source)));

    currentTick = 14;
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum),
                      static_cast<int>(service.getCurrentLevel(source)));

    currentTick = 15;
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::None),
                      static_cast<int>(service.getCurrentLevel(source)));
}

void test_AuthorizationService_RefreshesTimestampOnUpdate(void) {
    AuthorizationService service;

    uint32_t currentTick = 100;
    service.setTimeSource([&]() { return currentTick; });
    service.setAuthorizationTimeout(10);

    AuthorizationKey mgmtKey = {0x11, 0x22, 0x33, 0x44};
    AuthorizationKey maxKey = {0xAA, 0xBB, 0xCC, 0xDD};
    service.setKeys(mgmtKey, mgmtKey, maxKey);

    knx::IndividualAddress source(1, 2, 3);
    TEST_ASSERT_TRUE(service.handleRequest(source, mgmtKey).isOk());

    currentTick = 108;
    TEST_ASSERT_TRUE(service.handleRequest(source, maxKey).isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum),
                      static_cast<int>(service.getCurrentLevel(source)));

    currentTick = 117;
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum),
                      static_cast<int>(service.getCurrentLevel(source)));

    currentTick = 118;
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::None),
                      static_cast<int>(service.getCurrentLevel(source)));
}

void test_AuthorizationService_EvictsOldestAuthorization(void) {
    AuthorizationService service;

    uint32_t currentTick = 1;
    service.setTimeSource([&]() { return currentTick; });

    AuthorizationKey key = {0x11, 0x22, 0x33, 0x44};
    service.setKeys(key, key, key);

    for (uint16_t device = 1; device <= 16; ++device) {
        TEST_ASSERT_TRUE(service.handleRequest(knx::IndividualAddress(1, 1, device), key).isOk());
        ++currentTick;
    }

    const knx::IndividualAddress oldest(1, 1, 1);
    const knx::IndividualAddress newest(1, 1, 16);
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum),
                      static_cast<int>(service.getCurrentLevel(oldest)));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum),
                      static_cast<int>(service.getCurrentLevel(newest)));

    TEST_ASSERT_TRUE(service.handleRequest(knx::IndividualAddress(1, 1, 17), key).isOk());

    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::None),
                      static_cast<int>(service.getCurrentLevel(oldest)));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum),
                      static_cast<int>(service.getCurrentLevel(newest)));
    TEST_ASSERT_EQUAL(static_cast<int>(AuthorizationLevel::Maximum),
                      static_cast<int>(service.getCurrentLevel(knx::IndividualAddress(1, 1, 17))));
}

// ============================================================================
// Test Runner
// ============================================================================

// ============================================================================
// Current access level (03/03/07 §3.5.7), what the access checks compare
// ============================================================================

void test_AuthorizationService_UnkeyedDeviceGrantsLevelZeroToEveryone(void) {
    AuthorizationService service;
    // Nobody authorised, and nobody needs to: the gate must not change
    // anything on a device that was never keyed.
    TEST_ASSERT_EQUAL_UINT8(0u, service.currentAccessLevel(knx::IndividualAddress(1, 1, 5)));
}

void test_AuthorizationService_KeyedDeviceUsesGrantedOrUnprotectedLevel(void) {
    AuthorizationService service;
    const AuthorizationKey management = {0x00, 0x00, 0x00, 0x02};
    const AuthorizationKey configuration = {0x00, 0x00, 0x00, 0x01};
    const AuthorizationKey maximum = {0x00, 0x00, 0x00, 0x00};
    service.setKeys(management, configuration, maximum);

    const knx::IndividualAddress stranger(1, 1, 5);
    const knx::IndividualAddress tool(1, 1, 254);
    // Every level protected: an unauthorised partner has minimal access.
    TEST_ASSERT_EQUAL_UINT8(15u, service.currentAccessLevel(stranger));

    TEST_ASSERT_TRUE(service.handleRequest(tool, configuration).isOk());
    TEST_ASSERT_EQUAL_UINT8(1u, service.currentAccessLevel(tool));
    TEST_ASSERT_EQUAL_UINT8(15u, service.currentAccessLevel(stranger));

    // "The maximum access level protected with FFFFFFFFh" is what an
    // unauthorised partner gets.
    AuthorizationService partlyKeyed;
    partlyKeyed.setKeys({0xFF, 0xFF, 0xFF, 0xFF}, configuration, maximum);
    TEST_ASSERT_EQUAL_UINT8(2u, partlyKeyed.currentAccessLevel(stranger));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_AuthorizationService_UnkeyedDeviceGrantsLevelZeroToEveryone);
    RUN_TEST(test_AuthorizationService_KeyedDeviceUsesGrantedOrUnprotectedLevel);

    RUN_TEST(test_AuthorizationService_Init);
    RUN_TEST(test_AuthorizationService_SetKeys);
    RUN_TEST(test_AuthorizationService_ValidateManagementKey);
    RUN_TEST(test_AuthorizationService_ValidateConfigurationKey);
    RUN_TEST(test_AuthorizationService_ValidateMaximumKey);
    RUN_TEST(test_AuthorizationService_InvalidKey);
    RUN_TEST(test_AuthorizationService_GrantWithoutKeysConfigured);
    RUN_TEST(test_AuthorizationService_HandleRequest);
    RUN_TEST(test_AuthorizationService_StoreAuthorization);
    RUN_TEST(test_AuthorizationService_ClearAuthorization);
    RUN_TEST(test_AuthorizationService_ClearAllAuthorizations);
    RUN_TEST(test_AuthorizationService_CustomValidationCallback);
    RUN_TEST(test_AuthorizationService_UpdateExistingAuthorization);
    RUN_TEST(test_AuthorizationService_ExpiresAuthorizations);
    RUN_TEST(test_AuthorizationService_RefreshesTimestampOnUpdate);
    RUN_TEST(test_AuthorizationService_EvictsOldestAuthorization);
    
    return UNITY_END();
}
