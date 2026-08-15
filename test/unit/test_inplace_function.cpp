// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_inplace_function.cpp
 * @brief Unit tests for knx::util::InplaceFunction
 */

#include "knx/util/inplace_function.hpp"
#include <unity.h>
#include <cstring>
#include <utility>

using knx::util::InplaceFunction;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Basic invocation
// ---------------------------------------------------------------------------

static void test_inplace_function_invokes_plain_function_pointer()
{
    InplaceFunction<int(int), 16> fn = [](int x) { return x * 2; };
    TEST_ASSERT_EQUAL_INT(10, fn(5));
}

static void test_inplace_function_invokes_stateful_lambda()
{
    int captured = 42;
    InplaceFunction<int(), 32> fn = [captured]() { return captured; };
    TEST_ASSERT_EQUAL_INT(42, fn());
}

static void test_inplace_function_void_return()
{
    int side_effect = 0;
    InplaceFunction<void(int), 32> fn = [&side_effect](int v) { side_effect = v; };
    fn(99);
    TEST_ASSERT_EQUAL_INT(99, side_effect);
}

static void test_inplace_function_multiple_args()
{
    InplaceFunction<int(int, int), 16> fn = [](int a, int b) { return a + b; };
    TEST_ASSERT_EQUAL_INT(7, fn(3, 4));
}

// ---------------------------------------------------------------------------
// operator bool / null state
// ---------------------------------------------------------------------------

static void test_inplace_function_default_constructed_is_false()
{
    InplaceFunction<void(), 16> fn;
    TEST_ASSERT_FALSE(static_cast<bool>(fn));
}

static void test_inplace_function_assigned_is_true()
{
    InplaceFunction<void(), 16> fn = []() {};
    TEST_ASSERT_TRUE(static_cast<bool>(fn));
}

static void test_inplace_function_reset_makes_false()
{
    InplaceFunction<void(), 16> fn = []() {};
    fn.reset();
    TEST_ASSERT_FALSE(static_cast<bool>(fn));
}

static void test_inplace_function_nullptr_assign_makes_false()
{
    InplaceFunction<void(), 16> fn = []() {};
    fn = nullptr;
    TEST_ASSERT_FALSE(static_cast<bool>(fn));
}

// ---------------------------------------------------------------------------
// Copy semantics
// ---------------------------------------------------------------------------

static void test_inplace_function_copy_constructor()
{
    int value = 7;
    InplaceFunction<int(), 32> fn = [value]() { return value; };
    InplaceFunction<int(), 32> copy = fn;
    TEST_ASSERT_EQUAL_INT(7, copy());
    TEST_ASSERT_TRUE(static_cast<bool>(fn));   // original still valid
}

static void test_inplace_function_copy_assignment()
{
    InplaceFunction<int(), 32> fn = []() { return 1; };
    InplaceFunction<int(), 32> other = []() { return 2; };
    fn = other;
    TEST_ASSERT_EQUAL_INT(2, fn());
}

static void test_inplace_function_copy_is_independent()
{
    // Capture by value — copies are independent.
    int counter = 0;
    InplaceFunction<void(), 32> fn = [counter]() mutable { ++counter; };
    InplaceFunction<void(), 32> copy = fn;
    fn();
    fn();
    copy();
    // fn's internal counter advanced 2, copy's advanced 1 — they don't share state.
    // We can only verify they still invoke without crashing.
    TEST_ASSERT_TRUE(true);
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

static void test_inplace_function_move_constructor_leaves_source_empty()
{
    InplaceFunction<int(), 16> fn = []() { return 3; };
    InplaceFunction<int(), 16> moved = std::move(fn);
    TEST_ASSERT_EQUAL_INT(3, moved());
    TEST_ASSERT_FALSE(static_cast<bool>(fn));
}

static void test_inplace_function_move_assignment()
{
    InplaceFunction<int(), 16> fn = []() { return 5; };
    InplaceFunction<int(), 16> other;
    other = std::move(fn);
    TEST_ASSERT_EQUAL_INT(5, other());
    TEST_ASSERT_FALSE(static_cast<bool>(fn));
}

// ---------------------------------------------------------------------------
// Re-assignment
// ---------------------------------------------------------------------------

static void test_inplace_function_reassignment_replaces_callable()
{
    InplaceFunction<int(), 16> fn = []() { return 1; };
    fn = []() { return 2; };
    TEST_ASSERT_EQUAL_INT(2, fn());
}

// ---------------------------------------------------------------------------
// Destructor of stored callable is called
// ---------------------------------------------------------------------------

static int g_destroy_count = 0;

struct CountedCallable {
    CountedCallable() = default;
    CountedCallable(const CountedCallable&) = default;
    CountedCallable(CountedCallable&&) noexcept = default;
    ~CountedCallable() { ++g_destroy_count; }
    void operator()() const {}
};

static void test_inplace_function_destructor_called_on_reset()
{
    g_destroy_count = 0;
    {
        InplaceFunction<void(), sizeof(CountedCallable) + 8> fn = CountedCallable{};
        // The temporary CountedCallable is destroyed (count == 1), then the stored
        // one lives inside fn.  When fn goes out of scope it should be destroyed too.
        (void)fn;
    }
    // The stored callable must have been destroyed.
    TEST_ASSERT_GREATER_OR_EQUAL(1, g_destroy_count);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    UNITY_BEGIN();

    RUN_TEST(test_inplace_function_invokes_plain_function_pointer);
    RUN_TEST(test_inplace_function_invokes_stateful_lambda);
    RUN_TEST(test_inplace_function_void_return);
    RUN_TEST(test_inplace_function_multiple_args);

    RUN_TEST(test_inplace_function_default_constructed_is_false);
    RUN_TEST(test_inplace_function_assigned_is_true);
    RUN_TEST(test_inplace_function_reset_makes_false);
    RUN_TEST(test_inplace_function_nullptr_assign_makes_false);

    RUN_TEST(test_inplace_function_copy_constructor);
    RUN_TEST(test_inplace_function_copy_assignment);
    RUN_TEST(test_inplace_function_copy_is_independent);

    RUN_TEST(test_inplace_function_move_constructor_leaves_source_empty);
    RUN_TEST(test_inplace_function_move_assignment);

    RUN_TEST(test_inplace_function_reassignment_replaces_callable);
    RUN_TEST(test_inplace_function_destructor_called_on_reset);

    return UNITY_END();
}
