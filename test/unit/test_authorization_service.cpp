// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_authorization_service.cpp
 * @brief A_Authorize and A_Key_Write (03/03/07 §3.5.7, §3.5.8)
 */

#include "knx/application/authorization_service.hpp"
#include <unity.h>
#include <vector>

using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

namespace {

const AuthorizationKey kMaxKey = {0xAA, 0xBB, 0xCC, 0xDD};
const AuthorizationKey kConfigKey = {0x55, 0x66, 0x77, 0x88};
const AuthorizationKey kMgmtKey = {0x11, 0x22, 0x33, 0x44};

/// Levels 0, 1 and 2 keyed; 3…14 have no key, so 3 is the free level.
void keyThreeLevels(AuthorizationService& service)
{
    TEST_ASSERT_TRUE(service.setKey(0, kMaxKey).isOk());
    TEST_ASSERT_TRUE(service.setKey(1, kConfigKey).isOk());
    TEST_ASSERT_TRUE(service.setKey(2, kMgmtKey).isOk());
}

const knx::IndividualAddress kTool(1, 1, 254);
const knx::IndividualAddress kStranger(1, 1, 5);

} // namespace

// ============================================================================
// The key table
// ============================================================================

void test_ex_factory_every_level_has_no_key(void)
{
    AuthorizationService service;
    for (const auto& key : service.keyTable()) {
        TEST_ASSERT_TRUE(key == AuthorizationService::kNoKey);
    }
    TEST_ASSERT_FALSE(service.hasKeys());
}

void test_levels_zero_to_fourteen_take_a_key_and_fifteen_does_not(void)
{
    AuthorizationService service;
    TEST_ASSERT_TRUE(service.setKey(0, kMaxKey).isOk());
    TEST_ASSERT_TRUE(service.setKey(14, kMgmtKey).isOk());
    TEST_ASSERT_TRUE(service.setKey(15, kMgmtKey).isError());
    TEST_ASSERT_TRUE(service.keyTable()[14] == kMgmtKey);
}

void test_key_table_round_trips_through_its_record(void)
{
    AuthorizationService service;
    keyThreeLevels(service);
    std::array<uint8_t, AuthorizationService::kSerializedKeyTableSize> record{};
    AuthorizationService::serializeKeyTable(service.keyTable(), record);
    TEST_ASSERT_EQUAL_HEX8(0xAA, record[0]);
    TEST_ASSERT_EQUAL_HEX8(0x11, record[8]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, record[12]);

    AuthorizationService restored;
    restored.restoreKeyTable(AuthorizationService::deserializeKeyTable(record));
    TEST_ASSERT_TRUE(restored.keyTable() == service.keyTable());
}

// ============================================================================
// A_Authorize (§3.5.7)
// ============================================================================

// Habinari never keys a device; nothing may change for it.
void test_unkeyed_device_grants_level_zero_whatever_the_key(void)
{
    AuthorizationService service;
    TEST_ASSERT_EQUAL_UINT8(0u, service.currentAccessLevel(kStranger));
    TEST_ASSERT_EQUAL_UINT8(0u, service.levelForKey(AuthorizationService::kNoKey));
    TEST_ASSERT_EQUAL_UINT8(0u, service.levelForKey({0x00, 0x00, 0x00, 0x00}));
    TEST_ASSERT_EQUAL_UINT8(0u, service.levelForKey(kMaxKey));

    service.handleRequest(kTool, {0x12, 0x34, 0x56, 0x78});
    TEST_ASSERT_EQUAL_UINT8(0u, service.currentAccessLevel(kTool));
}

void test_each_key_authorises_its_own_level(void)
{
    AuthorizationService service;
    keyThreeLevels(service);

    AccessLevel answered = 0xEE;
    service.setResponseCallback([&](const knx::IndividualAddress&, AccessLevel level) { answered = level; });

    service.handleRequest(kTool, kMgmtKey);
    TEST_ASSERT_EQUAL_UINT8(2u, answered);
    TEST_ASSERT_EQUAL_UINT8(2u, service.currentAccessLevel(kTool));
    service.handleRequest(kTool, kConfigKey);
    TEST_ASSERT_EQUAL_UINT8(1u, answered);
    service.handleRequest(kTool, kMaxKey);
    TEST_ASSERT_EQUAL_UINT8(0u, answered);
    TEST_ASSERT_EQUAL_UINT8(0u, service.currentAccessLevel(kTool));
}

void test_invalid_key_gets_the_minimum_level(void)
{
    AuthorizationService service;
    keyThreeLevels(service);
    service.handleRequest(kTool, {0x99, 0x99, 0x99, 0x99});
    TEST_ASSERT_EQUAL_UINT8(15u, service.currentAccessLevel(kTool));
}

// "If the communication partner does not authorize itself, the Remote
// Management shall select the maximum access level protected with FFFFFFFFh";
// authorising with FFFFFFFFh is how DM_Authorize asks for that same level.
void test_no_authorisation_and_ffffffff_both_give_the_free_level(void)
{
    AuthorizationService service;
    keyThreeLevels(service);
    TEST_ASSERT_EQUAL_UINT8(3u, service.freeAccessLevel());
    TEST_ASSERT_EQUAL_UINT8(3u, service.currentAccessLevel(kStranger));
    service.handleRequest(kTool, AuthorizationService::kNoKey);
    TEST_ASSERT_EQUAL_UINT8(3u, service.currentAccessLevel(kTool));

    // A gap: level 1 without a key is the most privileged unprotected level.
    TEST_ASSERT_TRUE(service.setKey(1, AuthorizationService::kNoKey).isOk());
    TEST_ASSERT_EQUAL_UINT8(1u, service.currentAccessLevel(kStranger));

    // Every level keyed: nothing is free but the minimum.
    AuthorizationService allKeyed;
    for (AccessLevel level = 0; level < AuthorizationService::kKeyedLevelCount; ++level) {
        TEST_ASSERT_TRUE(allKeyed.setKey(level, {0, 0, 0, level}).isOk());
    }
    TEST_ASSERT_EQUAL_UINT8(15u, allKeyed.currentAccessLevel(kStranger));
}

void test_level_lasts_until_cleared(void)
{
    AuthorizationService service;
    keyThreeLevels(service);
    service.handleRequest(kTool, kMaxKey);
    service.clearAuthorization(kTool);
    TEST_ASSERT_EQUAL_UINT8(3u, service.currentAccessLevel(kTool));

    service.handleRequest(kTool, kMaxKey);
    service.handleRequest(kStranger, kMaxKey);
    service.clearAllAuthorizations();
    TEST_ASSERT_EQUAL_UINT8(3u, service.currentAccessLevel(kTool));
    TEST_ASSERT_EQUAL_UINT8(3u, service.currentAccessLevel(kStranger));
}

void test_authorisation_expiry(void)
{
    AuthorizationService service;
    keyThreeLevels(service);
    uint32_t tick = 10;
    service.setTimeSource([&]() { return tick; });
    service.setAuthorizationTimeout(5);

    service.handleRequest(kTool, kMaxKey);
    tick = 14;
    TEST_ASSERT_EQUAL_UINT8(0u, service.currentAccessLevel(kTool));
    tick = 15;
    TEST_ASSERT_EQUAL_UINT8(3u, service.currentAccessLevel(kTool));
}

void test_oldest_authorisation_is_evicted_when_full(void)
{
    AuthorizationService service;
    keyThreeLevels(service);
    uint32_t tick = 1;
    service.setTimeSource([&]() { return tick; });

    for (uint16_t device = 1; device <= 16; ++device) {
        service.handleRequest(knx::IndividualAddress(1, 1, device), kMaxKey);
        ++tick;
    }
    service.handleRequest(knx::IndividualAddress(1, 1, 17), kMaxKey);

    TEST_ASSERT_EQUAL_UINT8(3u, service.currentAccessLevel(knx::IndividualAddress(1, 1, 1)));
    TEST_ASSERT_EQUAL_UINT8(0u, service.currentAccessLevel(knx::IndividualAddress(1, 1, 16)));
    TEST_ASSERT_EQUAL_UINT8(0u, service.currentAccessLevel(knx::IndividualAddress(1, 1, 17)));
}

// ============================================================================
// A_Key_Write (§3.5.8)
// ============================================================================

void test_key_write_at_a_sufficient_level_is_stored_and_answered_with_the_level(void)
{
    AuthorizationService service;
    int changes = 0;
    service.setKeyTableChangedCallback([&](const AuthorizationService::KeyTable&) { ++changes; });

    // Unkeyed: everybody is at level 0, so level 0 itself may be keyed.
    TEST_ASSERT_EQUAL_UINT8(0u, service.handleKeyWrite(kTool, 0, kMaxKey));
    TEST_ASSERT_TRUE(service.keyTable()[0] == kMaxKey);
    TEST_ASSERT_EQUAL_INT(1, changes);

    // Level 0 is protected now, so kTool, which never authorised, is at the
    // free level 1 and may no longer touch it ...
    TEST_ASSERT_EQUAL_UINT8(0xFFu, service.handleKeyWrite(kTool, 0, {1, 2, 3, 4}));
    // ... until it authorises. The same key again changes nothing to store,
    // but is still answered.
    service.handleRequest(kTool, kMaxKey);
    TEST_ASSERT_EQUAL_UINT8(0u, service.handleKeyWrite(kTool, 0, kMaxKey));
    TEST_ASSERT_EQUAL_INT(1, changes);
}

void test_key_write_above_the_current_level_is_refused_with_ff(void)
{
    AuthorizationService service;
    keyThreeLevels(service);
    int changes = 0;
    service.setKeyTableChangedCallback([&](const AuthorizationService::KeyTable&) { ++changes; });

    // kTool holds level 2; level 1 and 0 are more privileged.
    service.handleRequest(kTool, kMgmtKey);
    TEST_ASSERT_EQUAL_UINT8(0xFFu, service.handleKeyWrite(kTool, 1, {1, 2, 3, 4}));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, service.handleKeyWrite(kTool, 0, {1, 2, 3, 4}));
    TEST_ASSERT_TRUE(service.keyTable()[1] == kConfigKey);
    TEST_ASSERT_EQUAL_INT(0, changes);

    // Its own level and less privileged ones it may change.
    TEST_ASSERT_EQUAL_UINT8(2u, service.handleKeyWrite(kTool, 2, {1, 2, 3, 4}));
    TEST_ASSERT_EQUAL_UINT8(5u, service.handleKeyWrite(kTool, 5, {5, 5, 5, 5}));
    TEST_ASSERT_EQUAL_INT(2, changes);
}

void test_key_write_for_a_level_without_a_key_slot_is_refused(void)
{
    AuthorizationService service;
    TEST_ASSERT_EQUAL_UINT8(0xFFu, service.handleKeyWrite(kTool, 15, {1, 2, 3, 4}));
    TEST_ASSERT_EQUAL_UINT8(0xFFu, service.handleKeyWrite(kTool, 0x80, {1, 2, 3, 4}));
    TEST_ASSERT_FALSE(service.hasKeys());
}

void test_key_write_of_ffffffff_removes_the_key(void)
{
    AuthorizationService service;
    keyThreeLevels(service);
    service.handleRequest(kTool, kMaxKey);

    TEST_ASSERT_EQUAL_UINT8(2u, service.handleKeyWrite(kTool, 2, AuthorizationService::kNoKey));
    TEST_ASSERT_TRUE(service.keyTable()[2] == AuthorizationService::kNoKey);
    // Level 2 is now the most privileged level without a key.
    TEST_ASSERT_EQUAL_UINT8(2u, service.currentAccessLevel(kStranger));
    // And the old key no longer authorises anything.
    service.handleRequest(kStranger, kMgmtKey);
    TEST_ASSERT_EQUAL_UINT8(15u, service.currentAccessLevel(kStranger));
}

void test_keyed_level_then_gates_a_partner_that_does_not_authorise(void)
{
    AuthorizationService service;
    // ETS on an unkeyed device protects level 0 ...
    TEST_ASSERT_EQUAL_UINT8(0u, service.handleKeyWrite(kTool, 0, kMaxKey));
    // ... after which a partner that does not authorise is at level 1, and
    // one that authorises with the key is back at 0.
    TEST_ASSERT_EQUAL_UINT8(1u, service.currentAccessLevel(kStranger));
    service.handleRequest(kStranger, kMaxKey);
    TEST_ASSERT_EQUAL_UINT8(0u, service.currentAccessLevel(kStranger));
}

// ============================================================================
// Master Reset (03/05/02 §3.7.1.2.3.2.3)
// ============================================================================

void test_reset_clears_the_requesters_level_and_less_privileged_ones(void)
{
    AuthorizationService service;
    keyThreeLevels(service);
    int changes = 0;
    service.setKeyTableChangedCallback([&](const AuthorizationService::KeyTable&) { ++changes; });

    service.resetKeysFrom(1);
    TEST_ASSERT_TRUE(service.keyTable()[0] == kMaxKey);
    TEST_ASSERT_TRUE(service.keyTable()[1] == AuthorizationService::kNoKey);
    TEST_ASSERT_TRUE(service.keyTable()[2] == AuthorizationService::kNoKey);
    TEST_ASSERT_EQUAL_INT(1, changes);

    // Nothing left to reset at or above 1: no change to persist.
    service.resetKeysFrom(1);
    TEST_ASSERT_EQUAL_INT(1, changes);

    service.resetKeysFrom(0);
    TEST_ASSERT_FALSE(service.hasKeys());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ex_factory_every_level_has_no_key);
    RUN_TEST(test_levels_zero_to_fourteen_take_a_key_and_fifteen_does_not);
    RUN_TEST(test_key_table_round_trips_through_its_record);
    RUN_TEST(test_unkeyed_device_grants_level_zero_whatever_the_key);
    RUN_TEST(test_each_key_authorises_its_own_level);
    RUN_TEST(test_invalid_key_gets_the_minimum_level);
    RUN_TEST(test_no_authorisation_and_ffffffff_both_give_the_free_level);
    RUN_TEST(test_level_lasts_until_cleared);
    RUN_TEST(test_authorisation_expiry);
    RUN_TEST(test_oldest_authorisation_is_evicted_when_full);
    RUN_TEST(test_key_write_at_a_sufficient_level_is_stored_and_answered_with_the_level);
    RUN_TEST(test_key_write_above_the_current_level_is_refused_with_ff);
    RUN_TEST(test_key_write_for_a_level_without_a_key_slot_is_refused);
    RUN_TEST(test_key_write_of_ffffffff_removes_the_key);
    RUN_TEST(test_keyed_level_then_gates_a_partner_that_does_not_authorise);
    RUN_TEST(test_reset_clears_the_requesters_level_and_less_privileged_ones);
    return UNITY_END();
}
