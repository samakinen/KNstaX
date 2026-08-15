// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/platform/memory_interface.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/platform/spi_interface.hpp"
#include "knx/platform/uart_interface.hpp"
#include "knx/platform/virtual_test_clock.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace knx {
namespace platform {

class VirtualPlatform : public Platform {
public:
    enum class TaskModel : uint8_t {
        Deterministic = 0,
        HostThreaded,
    };

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
        TaskModel taskModel{TaskModel::Deterministic};
        TaskHandle scheduledTask{nullptr};
        size_t taskCount{0};
        size_t readyTaskCount{0};
        size_t sleepingTaskCount{0};
        size_t predicateWaitTaskCount{0};
        bool pumpActive{false};
        uint64_t nowUs{0};
    };

    explicit VirtualPlatform(VirtualTestClock& clock, TaskModel taskModel = TaskModel::Deterministic);
    ~VirtualPlatform() override;

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

    MemoryInterface& memory() override;
    NetworkInterface* network() override;
    UartInterface* uart() override;
    SpiInterface* spi() override;

    void log(const char* level, const char* tag, const char* format, ...) override;

    void advanceTimeUs(uint64_t deltaUs);
    void advanceTimeMs(uint32_t deltaMs);
    void pumpTasks();
    std::vector<TaskSnapshot> taskSnapshots() const;
    SchedulerSnapshot schedulerSnapshot() const noexcept;
    VirtualTestClock& clock() { return _clock; }
    const VirtualTestClock& clock() const { return _clock; }
    TaskModel taskModel() const noexcept { return _taskModel; }

    /**
     * Block until at least @p n calls to delayMicroseconds() are concurrently
     * blocked inside the condition-variable wait.  Use this in tests to
     * synchronise on the moment a worker thread is actually sleeping before
     * advancing virtual time, eliminating the lost-wakeup race.
     *
     * @return true if the count was reached, false if @p timeout expired first.
     *         Assert on the result: a silent timeout puts the caller straight
     *         back into the race it was trying to avoid.
     */
    [[nodiscard]] bool waitUntilActiveDelayCalls(
        size_t n,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    /**
     * Block until at least @p n threads are concurrently parked inside
     * waitForPredicate() — the blocking path behind queueSend/queueReceive,
     * semaphoreTake, lockMutex and eventGroupWaitBits.
     *
     * Those calls latch their virtual deadline on entry, so a test that
     * advances the clock before the worker gets that far leaves it waiting for
     * a deadline that will never arrive; the underlying condition variable has
     * no wall-clock timeout, so the worker hangs forever. Sync on this first.
     *
     * @return true if the count was reached, false if @p timeout expired.
     */
    [[nodiscard]] bool waitUntilActiveWaiters(
        size_t n,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

private:
    struct VirtualMutex {
        std::mutex mutex;
        bool locked{false};
        std::thread::id owner;
    };

    struct VirtualQueue {
        std::mutex mutex;
        std::deque<std::vector<uint8_t>> items;
        size_t itemSize{0};
        size_t capacity{0};
    };

    struct VirtualSemaphore {
        std::mutex mutex;
        uint32_t count{0};
    };

    struct VirtualEventGroup {
        std::mutex mutex;
        uint32_t bits{0};
    };

    struct TaskNotification {
        mutable std::mutex mutex;
        uint32_t value{0};
    };

    struct TaskWrapper {
        std::thread thread;
        const char* name{nullptr};
        TaskFunction function;
        std::atomic<bool> running{true};
        TaskNotification notification;
        std::mutex gateMutex;
        std::condition_variable gateCv;
        bool executionPermit{false};
        uint64_t wakeDeadlineUs{0};

        enum class State : uint8_t {
            Ready = 0,
            Running,
            Sleeping,
            Completed,
        };

        enum class SleepReason : uint8_t {
            None = 0,
            Delay,
            PredicateWait,
        };

        State state{State::Ready};
        SleepReason sleepReason{SleepReason::None};

        explicit TaskWrapper(const char* taskName, TaskFunction fn) noexcept
            : name(taskName)
            , function(std::move(fn))
        {
        }
    };

    VirtualTestClock& _clock;
    TaskModel _taskModel;
    VirtualTestClock::ObserverId _clockObserverId;
    std::atomic<uint64_t> _observedNowUs;
    mutable std::mutex _waitMutex;
    std::condition_variable _waitCv;
    std::atomic<size_t> _activeDelayCount{0};
    std::mutex _activeDelayMutex;
    std::condition_variable _activeDelayCv;

    std::atomic<size_t> _activeWaiterCount{0};
    std::mutex _activeWaiterMutex;
    std::condition_variable _activeWaiterCv;
    mutable std::mutex _schedulerMutex;
    std::condition_variable _schedulerCv;
    TaskHandle _scheduledTask;
    bool _pumpActive;

    mutable std::mutex _tasksMutex;
    std::map<TaskHandle, std::unique_ptr<TaskWrapper>> _tasks;
    std::mutex _syncMutex;
    std::map<MutexHandle, std::unique_ptr<VirtualMutex>> _mutexes;
    std::map<QueueHandle, std::unique_ptr<VirtualQueue>> _queues;
    std::map<SemaphoreHandle, std::unique_ptr<VirtualSemaphore>> _semaphores;
    std::map<EventGroupHandle, std::unique_ptr<VirtualEventGroup>> _eventGroups;
    uint64_t _nextHandleId;

    std::unique_ptr<MemoryInterface> _memory;
    uint32_t _serialNumber;
    uint8_t _macAddress[6];

    TaskHandle generateHandleLocked();
    uint64_t deadlineUsFromTimeoutMs(uint32_t timeoutMs) const;
    void notifyAllWaiters();
    void onClockAdvanced(uint64_t nowUs);
    bool usesDeterministicTaskModel_() const noexcept;
    TaskWrapper* currentTaskWrapper_();
    static constexpr TaskState toPublicTaskState_(TaskWrapper::State state) noexcept
    {
        switch (state) {
            case TaskWrapper::State::Ready:
                return TaskState::Ready;
            case TaskWrapper::State::Running:
                return TaskState::Running;
            case TaskWrapper::State::Sleeping:
                return TaskState::Sleeping;
            case TaskWrapper::State::Completed:
                return TaskState::Completed;
        }

        return TaskState::Completed;
    }

    static constexpr TaskSleepReason toPublicTaskSleepReason_(TaskWrapper::SleepReason reason) noexcept
    {
        switch (reason) {
            case TaskWrapper::SleepReason::None:
                return TaskSleepReason::None;
            case TaskWrapper::SleepReason::Delay:
                return TaskSleepReason::Delay;
            case TaskWrapper::SleepReason::PredicateWait:
                return TaskSleepReason::PredicateWait;
        }

        return TaskSleepReason::None;
    }

    void waitForExecutionPermit_(TaskWrapper& task);
    void yieldCurrentTaskUntil_(uint64_t wakeDeadlineUs, TaskWrapper::SleepReason reason);
    void markTaskCompleted_(TaskHandle task);
    void pumpTasks_();

    template <typename Predicate>
    bool waitForPredicate(std::unique_lock<std::mutex>& lock, uint32_t timeoutMs, Predicate predicate) {
        if (predicate()) {
            return true;
        }

        if (timeoutMs == 0) {
            return false;
        }

        const bool waitForever = (timeoutMs == UINT32_MAX);
        const uint64_t deadlineUs = waitForever ? 0u : deadlineUsFromTimeoutMs(timeoutMs);

        if (usesDeterministicTaskModel_() && currentTask() != nullptr) {
            while (true) {
                if (predicate()) {
                    return true;
                }
                if (!waitForever && _observedNowUs.load(std::memory_order_acquire) >= deadlineUs) {
                    return predicate();
                }

                lock.unlock();
                yieldCurrentTaskUntil_(waitForever ? std::numeric_limits<uint64_t>::max() : deadlineUs,
                                       TaskWrapper::SleepReason::PredicateWait);
                lock.lock();
            }
        }

        // Publish that this thread is parked on virtual time, so a test can
        // synchronise on it before advancing the clock (see
        // waitUntilActiveWaiters). The deadline above is already fixed at this
        // point, which is precisely what the test needs to guarantee.
        ActiveWaiterScope waiterScope(*this);

        while (true) {
            if (predicate()) {
                return true;
            }
            if (!waitForever && _observedNowUs.load(std::memory_order_acquire) >= deadlineUs) {
                return predicate();
            }
            _waitCv.wait(lock, [&]() {
                return predicate()
                       || (!waitForever
                           && _observedNowUs.load(std::memory_order_acquire) >= deadlineUs);
            });
        }
    }

    /// RAII counter for threads blocked inside waitForPredicate().
    class ActiveWaiterScope {
    public:
        explicit ActiveWaiterScope(VirtualPlatform& platform) : _platform(platform) {
            std::lock_guard<std::mutex> lock(_platform._activeWaiterMutex);
            _platform._activeWaiterCount.fetch_add(1u, std::memory_order_release);
            _platform._activeWaiterCv.notify_all();
        }
        ~ActiveWaiterScope() {
            std::lock_guard<std::mutex> lock(_platform._activeWaiterMutex);
            _platform._activeWaiterCount.fetch_sub(1u, std::memory_order_release);
        }
        ActiveWaiterScope(const ActiveWaiterScope&) = delete;
        ActiveWaiterScope& operator=(const ActiveWaiterScope&) = delete;

    private:
        VirtualPlatform& _platform;
    };
};

} // namespace platform
} // namespace knx
