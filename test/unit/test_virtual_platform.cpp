// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/platform/virtual_platform.hpp"
#include "../common/virtual_platform_test_utils.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using knx::platform::QueueHandle;
using knx::platform::SemaphoreHandle;
using knx::platform::VirtualPlatform;
using knx::platform::VirtualTestClock;
using knx::testsupport::ScopedVirtualTask;
using TaskSleepReason = VirtualPlatform::TaskSleepReason;
using TaskState = VirtualPlatform::TaskState;

namespace {

VirtualTestClock testClock;
VirtualPlatform* platform = nullptr;

} // namespace

void setUp()
{
    testClock.reset();
    platform = new VirtualPlatform(testClock);
}

void tearDown()
{
    delete platform;
    platform = nullptr;
}

void test_VPLAT_001_delay_waits_for_virtual_time_only()
{
    bool completed = false;

    ScopedVirtualTask worker(*platform, "delay_worker", [&]() {
        platform->delay(10u);
        completed = true;
    });

    TEST_ASSERT_TRUE(platform->waitUntilActiveDelayCalls(1u));
    TEST_ASSERT_FALSE(completed);

    testClock.advanceMs(9u);
    TEST_ASSERT_FALSE(completed);

    testClock.advanceMs(1u);
    TEST_ASSERT_TRUE(completed);
    TEST_ASSERT_EQUAL_UINT32(10u, platform->millis());
}

void test_VPLAT_002_queue_receive_unblocks_on_virtual_signal_before_timeout()
{
    QueueHandle queue = platform->createQueue(sizeof(uint32_t), 1u);
    TEST_ASSERT_NOT_NULL(queue);

    bool finished = false;
    bool receivedOk = false;
    uint32_t receivedValue = 0u;

    ScopedVirtualTask worker(*platform, "queue_worker", [&]() {
        auto result = platform->queueReceive(queue, &receivedValue, 50u);
        receivedOk = result.isOk();
        finished = true;
    });

    TEST_ASSERT_TRUE(platform->waitUntilActiveWaiters(1u));
    testClock.advanceMs(10u);
    TEST_ASSERT_FALSE(finished);

    const uint32_t sendValue = 0x1234ABCDu;
    TEST_ASSERT_TRUE(platform->queueSend(queue, &sendValue, 0u).isOk());

    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_TRUE(receivedOk);
    TEST_ASSERT_EQUAL_HEX32(sendValue, receivedValue);
    TEST_ASSERT_EQUAL_UINT32(10u, platform->millis());
    platform->deleteQueue(queue);
}

void test_VPLAT_003_queue_receive_times_out_on_virtual_deadline()
{
    QueueHandle queue = platform->createQueue(sizeof(uint32_t), 1u);
    TEST_ASSERT_NOT_NULL(queue);

    bool finished = false;
    bool timedOut = false;

    ScopedVirtualTask worker(*platform, "timeout_worker", [&]() {
        uint32_t localValue = 0u;
        auto result = platform->queueReceive(queue, &localValue, 5u);
        timedOut = result.isError();
        finished = true;
    });

    TEST_ASSERT_TRUE(platform->waitUntilActiveWaiters(1u));
    testClock.advanceMs(4u);
    TEST_ASSERT_FALSE(finished);

    testClock.advanceMs(1u);
    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_TRUE(timedOut);
    platform->deleteQueue(queue);
}

void test_VPLAT_004_semaphore_take_released_by_virtual_progress_and_give()
{
    SemaphoreHandle semaphore = platform->createBinarySemaphore();
    TEST_ASSERT_NOT_NULL(semaphore);

    bool finished = false;
    bool acquired = false;

    ScopedVirtualTask worker(*platform, "semaphore_worker", [&]() {
        auto result = platform->semaphoreTake(semaphore, 20u);
        acquired = result.isOk();
        finished = true;
    });

    TEST_ASSERT_TRUE(platform->waitUntilActiveWaiters(1u));
    testClock.advanceMs(7u);
    TEST_ASSERT_FALSE(finished);
    TEST_ASSERT_TRUE(platform->semaphoreGive(semaphore).isOk());

    TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_TRUE(acquired);
    platform->deleteSemaphore(semaphore);
}

void test_VPLAT_005_created_tasks_follow_deterministic_virtual_schedule()
{
    uint32_t phase = 0u;

    knx::platform::TaskConfig config{};
    config.name = "deterministic_task";
    config.function = [&]() {
        phase = 1u;
        platform->taskDelay(5u);
        phase = 2u;
        platform->taskDelay(5u);
        phase = 3u;
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase);
    const auto initialSnapshot = platform->schedulerSnapshot();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(initialSnapshot.taskCount));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(initialSnapshot.sleepingTaskCount));

    platform->advanceTimeMs(4u);
    TEST_ASSERT_EQUAL_UINT32(1u, phase);

    platform->advanceTimeMs(1u);
    TEST_ASSERT_EQUAL_UINT32(2u, phase);

    platform->advanceTimeMs(5u);
    TEST_ASSERT_EQUAL_UINT32(3u, phase);

    platform->deleteTask(task);
}

void test_VPLAT_005A_scheduler_snapshots_expose_task_sleep_reason()
{
    uint32_t phase = 0u;

    knx::platform::TaskConfig config{};
    config.name = "snapshot_task";
    config.function = [&]() {
        phase = 1u;
        platform->taskDelay(7u);
        phase = 2u;
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase);

    const auto snapshots = platform->taskSnapshots();
    const auto* taskSnapshot = knx::testsupport::findTaskByName(snapshots, "snapshot_task");
    TEST_ASSERT_NOT_NULL(taskSnapshot);
    TEST_ASSERT_EQUAL(task, taskSnapshot->handle);
    TEST_ASSERT_TRUE(taskSnapshot->running);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TaskState::Sleeping), static_cast<uint8_t>(taskSnapshot->state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TaskSleepReason::Delay), static_cast<uint8_t>(taskSnapshot->sleepReason));
    TEST_ASSERT_EQUAL_UINT64(7000u, taskSnapshot->wakeDeadlineUs);

    // Deleting a task that is still blocked discards it: it never runs again.
    platform->deleteTask(task);
    platform->advanceTimeMs(10u);
    TEST_ASSERT_EQUAL_UINT32(1u, phase);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(platform->taskSnapshots().size()));
}

void test_VPLAT_006_created_task_queue_receive_uses_deterministic_wakeup()
{
    QueueHandle queue = platform->createQueue(sizeof(uint32_t), 1u);
    TEST_ASSERT_NOT_NULL(queue);

    uint32_t phase = 0u;
    uint32_t receivedValue = 0u;

    knx::platform::TaskConfig config{};
    config.name = "queue_wait_task";
    config.function = [&]() {
        phase = 1u;
        uint32_t localValue = 0u;
        const auto result = platform->queueReceive(queue, &localValue, 20u);
        if (result.isOk()) {
            receivedValue = localValue;
            phase = 2u;
        } else {
            phase = 99u;
        }
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase);

    const uint32_t sendValue = 0xA1B2C3D4u;
    TEST_ASSERT_TRUE(platform->queueSend(queue, &sendValue, 0u).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, phase);
    TEST_ASSERT_EQUAL_HEX32(sendValue, receivedValue);

    platform->deleteTask(task);
    platform->deleteQueue(queue);
}

void test_VPLAT_007_created_task_semaphore_take_uses_deterministic_wakeup()
{
    SemaphoreHandle semaphore = platform->createBinarySemaphore();
    TEST_ASSERT_NOT_NULL(semaphore);

    uint32_t phase = 0u;

    knx::platform::TaskConfig config{};
    config.name = "semaphore_wait_task";
    config.function = [&]() {
        phase = 1u;
        const auto result = platform->semaphoreTake(semaphore, 30u);
        phase = result.isOk() ? 2u : 99u;
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase);

    TEST_ASSERT_TRUE(platform->semaphoreGive(semaphore).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, phase);

    platform->deleteTask(task);
    platform->deleteSemaphore(semaphore);
}

void test_VPLAT_008_created_task_event_group_wait_uses_deterministic_wakeup()
{
    auto eventGroup = platform->createEventGroup();
    TEST_ASSERT_NOT_NULL(eventGroup);

    uint32_t phase = 0u;
    uint32_t observedBits = 0u;

    knx::platform::TaskConfig config{};
    config.name = "event_group_wait_task";
    config.function = [&]() {
        phase = 1u;
        observedBits = platform->eventGroupWaitBits(eventGroup,
                                                    0x03u,
                                                    knx::platform::EventGroupClearMode::Clear,
                                                    knx::platform::EventGroupWaitMode::All,
                                                    40u);
        phase = (observedBits == 0x03u) ? 2u : 99u;
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase);

    platform->eventGroupSetBits(eventGroup, 0x01u);
    TEST_ASSERT_EQUAL_UINT32(1u, phase);
    platform->eventGroupSetBits(eventGroup, 0x02u);
    TEST_ASSERT_EQUAL_UINT32(2u, phase);
    TEST_ASSERT_EQUAL_HEX32(0x03u, observedBits);

    platform->deleteTask(task);
    platform->deleteEventGroup(eventGroup);
}

void test_VPLAT_009_created_task_notify_take_uses_deterministic_wakeup()
{
    uint32_t phase = 0u;
    uint32_t notifiedValue = 0u;
    knx::platform::TaskHandle taskHandle = nullptr;

    knx::platform::TaskConfig config{};
    config.name = "notify_wait_task";
    config.function = [&]() {
        taskHandle = platform->currentTask();
        phase = 1u;
        notifiedValue = platform->taskNotifyTake(knx::platform::TaskNotifyClearMode::Clear, 50u);
        phase = (notifiedValue == 1u) ? 2u : 99u;
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL(task, taskHandle);
    TEST_ASSERT_EQUAL_UINT32(1u, phase);
    TEST_ASSERT_NULL(platform->currentTask());

    TEST_ASSERT_TRUE(platform->taskNotifyGive(task).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, phase);
    TEST_ASSERT_EQUAL_UINT32(1u, notifiedValue);

    platform->deleteTask(task);
}

void test_VPLAT_010_advance_stops_at_every_task_deadline()
{
    std::vector<uint64_t> wakeTimesUs;

    ScopedVirtualTask ticker(*platform, "ticker", [&]() {
        for (int i = 0; i < 5; ++i) {
            platform->taskDelay(1u);
            wakeTimesUs.push_back(platform->micros());
        }
    });

    platform->advanceTimeUs(3500u);
    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(wakeTimesUs.size()));
    TEST_ASSERT_EQUAL_UINT64(1000u, wakeTimesUs[0]);
    TEST_ASSERT_EQUAL_UINT64(2000u, wakeTimesUs[1]);
    TEST_ASSERT_EQUAL_UINT64(3000u, wakeTimesUs[2]);
    TEST_ASSERT_EQUAL_UINT64(3500u, platform->micros());
}

void test_VPLAT_011_runnable_tasks_run_by_priority_then_longest_waiting()
{
    std::string order;
    auto eventGroup = platform->createEventGroup();

    const auto makeTask = [&](const char* name, uint32_t priority, char tag) {
        knx::platform::TaskConfig config{};
        config.name = name;
        config.priority = priority;
        config.function = [&, tag]() {
            (void)platform->eventGroupWaitBits(eventGroup, 0x01u, knx::platform::EventGroupClearMode::Keep,
                                               knx::platform::EventGroupWaitMode::Any, UINT32_MAX);
            order.push_back(tag);
            platform->taskYield();
            order.push_back(tag);
        };
        return platform->createTask(config);
    };

    const auto low1 = makeTask("low1", 1u, 'a');
    const auto high = makeTask("high", 5u, 'H');
    const auto low2 = makeTask("low2", 1u, 'b');
    TEST_ASSERT_TRUE(platform->waitUntilActiveWaiters(3u));

    platform->eventGroupSetBits(eventGroup, 0x01u);
    // The high-priority task runs to completion first (its yield finds no
    // better candidate); the two equal-priority tasks then alternate.
    TEST_ASSERT_EQUAL_STRING("HHabab", order.c_str());

    platform->deleteTask(low1);
    platform->deleteTask(high);
    platform->deleteTask(low2);
    platform->deleteEventGroup(eventGroup);
}

void test_VPLAT_012_driver_delay_advances_virtual_time()
{
    uint32_t wakeups = 0u;
    ScopedVirtualTask ticker(*platform, "ticker", [&]() {
        for (;;) {
            platform->taskDelay(2u);
            ++wakeups;
        }
    });

    platform->delay(7u);
    TEST_ASSERT_EQUAL_UINT32(7u, platform->millis());
    TEST_ASSERT_EQUAL_UINT32(3u, wakeups);
}

void test_VPLAT_013_driver_wait_that_needs_time_fails_loudly()
{
    QueueHandle queue = platform->createQueue(sizeof(uint32_t), 1u);
    uint32_t value = 0u;

    // A zero timeout is a poll and simply fails.
    TEST_ASSERT_TRUE(platform->queueReceive(queue, &value, 0u).isError());

    // Satisfied by a task that has already run: driver calls leave no task
    // runnable behind them, so the sender filled the queue in createTask().
    const uint32_t sent = 42u;
    ScopedVirtualTask sender(*platform, "sender", [&]() {
        platform->taskYield();
        (void)platform->queueSend(queue, &sent, 0u);
        platform->taskDelay(100u);
    });
    TEST_ASSERT_TRUE(platform->queueReceive(queue, &value, 10u).isOk());
    TEST_ASSERT_EQUAL_UINT32(42u, value);

    // Nothing will: the wait would hang on virtual time nobody advances.
    bool threw = false;
    try {
        (void)platform->queueReceive(queue, &value, 10u);
    } catch (const std::logic_error&) {
        threw = true;
    }
    TEST_ASSERT_TRUE(threw);
    platform->deleteQueue(queue);
}

void test_VPLAT_014_exception_in_task_reaches_the_driver()
{
    bool caught = false;
    knx::platform::TaskConfig config{};
    config.name = "thrower";
    config.function = [&]() {
        platform->taskDelay(3u);
        throw std::runtime_error("boom");
    };
    const auto task = platform->createTask(config);

    try {
        platform->advanceTimeMs(5u);
    } catch (const std::runtime_error& error) {
        caught = std::string(error.what()) == "boom";
    }
    TEST_ASSERT_TRUE(caught);
    // The task ended at the throw; the advance stopped there.
    TEST_ASSERT_EQUAL_UINT32(3u, platform->millis());
    const auto snapshots = platform->taskSnapshots();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(snapshots.size()));
    TEST_ASSERT_FALSE(snapshots[0].running);
    platform->deleteTask(task);
}

void test_VPLAT_015_task_can_delete_itself_and_others()
{
    bool afterSelfDelete = false;
    bool victimRan = false;

    knx::platform::TaskConfig victimConfig{};
    victimConfig.name = "victim";
    victimConfig.function = [&]() {
        platform->taskDelay(5u);
        victimRan = true;
    };
    const auto victim = platform->createTask(victimConfig);

    knx::platform::TaskConfig killerConfig{};
    killerConfig.name = "killer";
    killerConfig.function = [&]() {
        platform->taskDelay(1u);
        platform->deleteTask(victim);
        platform->deleteTask(nullptr);
        afterSelfDelete = true;
    };
    (void)platform->createTask(killerConfig);

    platform->advanceTimeMs(10u);
    TEST_ASSERT_FALSE(victimRan);
    TEST_ASSERT_FALSE(afterSelfDelete);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(platform->taskSnapshots().size()));
}

void test_VPLAT_016_mutex_hands_over_between_tasks()
{
    auto mutex = platform->createMutex();
    std::string order;

    ScopedVirtualTask first(*platform, "first", [&]() {
        TEST_ASSERT_TRUE(platform->lockMutex(mutex).isOk());
        order.push_back('1');
        platform->taskDelay(5u);
        order.push_back('2');
        platform->unlockMutex(mutex);
    });
    ScopedVirtualTask second(*platform, "second", [&]() {
        TEST_ASSERT_TRUE(platform->lockMutex(mutex).isOk());
        order.push_back('3');
        platform->unlockMutex(mutex);
    });

    TEST_ASSERT_EQUAL_STRING("1", order.c_str());
    TEST_ASSERT_TRUE(platform->waitUntilActiveWaiters(1u));
    platform->advanceTimeMs(5u);
    TEST_ASSERT_EQUAL_STRING("123", order.c_str());
    platform->deleteMutex(mutex);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_VPLAT_001_delay_waits_for_virtual_time_only);
    RUN_TEST(test_VPLAT_002_queue_receive_unblocks_on_virtual_signal_before_timeout);
    RUN_TEST(test_VPLAT_003_queue_receive_times_out_on_virtual_deadline);
    RUN_TEST(test_VPLAT_004_semaphore_take_released_by_virtual_progress_and_give);
    RUN_TEST(test_VPLAT_005_created_tasks_follow_deterministic_virtual_schedule);
    RUN_TEST(test_VPLAT_005A_scheduler_snapshots_expose_task_sleep_reason);
    RUN_TEST(test_VPLAT_006_created_task_queue_receive_uses_deterministic_wakeup);
    RUN_TEST(test_VPLAT_007_created_task_semaphore_take_uses_deterministic_wakeup);
    RUN_TEST(test_VPLAT_008_created_task_event_group_wait_uses_deterministic_wakeup);
    RUN_TEST(test_VPLAT_009_created_task_notify_take_uses_deterministic_wakeup);
    RUN_TEST(test_VPLAT_010_advance_stops_at_every_task_deadline);
    RUN_TEST(test_VPLAT_011_runnable_tasks_run_by_priority_then_longest_waiting);
    RUN_TEST(test_VPLAT_012_driver_delay_advances_virtual_time);
    RUN_TEST(test_VPLAT_013_driver_wait_that_needs_time_fails_loudly);
    RUN_TEST(test_VPLAT_014_exception_in_task_reaches_the_driver);
    RUN_TEST(test_VPLAT_015_task_can_delete_itself_and_others);
    RUN_TEST(test_VPLAT_016_mutex_hands_over_between_tasks);
    return UNITY_END();
}
