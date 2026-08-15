// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/physical/timer_gpio_hal_virtual.hpp"
#include "knx/physical/virtual_tp1_test_runtime.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

using knx::physical::VirtualTp1TestRuntime;

namespace {

VirtualTp1TestRuntime runtime;

class ThreadSignal {
public:
    void notify()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _signaled = true;
        }
        _cv.notify_all();
    }

    void wait()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [this]() { return _signaled; });
    }

private:
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _signaled{false};
};

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
    ThreadSignal armed;
    ThreadSignal finishedSignal;
    std::atomic<bool> completed{false};

    std::thread worker([&]() {
        (void)(runtime.platform().micros() + 4000u);
        armed.notify();
        runtime.platform().delay(4u);
        completed.store(true);
        finishedSignal.notify();
    });

    armed.wait();
    TEST_ASSERT_FALSE(completed.load());

    runtime.advanceTimeMs(3u);
    TEST_ASSERT_FALSE(completed.load());
    TEST_ASSERT_EQUAL_UINT64(3000u, runtime.bus().nowUs());

    runtime.advanceTimeMs(1u);
    finishedSignal.wait();
    worker.join();

    TEST_ASSERT_TRUE(completed.load());
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
