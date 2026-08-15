// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/platform/virtual_test_clock.hpp"

#include <vector>

using knx::platform::VirtualTestClock;

void setUp() {}
void tearDown() {}

void test_VTCLOCK_001_monotonic_now_and_ms_projection()
{
    VirtualTestClock clock;
    TEST_ASSERT_EQUAL_UINT64(0u, clock.nowUs());
    TEST_ASSERT_EQUAL_UINT32(0u, clock.nowMs());

    clock.advanceUs(1500u);
    TEST_ASSERT_EQUAL_UINT64(1500u, clock.nowUs());
    TEST_ASSERT_EQUAL_UINT32(1u, clock.nowMs());

    clock.advanceMs(2u);
    TEST_ASSERT_EQUAL_UINT64(3500u, clock.nowUs());
    TEST_ASSERT_EQUAL_UINT32(3u, clock.nowMs());
}

void test_VTCLOCK_002_observers_receive_advanced_timestamp()
{
    VirtualTestClock clock;
    std::vector<uint64_t> observed;

    const auto observerId = clock.addAdvanceObserver([&observed](uint64_t nowUs) {
        observed.push_back(nowUs);
    });

    TEST_ASSERT_TRUE(observerId != 0u);

    clock.advanceUs(7u);
    clock.advanceUs(5u);

    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(observed.size()));
    TEST_ASSERT_EQUAL_UINT64(7u, observed[0]);
    TEST_ASSERT_EQUAL_UINT64(12u, observed[1]);
}

void test_VTCLOCK_003_removing_observer_stops_callbacks()
{
    VirtualTestClock clock;
    uint32_t callbackCount = 0u;

    const auto observerId = clock.addAdvanceObserver([&callbackCount](uint64_t) {
        ++callbackCount;
    });

    TEST_ASSERT_TRUE(clock.removeAdvanceObserver(observerId));
    TEST_ASSERT_FALSE(clock.removeAdvanceObserver(observerId));

    clock.advanceUs(10u);
    TEST_ASSERT_EQUAL_UINT32(0u, callbackCount);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_VTCLOCK_001_monotonic_now_and_ms_projection);
    RUN_TEST(test_VTCLOCK_002_observers_receive_advanced_timestamp);
    RUN_TEST(test_VTCLOCK_003_removing_observer_stops_callbacks);
    return UNITY_END();
}
