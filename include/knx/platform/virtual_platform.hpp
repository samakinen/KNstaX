// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/platform/memory_interface.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/platform/virtual_test_clock.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace knx {
namespace platform {

/**
 * @brief Deterministic host platform driven by virtual time.
 *
 * Everything runs on one OS thread, the one that constructed the platform.
 *
 * Tasks are fibres: createTask() gives each its own stack, and every blocking
 * call a task makes (delay, taskDelay, taskYield, queue, semaphore, mutex,
 * event group and notification waits) suspends the fibre and returns control
 * to the scheduler instead of blocking the thread. A task only ever stops
 * running at such a call; there is no preemption.
 *
 * The code that is not inside a task -- the test body, or whatever owns the
 * platform -- is the "driver context", and drives the scheduler:
 *
 *  - Every call from the driver context that can make a task runnable
 *    (createTask, a give, send, set bits or notify, a clock advance, and the
 *    run/wait helpers below) runs every runnable task to its next blocking
 *    point before it returns. The same calls made from a task only mark the
 *    other task runnable; it runs once the calling task blocks.
 *  - Runnable tasks run highest priority first, and among equal priorities
 *    the one that has been suspended longest first. Nothing about the order
 *    depends on the host.
 *  - advanceTimeUs()/advanceTimeMs() step the clock from one task deadline to
 *    the next, so a task that sleeps 1 ms in a loop runs once per millisecond
 *    and sees the time it asked for. Advancing the VirtualTestClock directly
 *    jumps straight to the target and runs whatever is due there.
 *  - delay(), delayMicroseconds() and taskDelay() from the driver context
 *    advance virtual time by that amount (running tasks on the way): the
 *    driver is the only thing that can make time pass, and that is what it
 *    asked for.
 *  - A queue, semaphore, mutex, event group or notification wait from the
 *    driver context first runs the tasks; if the wait is still not satisfied
 *    and its timeout is not zero, it would have to wait for virtual time that
 *    nothing else will advance. That is a bug in the caller, and the call
 *    throws std::logic_error instead of hanging.
 *  - taskNotifyTake() from the driver context returns 0: only tasks have a
 *    notification value.
 *
 * An exception escaping a task function (a failed test assertion, restart(),
 * fatalError()) ends that task and is rethrown in the driver context, from
 * the call that was running the scheduler.
 *
 * deleteTask() on a task that has not returned discards it the way FreeRTOS
 * does: it never runs again, and objects on its stack are not destroyed.
 * Deleting a queue, semaphore, mutex or event group a task is still waiting
 * on is undefined, as on FreeRTOS.
 *
 * Calling into the platform from any other thread is a bug and aborts.
 */
class VirtualPlatform : public Platform {
public:
    enum class TaskState : uint8_t {
        Ready = 0,
        Running,
        Sleeping,
        Completed,
    };

    enum class TaskSleepReason : uint8_t {
        None = 0,
        Delay,
        PredicateWait,
    };

    struct TaskSnapshot {
        TaskHandle handle{nullptr};
        const char* name{nullptr};
        TaskState state{TaskState::Ready};
        TaskSleepReason sleepReason{TaskSleepReason::None};
        uint64_t wakeDeadlineUs{0};
        bool running{false};
        uint32_t notificationValue{0};
    };

    struct SchedulerSnapshot {
        TaskHandle scheduledTask{nullptr};
        size_t taskCount{0};
        size_t readyTaskCount{0};
        size_t sleepingTaskCount{0};
        size_t predicateWaitTaskCount{0};
        bool pumpActive{false};
        uint64_t nowUs{0};
    };

    /// Smallest fibre stack. TaskConfig::stackSize is sized for the target;
    /// host frames are larger, and much larger under AddressSanitizer.
    static constexpr size_t kMinTaskStackBytes = 256u * 1024u;

    explicit VirtualPlatform(VirtualTestClock& clock);
    ~VirtualPlatform() override;

    VirtualPlatform(const VirtualPlatform&) = delete;
    VirtualPlatform& operator=(const VirtualPlatform&) = delete;

    void restart() override;
    void fatalError() override;
    uint32_t millis() const override;
    uint64_t micros() const override;
    void delay(uint32_t ms) override;
    void delayMicroseconds(uint32_t us) override;
    uint32_t uniqueSerialNumber() const override;
    void macAddress(std::span<uint8_t, 6> mac) const override;

    TaskHandle createTask(const TaskConfig& config) override;
    void deleteTask(TaskHandle task) override;
    TaskHandle currentTask() override;
    void taskDelay(uint32_t ms) override;
    void taskYield() override;

    MutexHandle createMutex() override;
    void deleteMutex(MutexHandle mutex) override;
    util::Result<void> lockMutex(MutexHandle mutex, uint32_t timeout_ms = UINT32_MAX) override;
    void unlockMutex(MutexHandle mutex) override;

    QueueHandle createQueue(size_t itemSize, size_t length) override;
    void deleteQueue(QueueHandle queue) override;
    util::Result<void> queueSend(QueueHandle queue, const void* item, uint32_t timeoutMs = 0) override;
    util::Result<void> queueReceive(QueueHandle queue, void* item, uint32_t timeoutMs = 0) override;
    size_t queueCount(QueueHandle queue) override;

    SemaphoreHandle createBinarySemaphore() override;
    void deleteSemaphore(SemaphoreHandle semaphore) override;
    util::Result<void> semaphoreGive(SemaphoreHandle semaphore) override;
    util::Result<void> semaphoreTake(SemaphoreHandle semaphore, uint32_t timeoutMs = 0) override;

    EventGroupHandle createEventGroup() override;
    void deleteEventGroup(EventGroupHandle eventGroup) override;
    void eventGroupSetBits(EventGroupHandle eventGroup, uint32_t bits) override;
    void eventGroupClearBits(EventGroupHandle eventGroup, uint32_t bits) override;
    uint32_t eventGroupWaitBits(EventGroupHandle eventGroup,
                                uint32_t bits,
                                EventGroupClearMode clearOnExit,
                                EventGroupWaitMode waitAll,
                                uint32_t timeoutMs) override;

    util::Result<void> taskNotifyGive(TaskHandle task) override;
    util::Result<void> taskNotifyGiveFromISR(TaskHandle task) override;
    uint32_t taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t timeout_ms = UINT32_MAX) override;

    MemoryInterface& memory();
    NetworkInterface* network() override;

    void log(const char* level, const char* tag, const char* format, ...) override;

    /// Advance virtual time, stopping at every task deadline on the way and
    /// running what became due there. From a task, only moves the clock.
    void advanceTimeUs(uint64_t deltaUs);
    void advanceTimeMs(uint32_t deltaMs);

    /// Run every runnable task to its next blocking point, until none is left,
    /// without moving time. No-op from a task.
    void runUntilQuiescent();

    std::vector<TaskSnapshot> taskSnapshots() const;
    SchedulerSnapshot schedulerSnapshot() const noexcept;
    VirtualTestClock& clock() { return _clock; }
    const VirtualTestClock& clock() const { return _clock; }

    /**
     * Run the tasks until quiescent, then report whether at least @p n of them
     * are blocked in delay()/delayMicroseconds()/taskDelay().
     *
     * Nothing runs concurrently with the caller, so there is nothing to wait
     * for: false means the tasks did not get there, and asserting on it is
     * what the call is for.
     */
    [[nodiscard]] bool waitUntilActiveDelayCalls(size_t n);

    /**
     * As waitUntilActiveDelayCalls(), for tasks blocked in a queue, semaphore,
     * mutex, event group or notification wait.
     */
    [[nodiscard]] bool waitUntilActiveWaiters(size_t n);

private:
    struct Fibre;

    struct VirtualMutex {
        bool locked{false};
        TaskHandle owner{nullptr};
    };

    struct VirtualQueue {
        std::deque<std::vector<uint8_t>> items;
        size_t itemSize{0};
        size_t capacity{0};
    };

    struct VirtualSemaphore {
        uint32_t count{0};
    };

    struct VirtualEventGroup {
        uint32_t bits{0};
    };

    /// Non-owning view of the condition a blocked task waits for. It points
    /// into the waiting task's own stack frame, which outlives the wait.
    struct WaitCondition {
        const void* object{nullptr};
        bool (*test)(const void*){nullptr};
        bool operator()() const { return test(object); }
    };

    VirtualTestClock& _clock;
    VirtualTestClock::ObserverId _clockObserverId{0};
    const std::thread::id _ownerThread;

    std::map<TaskHandle, std::unique_ptr<Fibre>> _tasks;
    Fibre* _current{nullptr};
    bool _dispatching{false};
    uint64_t _suspendSequence{0};
    std::exception_ptr _pendingException;

    // The driver context's saved registers, and (under ASan) its stack.
    struct DriverContext;
    std::unique_ptr<DriverContext> _driver;

    std::map<MutexHandle, std::unique_ptr<VirtualMutex>> _mutexes;
    std::map<QueueHandle, std::unique_ptr<VirtualQueue>> _queues;
    std::map<SemaphoreHandle, std::unique_ptr<VirtualSemaphore>> _semaphores;
    std::map<EventGroupHandle, std::unique_ptr<VirtualEventGroup>> _eventGroups;
    uint64_t _nextHandleId{1};

    std::unique_ptr<MemoryInterface> _memory;
    uint32_t _serialNumber;
    uint8_t _macAddress[6];

    void requireOwnerThread_() const;
    bool inDriverContext_() const noexcept { return _current == nullptr; }
    void* generateHandle_();
    uint64_t deadlineAfterUs_(uint64_t delayUs) const;
    void onClockAdvanced_(uint64_t nowUs);

    template <typename Predicate>
    bool waitFor_(uint32_t timeoutMs, const Predicate& predicate);
    bool waitForCondition_(uint32_t timeoutMs, WaitCondition condition);
    void sleepCurrentUntil_(uint64_t deadlineUs);
    void yieldCurrent_();
    void suspendCurrent_();
    void wakeTasks_();
    void advanceTo_(uint64_t targetUs);
    bool isRunnable_(const Fibre& fibre, uint64_t nowUs) const;
    Fibre* pickRunnable_();
    void runFibre_(Fibre& fibre);
    void releaseFibre_(Fibre& fibre) noexcept;
    Fibre* findTask_(TaskHandle task) const;
    size_t countBlocked_(TaskSleepReason reason) const;

    static void fibreEntry_(unsigned int platformLo, unsigned int platformHi);
    [[noreturn]] void fibreMain_();
};

} // namespace platform
} // namespace knx
