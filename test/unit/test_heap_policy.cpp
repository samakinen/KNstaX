// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_heap_policy.cpp
 * @brief Unit tests for the heap policy (knx/util/heap.hpp)
 *
 * The decision function is driven directly: on a host the policy does not
 * replace operator new (KNX_HEAP_POLICY=free), so what is tested is the state
 * machine and the handler contract the replaced operator new relies on. The
 * trap of a debug build cannot be tested in-process; the release behaviour (the
 * allocation is refused) is tested where NDEBUG is set. The gate image proves the
 * rest: it runs bus traffic with the heap locked.
 */

#include "unity.h"
#include "knx/util/heap.hpp"

#include <cstddef>

namespace {

std::size_t g_seenBytes = 0;
const void* g_seenCaller = nullptr;
int g_calls = 0;

void recordViolation(std::size_t bytes, const void* caller) {
    g_seenBytes = bytes;
    g_seenCaller = caller;
    ++g_calls;
}

std::size_t fakeHighWater() { return 4321u; }

}  // namespace

void setUp(void) {
    knx::heap::unlock();
    knx::heap::setViolationHandler(nullptr);
    knx::heap::setHighWaterHook(nullptr);
    g_seenBytes = 0;
    g_seenCaller = nullptr;
    g_calls = 0;
}

void tearDown(void) {
    knx::heap::unlock();
    knx::heap::setViolationHandler(nullptr);
    knx::heap::setHighWaterHook(nullptr);
}

void test_unlocked_allocations_are_admitted_and_counted(void) {
    const std::size_t before = knx::heap::allocationCount();
    TEST_ASSERT_FALSE(knx::heap::isLocked());
    TEST_ASSERT_TRUE(knx::heap::admit(64, nullptr));
    TEST_ASSERT_EQUAL_UINT32(before + 1u, knx::heap::allocationCount());
    TEST_ASSERT_EQUAL_UINT32(0u, knx::heap::violationCount());
}

void test_lock_and_unlock_change_the_state(void) {
    knx::heap::lock();
    TEST_ASSERT_TRUE(knx::heap::isLocked());
    knx::heap::unlock();
    TEST_ASSERT_FALSE(knx::heap::isLocked());
}

void test_a_handler_sees_a_locked_allocation_and_lets_it_go_ahead(void) {
    knx::heap::setViolationHandler(&recordViolation);
    knx::heap::lock();
    int marker = 0;
    const std::size_t violations = knx::heap::violationCount();
    TEST_ASSERT_TRUE(knx::heap::admit(48, &marker));
    TEST_ASSERT_EQUAL_INT(1, g_calls);
    TEST_ASSERT_EQUAL_UINT32(48u, g_seenBytes);
    TEST_ASSERT_TRUE(g_seenCaller == &marker);
    TEST_ASSERT_EQUAL_UINT32(violations + 1u, knx::heap::violationCount());
}

void test_no_handler_unlocked_means_no_violation(void) {
    knx::heap::setViolationHandler(&recordViolation);
    TEST_ASSERT_TRUE(knx::heap::admit(48, nullptr));
    TEST_ASSERT_EQUAL_INT(0, g_calls);
}

#if defined(NDEBUG)
void test_release_build_refuses_a_locked_allocation_without_a_handler(void) {
    knx::heap::lock();
    TEST_ASSERT_FALSE(knx::heap::admit(16, nullptr));
}
#endif

void test_high_water_comes_from_the_products_hook(void) {
    TEST_ASSERT_EQUAL_UINT32(0u, knx::heap::highWater());
    knx::heap::setHighWaterHook(&fakeHighWater);
    TEST_ASSERT_EQUAL_UINT32(4321u, knx::heap::highWater());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_unlocked_allocations_are_admitted_and_counted);
    RUN_TEST(test_lock_and_unlock_change_the_state);
    RUN_TEST(test_a_handler_sees_a_locked_allocation_and_lets_it_go_ahead);
    RUN_TEST(test_no_handler_unlocked_means_no_violation);
#if defined(NDEBUG)
    RUN_TEST(test_release_build_refuses_a_locked_allocation_without_a_handler);
#endif
    RUN_TEST(test_high_water_comes_from_the_products_hook);
    return UNITY_END();
}
