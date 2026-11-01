// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sami Mäkinen

/**
 * @file test_fixed_vector.cpp
 * @brief Unit tests for knx::util::FixedVector
 */

#include "knx/util/fixed_vector.hpp"

#include <unity.h>

#include <csignal>
#include <cstdint>
#include <memory>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>

using knx::util::FixedVector;

void setUp() {}
void tearDown() {}

namespace {

/// Counts live instances, so a test can see exactly when elements die.
struct Tracked {
    static inline int live = 0;
    int value;
    explicit Tracked(int v) : value(v) { ++live; }
    Tracked(const Tracked& other) : value(other.value) { ++live; }
    Tracked(Tracked&& other) noexcept : value(other.value) { ++live; }
    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    ~Tracked() { --live; }
};

/// No default constructor: the old std::array storage could not hold this.
struct NoDefault {
    explicit NoDefault(int v) : value(v) {}
    int value;
};

} // namespace

// Byte vectors are embedded in frames that travel through byte-copying
// queues, so they must stay trivially copyable as with the old std::array.
static_assert(std::is_trivially_copyable_v<FixedVector<uint8_t, 16>>);
static_assert(std::is_trivially_destructible_v<FixedVector<uint8_t, 16>>);
static_assert(!std::is_copy_constructible_v<FixedVector<std::unique_ptr<int>, 4>>);
static_assert(std::is_move_constructible_v<FixedVector<std::unique_ptr<int>, 4>>);

static void test_fixed_vector_push_and_index()
{
    FixedVector<uint8_t, 3> v;
    TEST_ASSERT_TRUE(v.empty());
    TEST_ASSERT_TRUE(v.push_back(1));
    TEST_ASSERT_TRUE(v.push_back(2));
    TEST_ASSERT_TRUE(v.emplace_back(3));
    TEST_ASSERT_TRUE(v.full());
    TEST_ASSERT_FALSE(v.push_back(4));
    TEST_ASSERT_EQUAL_UINT(3, v.size());
    TEST_ASSERT_EQUAL_UINT8(1, v.front());
    TEST_ASSERT_EQUAL_UINT8(3, v.back());
    TEST_ASSERT_EQUAL_UINT8(2, v[1]);
}

static void test_fixed_vector_assign_rejects_oversize_and_keeps_contents()
{
    FixedVector<uint8_t, 2> v{7};
    const uint8_t three[] = {1, 2, 3};
    TEST_ASSERT_FALSE(v.assign(std::span<const uint8_t>(three)));
    TEST_ASSERT_EQUAL_UINT(1, v.size());
    TEST_ASSERT_EQUAL_UINT8(7, v[0]);
}

static void test_fixed_vector_resize_value_initialises_new_elements()
{
    FixedVector<uint8_t, 4> v{9};
    v.resize(3);
    TEST_ASSERT_EQUAL_UINT(3, v.size());
    TEST_ASSERT_EQUAL_UINT8(9, v[0]);
    TEST_ASSERT_EQUAL_UINT8(0, v[1]);
    TEST_ASSERT_EQUAL_UINT8(0, v[2]);
    v.resize(10, 5);
    TEST_ASSERT_EQUAL_UINT(4, v.size());
    TEST_ASSERT_EQUAL_UINT8(5, v[3]);
}

static void test_fixed_vector_erase_shifts_down()
{
    FixedVector<int, 4> v{1, 2, 3, 4};
    v.erase(1);
    TEST_ASSERT_EQUAL_UINT(3, v.size());
    TEST_ASSERT_EQUAL_INT(1, v[0]);
    TEST_ASSERT_EQUAL_INT(3, v[1]);
    TEST_ASSERT_EQUAL_INT(4, v[2]);
    v.erase(7);
    TEST_ASSERT_EQUAL_UINT(3, v.size());
}

static void test_fixed_vector_removal_destroys_elements()
{
    Tracked::live = 0;
    {
        FixedVector<Tracked, 4> v;
        v.emplace_back(1);
        v.emplace_back(2);
        v.emplace_back(3);
        TEST_ASSERT_EQUAL_INT(3, Tracked::live);
        v.pop_back();
        TEST_ASSERT_EQUAL_INT(2, Tracked::live);
        v.erase(0);
        TEST_ASSERT_EQUAL_INT(1, Tracked::live);
        TEST_ASSERT_EQUAL_INT(2, v[0].value);
        v.clear();
        TEST_ASSERT_EQUAL_INT(0, Tracked::live);
        v.emplace_back(4);
        v.emplace_back(5);
        v.resize(1, Tracked(0));
        TEST_ASSERT_EQUAL_INT(1, Tracked::live);
    }
    TEST_ASSERT_EQUAL_INT(0, Tracked::live);
}

static void test_fixed_vector_unique_ptr_released_on_erase()
{
    auto shared = std::make_shared<int>(1);
    FixedVector<std::shared_ptr<int>, 4> v;
    v.push_back(shared);
    v.push_back(shared);
    TEST_ASSERT_EQUAL_INT(3, shared.use_count());
    v.erase(0);
    TEST_ASSERT_EQUAL_INT(2, shared.use_count());
    v.pop_back();
    TEST_ASSERT_EQUAL_INT(1, shared.use_count());

    FixedVector<std::unique_ptr<int>, 2> owners;
    owners.push_back(std::make_unique<int>(5));
    FixedVector<std::unique_ptr<int>, 2> moved = std::move(owners);
    TEST_ASSERT_EQUAL_UINT(1, moved.size());
    TEST_ASSERT_EQUAL_INT(5, *moved[0]);
}

static void test_fixed_vector_copy_is_independent()
{
    Tracked::live = 0;
    {
        FixedVector<Tracked, 3> a;
        a.emplace_back(1);
        a.emplace_back(2);
        FixedVector<Tracked, 3> b = a;
        TEST_ASSERT_EQUAL_INT(4, Tracked::live);
        b.pop_back();
        TEST_ASSERT_EQUAL_UINT(2, a.size());
        a = b;
        TEST_ASSERT_EQUAL_UINT(1, a.size());
        TEST_ASSERT_EQUAL_INT(2, Tracked::live);
    }
    TEST_ASSERT_EQUAL_INT(0, Tracked::live);
}

static void test_fixed_vector_holds_non_default_constructible()
{
    FixedVector<NoDefault, 2> v;
    v.emplace_back(42);
    TEST_ASSERT_EQUAL_INT(42, v.back().value);
}

/// Runs @p body in a child process and reports whether it died of SIGABRT.
template <typename Body>
static bool abortsInChild(Body body)
{
    const pid_t pid = fork();
    if (pid == 0) {
        body();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

static void test_fixed_vector_back_and_front_assert_on_empty()
{
#ifdef NDEBUG
    TEST_IGNORE_MESSAGE("assertions are compiled out in this build");
#else
    // back() on an empty vector used to read index SIZE_MAX.
    TEST_ASSERT_TRUE(abortsInChild([] {
        FixedVector<int, 2> v;
        volatile int x = v.back();
        (void)x;
    }));
    TEST_ASSERT_TRUE(abortsInChild([] {
        const FixedVector<int, 2> v;
        volatile int x = v.front();
        (void)x;
    }));
#endif
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_fixed_vector_push_and_index);
    RUN_TEST(test_fixed_vector_assign_rejects_oversize_and_keeps_contents);
    RUN_TEST(test_fixed_vector_resize_value_initialises_new_elements);
    RUN_TEST(test_fixed_vector_erase_shifts_down);
    RUN_TEST(test_fixed_vector_removal_destroys_elements);
    RUN_TEST(test_fixed_vector_unique_ptr_released_on_erase);
    RUN_TEST(test_fixed_vector_copy_is_independent);
    RUN_TEST(test_fixed_vector_holds_non_default_constructible);
    RUN_TEST(test_fixed_vector_back_and_front_assert_on_empty);
    return UNITY_END();
}
