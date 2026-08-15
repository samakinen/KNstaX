// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/platform/virtual_platform.hpp"
#include "../common/virtual_platform_test_utils.hpp"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

using knx::platform::QueueHandle;
using knx::platform::SemaphoreHandle;
using knx::platform::VirtualPlatform;
using knx::platform::VirtualTestClock;
using TaskSleepReason = VirtualPlatform::TaskSleepReason;
using TaskState = VirtualPlatform::TaskState;

namespace {

VirtualTestClock testClock;
VirtualPlatform* platform = nullptr;

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
    ThreadSignal finishedSignal;
    std::atomic<bool> completed{false};

    knx::testsupport::VirtualTimeWorker worker(testClock, [&]() {
        platform->delay(10u);
        completed.store(true);
        finishedSignal.notify();
    });

    // Wait until the worker is actually blocked in delayMicroseconds()
    // before advancing virtual time to avoid the lost-wakeup race.
    TEST_ASSERT_TRUE(platform->waitUntilActiveDelayCalls(1u));
    TEST_ASSERT_FALSE(completed.load());

    testClock.advanceMs(9u);
    TEST_ASSERT_FALSE(completed.load());

    testClock.advanceMs(1u);
    finishedSignal.wait();
    worker.join();

    TEST_ASSERT_TRUE(completed.load());
    TEST_ASSERT_EQUAL_UINT32(10u, platform->millis());
}

void test_VPLAT_002_queue_receive_unblocks_on_virtual_signal_before_timeout()
{
    QueueHandle queue = platform->createQueue(sizeof(uint32_t), 1u);
    TEST_ASSERT_NOT_NULL(queue);

    ThreadSignal armed;
    ThreadSignal finishedSignal;
    std::atomic<bool> finished{false};
    std::atomic<bool> receivedOk{false};
    uint32_t receivedValue = 0u;

    knx::testsupport::VirtualTimeWorker worker(testClock, [&]() {
        (void)(platform->micros() + 50000u);
        armed.notify();
        uint32_t localValue = 0u;
        auto result = platform->queueReceive(queue, &localValue, 50u);
        receivedValue = localValue;
        receivedOk.store(result.isOk());
        finished.store(true);
        finishedSignal.notify();
    });

    armed.wait();
    // armed.notify() fires before queueReceive(), which latches its virtual
    // deadline on entry — wait for the worker to actually be parked before
    // moving the clock, or the deadline is computed past every value the clock
    // will reach.
    TEST_ASSERT_TRUE(platform->waitUntilActiveWaiters(1u));
    testClock.advanceMs(10u);

    const uint32_t sendValue = 0x1234ABCDu;
    TEST_ASSERT_TRUE(platform->queueSend(queue, &sendValue, 0u).isOk());

    finishedSignal.wait();
    worker.join();

    TEST_ASSERT_TRUE(finished.load());
    TEST_ASSERT_TRUE(receivedOk.load());
    TEST_ASSERT_EQUAL_HEX32(sendValue, receivedValue);
    platform->deleteQueue(queue);
}

void test_VPLAT_003_queue_receive_times_out_on_virtual_deadline()
{
    QueueHandle queue = platform->createQueue(sizeof(uint32_t), 1u);
    TEST_ASSERT_NOT_NULL(queue);

    ThreadSignal armed;
    ThreadSignal finishedSignal;
    std::atomic<bool> finished{false};
    std::atomic<bool> timedOut{false};

    knx::testsupport::VirtualTimeWorker worker(testClock, [&]() {
        (void)(platform->micros() + 5000u);
        armed.notify();
        uint32_t localValue = 0u;
        auto result = platform->queueReceive(queue, &localValue, 5u);
        timedOut.store(result.isError());
        finished.store(true);
        finishedSignal.notify();
    });

    armed.wait();
    // Nothing ever satisfies this queue's predicate — the worker can only be
    // released by the virtual deadline it latches inside queueReceive(). If the
    // clock moves first that deadline becomes unreachable and the worker blocks
    // forever on a condition variable with no wall-clock timeout.
    TEST_ASSERT_TRUE(platform->waitUntilActiveWaiters(1u));
    testClock.advanceMs(4u);
    TEST_ASSERT_FALSE(timedOut.load());

    testClock.advanceMs(1u);
    finishedSignal.wait();
    worker.join();

    TEST_ASSERT_TRUE(finished.load());
    TEST_ASSERT_TRUE(timedOut.load());
    platform->deleteQueue(queue);
}

void test_VPLAT_004_semaphore_take_released_by_virtual_progress_and_give()
{
    SemaphoreHandle semaphore = platform->createBinarySemaphore();
    TEST_ASSERT_NOT_NULL(semaphore);

    ThreadSignal armed;
    ThreadSignal finishedSignal;
    std::atomic<bool> finished{false};
    std::atomic<bool> acquired{false};

    knx::testsupport::VirtualTimeWorker worker(testClock, [&]() {
        (void)(platform->micros() + 20000u);
        armed.notify();
        auto result = platform->semaphoreTake(semaphore, 20u);
        acquired.store(result.isOk());
        finished.store(true);
        finishedSignal.notify();
    });

    armed.wait();
    // Same latched-deadline race as the queue tests above.
    TEST_ASSERT_TRUE(platform->waitUntilActiveWaiters(1u));
    testClock.advanceMs(7u);
    TEST_ASSERT_TRUE(platform->semaphoreGive(semaphore).isOk());
    finishedSignal.wait();
    worker.join();

    TEST_ASSERT_TRUE(finished.load());
    TEST_ASSERT_TRUE(acquired.load());
    platform->deleteSemaphore(semaphore);
}

void test_VPLAT_005_created_tasks_follow_deterministic_virtual_schedule()
{
    std::atomic<uint32_t> phase{0u};

    knx::platform::TaskConfig config{};
    config.name = "deterministic_task";
    config.function = [&]() {
        phase.store(1u);
        platform->taskDelay(5u);
        phase.store(2u);
        platform->taskDelay(5u);
        phase.store(3u);
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase.load());
    const auto initialSnapshot = platform->schedulerSnapshot();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(initialSnapshot.taskCount));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(initialSnapshot.sleepingTaskCount));

    platform->advanceTimeMs(4u);
    TEST_ASSERT_EQUAL_UINT32(1u, phase.load());

    platform->advanceTimeMs(1u);
    TEST_ASSERT_EQUAL_UINT32(2u, phase.load());

    platform->advanceTimeMs(5u);
    TEST_ASSERT_EQUAL_UINT32(3u, phase.load());

    platform->deleteTask(task);
}

void test_VPLAT_005A_scheduler_snapshots_expose_task_sleep_reason()
{
    std::atomic<uint32_t> phase{0u};

    knx::platform::TaskConfig config{};
    config.name = "snapshot_task";
    config.function = [&]() {
        phase.store(1u);
        platform->taskDelay(7u);
        phase.store(2u);
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase.load());

    const auto snapshots = platform->taskSnapshots();
    const auto* taskSnapshot = knx::testsupport::findTaskByName(snapshots, "snapshot_task");
    TEST_ASSERT_NOT_NULL(taskSnapshot);
    TEST_ASSERT_EQUAL(task, taskSnapshot->handle);
    TEST_ASSERT_TRUE(taskSnapshot->running);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TaskState::Sleeping), static_cast<uint8_t>(taskSnapshot->state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TaskSleepReason::Delay), static_cast<uint8_t>(taskSnapshot->sleepReason));
    TEST_ASSERT_EQUAL_UINT64(7000u, taskSnapshot->wakeDeadlineUs);

    platform->deleteTask(task);
}

void test_VPLAT_006_created_task_queue_receive_uses_deterministic_wakeup()
{
    QueueHandle queue = platform->createQueue(sizeof(uint32_t), 1u);
    TEST_ASSERT_NOT_NULL(queue);

    std::atomic<uint32_t> phase{0u};
    std::atomic<uint32_t> receivedValue{0u};

    knx::platform::TaskConfig config{};
    config.name = "queue_wait_task";
    config.function = [&]() {
        phase.store(1u);
        uint32_t localValue = 0u;
        const auto result = platform->queueReceive(queue, &localValue, 20u);
        if (result.isOk()) {
            receivedValue.store(localValue);
            phase.store(2u);
        } else {
            phase.store(99u);
        }
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase.load());

    const uint32_t sendValue = 0xA1B2C3D4u;
    TEST_ASSERT_TRUE(platform->queueSend(queue, &sendValue, 0u).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, phase.load());
    TEST_ASSERT_EQUAL_HEX32(sendValue, receivedValue.load());

    platform->deleteTask(task);
    platform->deleteQueue(queue);
}

void test_VPLAT_007_created_task_semaphore_take_uses_deterministic_wakeup()
{
    SemaphoreHandle semaphore = platform->createBinarySemaphore();
    TEST_ASSERT_NOT_NULL(semaphore);

    std::atomic<uint32_t> phase{0u};

    knx::platform::TaskConfig config{};
    config.name = "semaphore_wait_task";
    config.function = [&]() {
        phase.store(1u);
        const auto result = platform->semaphoreTake(semaphore, 30u);
        phase.store(result.isOk() ? 2u : 99u);
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase.load());

    TEST_ASSERT_TRUE(platform->semaphoreGive(semaphore).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, phase.load());

    platform->deleteTask(task);
    platform->deleteSemaphore(semaphore);
}

void test_VPLAT_008_created_task_event_group_wait_uses_deterministic_wakeup()
{
    auto eventGroup = platform->createEventGroup();
    TEST_ASSERT_NOT_NULL(eventGroup);

    std::atomic<uint32_t> phase{0u};
    std::atomic<uint32_t> observedBits{0u};

    knx::platform::TaskConfig config{};
    config.name = "event_group_wait_task";
    config.function = [&]() {
        phase.store(1u);
        observedBits.store(platform->eventGroupWaitBits(eventGroup,
                                                        0x03u,
                                                        knx::platform::EventGroupClearMode::Clear,
                                                        knx::platform::EventGroupWaitMode::All,
                                                        40u));
        phase.store(observedBits.load() == 0x03u ? 2u : 99u);
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_UINT32(1u, phase.load());

    platform->eventGroupSetBits(eventGroup, 0x01u);
    TEST_ASSERT_EQUAL_UINT32(1u, phase.load());
    platform->eventGroupSetBits(eventGroup, 0x02u);
    TEST_ASSERT_EQUAL_UINT32(2u, phase.load());
    TEST_ASSERT_EQUAL_HEX32(0x03u, observedBits.load());

    platform->deleteTask(task);
    platform->deleteEventGroup(eventGroup);
}

void test_VPLAT_009_created_task_notify_take_uses_deterministic_wakeup()
{
    std::atomic<uint32_t> phase{0u};
    std::atomic<uint32_t> notifiedValue{0u};
    std::atomic<knx::platform::TaskHandle> taskHandle{nullptr};

    knx::platform::TaskConfig config{};
    config.name = "notify_wait_task";
    config.function = [&]() {
        taskHandle.store(platform->currentTask());
        phase.store(1u);
        notifiedValue.store(platform->taskNotifyTake(knx::platform::TaskNotifyClearMode::Clear, 50u));
        phase.store(notifiedValue.load() == 1u ? 2u : 99u);
    };

    const auto task = platform->createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL(task, taskHandle.load());
    TEST_ASSERT_EQUAL_UINT32(1u, phase.load());

    TEST_ASSERT_TRUE(platform->taskNotifyGive(task).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, phase.load());
    TEST_ASSERT_EQUAL_UINT32(1u, notifiedValue.load());

    platform->deleteTask(task);
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
    return UNITY_END();
}
