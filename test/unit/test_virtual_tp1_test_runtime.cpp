// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/physical/timer_gpio_hal_virtual.hpp"
#include "knx/physical/virtual_tp1_test_runtime.hpp"
#include "../common/virtual_platform_test_utils.hpp"

using knx::physical::VirtualTp1TestRuntime;

namespace {

VirtualTp1TestRuntime runtime;

} // namespace

void setUp()
{
    runtime.reset();
}

void tearDown() {}

void test_VTRUN_001_runtime_advances_bus_and_platform_together()
{
    TEST_ASSERT_EQUAL_UINT64(0u, runtime.nowUs());
    TEST_ASSERT_EQUAL_UINT64(0u, runtime.bus().nowUs());
    TEST_ASSERT_EQUAL_UINT32(0u, runtime.platform().millis());

    runtime.advanceTimeUs(2500u);

    TEST_ASSERT_EQUAL_UINT64(2500u, runtime.nowUs());
    TEST_ASSERT_EQUAL_UINT64(2500u, runtime.bus().nowUs());
    TEST_ASSERT_EQUAL_UINT64(2500u, runtime.platform().micros());
    TEST_ASSERT_EQUAL_UINT32(2u, runtime.platform().millis());
}

void test_VTRUN_002_runtime_platform_delay_releases_on_shared_bus_time()
{
    bool completed = false;

    knx::testsupport::ScopedVirtualTask worker(runtime.platform(), "delay_worker", [&]() {
        runtime.platform().delay(4u);
        completed = true;
    });

    TEST_ASSERT_TRUE(runtime.platform().waitUntilActiveDelayCalls(1u));
    TEST_ASSERT_FALSE(completed);

    runtime.advanceTimeMs(3u);
    TEST_ASSERT_FALSE(completed);
    TEST_ASSERT_EQUAL_UINT64(3000u, runtime.bus().nowUs());

    runtime.advanceTimeMs(1u);
    TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_UINT64(4000u, runtime.bus().nowUs());
    TEST_ASSERT_EQUAL_UINT64(4000u, runtime.platform().micros());
}

void test_VTRUN_003_attached_bus_processes_scheduled_rx_on_runtime_advance()
{
    TEST_ASSERT_TRUE(runtime.bus().scheduleRxLevelAtUs(10u, 0u));
    TEST_ASSERT_EQUAL_UINT8(1u, runtime.bus().rxLevel());

    runtime.advanceTimeUs(9u);
    TEST_ASSERT_EQUAL_UINT8(1u, runtime.bus().rxLevel());

    runtime.advanceTimeUs(1u);
    TEST_ASSERT_EQUAL_UINT8(0u, runtime.bus().rxLevel());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_VTRUN_001_runtime_advances_bus_and_platform_together);
    RUN_TEST(test_VTRUN_002_runtime_platform_delay_releases_on_shared_bus_time);
    RUN_TEST(test_VTRUN_003_attached_bus_processes_scheduled_rx_on_runtime_advance);
    return UNITY_END();
}
