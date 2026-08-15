// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/platform/virtual_platform.hpp"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
thread_local knx::platform::TaskHandle g_virtualCurrentTask = nullptr;
}

namespace knx {
namespace platform {

namespace {

class VirtualRamMemory final : public MemoryInterface {
public:
    VirtualRamMemory()
        : _storage(64u * 1024u, 0u)
    {
    }

    MemoryType type() const override { return MemoryType::RAM; }
    size_t size() const override { return _storage.size(); }
    size_t pageSize() const override { return 1u; }
    size_t eraseBlockSize() const override { return 1u; }
    util::Result<void> init() override { return util::Result<void>::ok(); }

    uint32_t read(uint32_t address, std::span<uint8_t> buffer) override
    {
        if (buffer.size() == 0 || address >= _storage.size()) {
            return 0u;
        }
        const size_t maxLen = _storage.size() - address;
        const size_t toCopy = (buffer.size() > maxLen) ? maxLen : buffer.size();
        std::memcpy(buffer.data(), _storage.data() + address, toCopy);
        return static_cast<uint32_t>(toCopy);
    }

    uint32_t write(uint32_t address, std::span<const uint8_t> buffer) override
    {
        if (buffer.size() == 0 || address >= _storage.size()) {
            return 0u;
        }
        const size_t maxLen = _storage.size() - address;
        const size_t toCopy = (buffer.size() > maxLen) ? maxLen : buffer.size();
        std::memcpy(_storage.data() + address, buffer.data(), toCopy);
        return static_cast<uint32_t>(toCopy);
    }

    uint32_t write(uint32_t address, uint8_t value, size_t repeat) override
    {
        if (address > _storage.size() || repeat > (_storage.size() - address)) {
            return 0u;
        }
        std::fill_n(_storage.begin() + static_cast<std::ptrdiff_t>(address),
                    static_cast<std::ptrdiff_t>(repeat),
                    value);
        return static_cast<uint32_t>(repeat);
    }

    void commit() override {}

    util::Result<void> erase(uint32_t address, size_t length) override
    {
        if (address > _storage.size() || length > (_storage.size() - address)) {
            return util::ErrorCode::InvalidParameter;
        }
        std::fill_n(_storage.begin() + static_cast<std::ptrdiff_t>(address),
                    static_cast<std::ptrdiff_t>(length),
                    0u);
        return util::Result<void>::ok();
    }

    std::span<uint8_t> getBuffer(uint32_t address, size_t length) override
    {
        if (address >= _storage.size() || length == 0) {
            return {};
        }
        const size_t maxLen = _storage.size() - address;
        const size_t toReturn = (length > maxLen) ? maxLen : length;
        return std::span<uint8_t>(_storage).subspan(address, toReturn);
    }

private:
    std::vector<uint8_t> _storage;
};

} // namespace

VirtualPlatform::VirtualPlatform(VirtualTestClock& clock, TaskModel taskModel)
    : _clock(clock)
    , _taskModel(taskModel)
    , _clockObserverId(0)
    , _observedNowUs(clock.nowUs())
    , _waitMutex()
    , _waitCv()
    , _schedulerMutex()
    , _schedulerCv()
    , _scheduledTask(nullptr)
    , _pumpActive(false)
    , _tasksMutex()
    , _tasks()
    , _syncMutex()
    , _mutexes()
    , _queues()
    , _semaphores()
    , _eventGroups()
    , _nextHandleId(1)
    , _memory(std::make_unique<VirtualRamMemory>())
    , _serialNumber(0x56545031u)
    , _macAddress{0x02, 0x56, 0x54, 0x50, 0x31, 0x01}
{
    _clockObserverId = _clock.addAdvanceObserver([this](uint64_t nowUs) {
        this->onClockAdvanced(nowUs);
    });
    (void)_memory->init();
}

VirtualPlatform::~VirtualPlatform()
{
    if (_clockObserverId != 0) {
        (void)_clock.removeAdvanceObserver(_clockObserverId);
    }

    std::vector<TaskHandle> taskHandles;
    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        for (const auto& entry : _tasks) {
            taskHandles.push_back(entry.first);
        }
    }

    for (TaskHandle handle : taskHandles) {
        deleteTask(handle);
    }
}

void VirtualPlatform::restart()
{
    throw std::runtime_error("VirtualPlatform restart requested");
}

void VirtualPlatform::fatalError()
{
    throw std::runtime_error("VirtualPlatform fatalError requested");
}

uint32_t VirtualPlatform::millis() const
{
    return _clock.nowMs();
}

uint64_t VirtualPlatform::micros() const
{
    return _clock.nowUs();
}

void VirtualPlatform::delay(uint32_t ms)
{
    delayMicroseconds(static_cast<uint32_t>(std::min<uint64_t>(
        std::numeric_limits<uint32_t>::max(),
        static_cast<uint64_t>(ms) * 1000u)));
}

void VirtualPlatform::delayMicroseconds(uint32_t us)
{
    if (us == 0u) {
        return;
    }

    // Use the observer-fed timestamp so we never need the clock mutex
    // while holding _waitMutex. This removes the inversion with clock
    // observers and avoids the lost-wakeup window from sampling outside
    // the condition-variable wait.
    const uint64_t deadlineUs = _observedNowUs.load(std::memory_order_acquire)
                                + static_cast<uint64_t>(us);

    {
        std::lock_guard<std::mutex> adLock(_activeDelayMutex);
        _activeDelayCount.fetch_add(1u, std::memory_order_release);
        _activeDelayCv.notify_all();
    }

    {
        std::unique_lock<std::mutex> lock(_waitMutex);
        _waitCv.wait(lock, [&]() {
            return _observedNowUs.load(std::memory_order_acquire) >= deadlineUs;
        });
    }

    {
        std::lock_guard<std::mutex> adLock(_activeDelayMutex);
        _activeDelayCount.fetch_sub(1u, std::memory_order_release);
    }
}

bool VirtualPlatform::waitUntilActiveDelayCalls(size_t n, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(_activeDelayMutex);
    return _activeDelayCv.wait_for(lock, timeout, [&]() {
        return _activeDelayCount.load(std::memory_order_acquire) >= n;
    });
}

bool VirtualPlatform::waitUntilActiveWaiters(size_t n, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(_activeWaiterMutex);
    return _activeWaiterCv.wait_for(lock, timeout, [&]() {
        return _activeWaiterCount.load(std::memory_order_acquire) >= n;
    });
}

uint32_t VirtualPlatform::uniqueSerialNumber() const
{
    return _serialNumber;
}

void VirtualPlatform::macAddress(std::span<uint8_t, 6> mac) const
{
    std::memcpy(mac.data(), _macAddress, mac.size_bytes());
}

TaskHandle VirtualPlatform::createTask(const TaskConfig& config)
{
    if (!config.function) {
        return nullptr;
    }

    std::unique_ptr<TaskWrapper> wrapper = std::make_unique<TaskWrapper>(config.name, config.function);
    TaskHandle handle = nullptr;

    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        handle = generateHandleLocked();
        wrapper->thread = std::thread([this, handle, rawWrapper = wrapper.get(), fn = wrapper->function]() {
            g_virtualCurrentTask = handle;
            if (usesDeterministicTaskModel_()) {
                waitForExecutionPermit_(*rawWrapper);
            }
            if (!rawWrapper->running.load(std::memory_order_acquire)) {
                g_virtualCurrentTask = nullptr;
                markTaskCompleted_(handle);
                return;
            }
            fn();
            g_virtualCurrentTask = nullptr;
            markTaskCompleted_(handle);
        });
        _tasks.emplace(handle, std::move(wrapper));
    }

    if (usesDeterministicTaskModel_()) {
        pumpTasks();
    }

    return handle;
}

void VirtualPlatform::deleteTask(TaskHandle task)
{
    std::unique_ptr<TaskWrapper> wrapper;
    {
        std::lock_guard<std::mutex> schedulerLock(_schedulerMutex);
        std::lock_guard<std::mutex> lock(_tasksMutex);
        auto it = _tasks.find(task);
        if (it == _tasks.end()) {
            return;
        }
        wrapper = std::move(it->second);
        _tasks.erase(it);
        if (_scheduledTask == task) {
            _scheduledTask = nullptr;
        }
    }

    if (wrapper) {
        wrapper->running.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(wrapper->gateMutex);
            wrapper->executionPermit = true;
            wrapper->state = TaskWrapper::State::Completed;
        }
        wrapper->gateCv.notify_all();
    }

    if (wrapper && wrapper->thread.joinable()) {
        if (wrapper->thread.get_id() == std::this_thread::get_id()) {
            wrapper->thread.detach();
        } else {
            notifyAllWaiters();
            _schedulerCv.notify_all();
            wrapper->thread.join();
        }
    }
}

TaskHandle VirtualPlatform::currentTask()
{
    return g_virtualCurrentTask;
}

void VirtualPlatform::taskDelay(uint32_t ms)
{
    if (!usesDeterministicTaskModel_() || currentTask() == nullptr) {
        delay(ms);
        return;
    }

    const uint64_t nowUs = _observedNowUs.load(std::memory_order_acquire);
    const uint64_t delayUs = static_cast<uint64_t>(ms) * 1000u;
    const uint64_t wakeDeadlineUs = (nowUs > std::numeric_limits<uint64_t>::max() - delayUs)
                                        ? std::numeric_limits<uint64_t>::max()
                                        : nowUs + delayUs;
    yieldCurrentTaskUntil_(wakeDeadlineUs, TaskWrapper::SleepReason::Delay);
}

void VirtualPlatform::taskYield()
{
    if (!usesDeterministicTaskModel_() || currentTask() == nullptr) {
        std::this_thread::yield();
        return;
    }

    yieldCurrentTaskUntil_(_observedNowUs.load(std::memory_order_acquire), TaskWrapper::SleepReason::Delay);
}

MutexHandle VirtualPlatform::createMutex()
{
    std::lock_guard<std::mutex> lock(_syncMutex);
    const MutexHandle handle = generateHandleLocked();
    _mutexes.emplace(handle, std::make_unique<VirtualMutex>());
    return handle;
}

void VirtualPlatform::deleteMutex(MutexHandle mutex)
{
    std::lock_guard<std::mutex> lock(_syncMutex);
    _mutexes.erase(mutex);
}

util::Result<void> VirtualPlatform::lockMutex(MutexHandle mutex, uint32_t timeout_ms)
{
    VirtualMutex* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _mutexes.find(mutex);
        if (it == _mutexes.end()) {
            return util::ErrorCode::InvalidParameter;
        }
        state = it->second.get();
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    const auto thisThread = std::this_thread::get_id();
    const bool acquired = waitForPredicate(lock, timeout_ms, [&]() {
        return !state->locked || state->owner == thisThread;
    });
    if (!acquired) {
        return util::ErrorCode::Timeout;
    }

    state->locked = true;
    state->owner = thisThread;
    return util::Result<void>::ok();
}

void VirtualPlatform::unlockMutex(MutexHandle mutex)
{
    VirtualMutex* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _mutexes.find(mutex);
        if (it == _mutexes.end()) {
            return;
        }
        state = it->second.get();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->locked = false;
        state->owner = std::thread::id();
    }
    notifyAllWaiters();
}

QueueHandle VirtualPlatform::createQueue(size_t itemSize, size_t length)
{
    if (itemSize == 0u || length == 0u) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(_syncMutex);
    const QueueHandle handle = generateHandleLocked();
    std::unique_ptr<VirtualQueue> queue = std::make_unique<VirtualQueue>();
    queue->itemSize = itemSize;
    queue->capacity = length;
    _queues.emplace(handle, std::move(queue));
    return handle;
}

void VirtualPlatform::deleteQueue(QueueHandle queue)
{
    std::lock_guard<std::mutex> lock(_syncMutex);
    _queues.erase(queue);
}

util::Result<void> VirtualPlatform::queueSend(QueueHandle queue, const void* item, uint32_t timeoutMs)
{
    if (!item) {
        return util::ErrorCode::InvalidParameter;
    }

    VirtualQueue* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _queues.find(queue);
        if (it == _queues.end()) {
            return util::ErrorCode::InvalidParameter;
        }
        state = it->second.get();
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    const bool ready = waitForPredicate(lock, timeoutMs, [&]() {
        return state->items.size() < state->capacity;
    });
    if (!ready) {
        return util::ErrorCode::Timeout;
    }

    std::vector<uint8_t> payload(state->itemSize);
    std::memcpy(payload.data(), item, state->itemSize);
    state->items.push_back(std::move(payload));
    lock.unlock();
    notifyAllWaiters();
    return util::Result<void>::ok();
}

util::Result<void> VirtualPlatform::queueReceive(QueueHandle queue, void* item, uint32_t timeoutMs)
{
    if (!item) {
        return util::ErrorCode::InvalidParameter;
    }

    VirtualQueue* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _queues.find(queue);
        if (it == _queues.end()) {
            return util::ErrorCode::InvalidParameter;
        }
        state = it->second.get();
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    const bool ready = waitForPredicate(lock, timeoutMs, [&]() {
        return !state->items.empty();
    });
    if (!ready) {
        return util::ErrorCode::Timeout;
    }

    const std::vector<uint8_t> payload = std::move(state->items.front());
    state->items.pop_front();
    std::memcpy(item, payload.data(), state->itemSize);
    lock.unlock();
    notifyAllWaiters();
    return util::Result<void>::ok();
}

size_t VirtualPlatform::queueCount(QueueHandle queue)
{
    VirtualQueue* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _queues.find(queue);
        if (it == _queues.end()) {
            return 0u;
        }
        state = it->second.get();
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    return state->items.size();
}

SemaphoreHandle VirtualPlatform::createBinarySemaphore()
{
    std::lock_guard<std::mutex> lock(_syncMutex);
    const SemaphoreHandle handle = generateHandleLocked();
    _semaphores.emplace(handle, std::make_unique<VirtualSemaphore>());
    return handle;
}

void VirtualPlatform::deleteSemaphore(SemaphoreHandle semaphore)
{
    std::lock_guard<std::mutex> lock(_syncMutex);
    _semaphores.erase(semaphore);
}

util::Result<void> VirtualPlatform::semaphoreGive(SemaphoreHandle semaphore)
{
    VirtualSemaphore* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _semaphores.find(semaphore);
        if (it == _semaphores.end()) {
            return util::ErrorCode::InvalidParameter;
        }
        state = it->second.get();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->count < std::numeric_limits<uint32_t>::max()) {
            ++state->count;
        }
    }
    notifyAllWaiters();
    return util::Result<void>::ok();
}

util::Result<void> VirtualPlatform::semaphoreTake(SemaphoreHandle semaphore, uint32_t timeoutMs)
{
    VirtualSemaphore* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _semaphores.find(semaphore);
        if (it == _semaphores.end()) {
            return util::ErrorCode::InvalidParameter;
        }
        state = it->second.get();
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    const bool ready = waitForPredicate(lock, timeoutMs, [&]() {
        return state->count > 0u;
    });
    if (!ready) {
        return util::ErrorCode::Timeout;
    }

    --state->count;
    return util::Result<void>::ok();
}

EventGroupHandle VirtualPlatform::createEventGroup()
{
    std::lock_guard<std::mutex> lock(_syncMutex);
    const EventGroupHandle handle = generateHandleLocked();
    _eventGroups.emplace(handle, std::make_unique<VirtualEventGroup>());
    return handle;
}

void VirtualPlatform::deleteEventGroup(EventGroupHandle eventGroup)
{
    std::lock_guard<std::mutex> lock(_syncMutex);
    _eventGroups.erase(eventGroup);
}

void VirtualPlatform::eventGroupSetBits(EventGroupHandle eventGroup, uint32_t bits)
{
    VirtualEventGroup* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _eventGroups.find(eventGroup);
        if (it == _eventGroups.end()) {
            return;
        }
        state = it->second.get();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->bits |= bits;
    }
    notifyAllWaiters();
}

void VirtualPlatform::eventGroupClearBits(EventGroupHandle eventGroup, uint32_t bits)
{
    VirtualEventGroup* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _eventGroups.find(eventGroup);
        if (it == _eventGroups.end()) {
            return;
        }
        state = it->second.get();
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->bits &= ~bits;
    }
    notifyAllWaiters();
}

uint32_t VirtualPlatform::eventGroupWaitBits(EventGroupHandle eventGroup,
                                             uint32_t bits,
                                             EventGroupClearMode clearOnExit,
                                             EventGroupWaitMode waitAll,
                                             uint32_t timeoutMs)
{
    VirtualEventGroup* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(_syncMutex);
        auto it = _eventGroups.find(eventGroup);
        if (it == _eventGroups.end()) {
            return 0u;
        }
        state = it->second.get();
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    const auto condition = [&]() {
        if (waitAll == EventGroupWaitMode::All) {
            return (state->bits & bits) == bits;
        }
        return (state->bits & bits) != 0u;
    };

    const bool ready = waitForPredicate(lock, timeoutMs, condition);
    if (!ready) {
        return state->bits & bits;
    }

    const uint32_t observedBits = state->bits & bits;
    if (clearOnExit == EventGroupClearMode::Clear) {
        state->bits &= ~bits;
    }
    return observedBits;
}

util::Result<void> VirtualPlatform::taskNotifyGive(TaskHandle task)
{
    if (!task) {
        task = currentTask();
    }

    TaskWrapper* wrapper = nullptr;
    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        auto it = _tasks.find(task);
        if (it == _tasks.end()) {
            return util::ErrorCode::InvalidParameter;
        }
        wrapper = it->second.get();
    }

    {
        std::lock_guard<std::mutex> lock(wrapper->notification.mutex);
        if (wrapper->notification.value < std::numeric_limits<uint32_t>::max()) {
            ++wrapper->notification.value;
        }
    }
    notifyAllWaiters();
    return util::Result<void>::ok();
}

util::Result<void> VirtualPlatform::taskNotifyGiveFromISR(TaskHandle task)
{
    return taskNotifyGive(task);
}

uint32_t VirtualPlatform::taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t timeout_ms)
{
    const TaskHandle task = currentTask();
    if (!task) {
        return 0u;
    }

    TaskWrapper* wrapper = nullptr;
    {
        std::lock_guard<std::mutex> lock(_tasksMutex);
        auto it = _tasks.find(task);
        if (it == _tasks.end()) {
            return 0u;
        }
        wrapper = it->second.get();
    }

    std::unique_lock<std::mutex> lock(wrapper->notification.mutex);
    const bool ready = waitForPredicate(lock, timeout_ms, [&]() {
        return wrapper->notification.value > 0u;
    });
    if (!ready) {
        return 0u;
    }

    const uint32_t value = wrapper->notification.value;
    if (clearMode == TaskNotifyClearMode::Clear) {
        wrapper->notification.value = 0u;
    } else if (wrapper->notification.value > 0u) {
        --wrapper->notification.value;
    }
    return value;
}

MemoryInterface& VirtualPlatform::memory()
{
    return *_memory;
}

NetworkInterface* VirtualPlatform::network()
{
    return nullptr;
}

UartInterface* VirtualPlatform::uart()
{
    return nullptr;
}

SpiInterface* VirtualPlatform::spi()
{
    return nullptr;
}

void VirtualPlatform::log(const char* level, const char* tag, const char* format, ...)
{
    std::fprintf(stderr, "[%s] %s: ", level ? level : "LOG", tag ? tag : "VirtualPlatform");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format ? format : "", args);
    va_end(args);
    std::fprintf(stderr, "\n");
}

void VirtualPlatform::advanceTimeUs(uint64_t deltaUs)
{
    _clock.advanceUs(deltaUs);
    pumpTasks();
}

void VirtualPlatform::advanceTimeMs(uint32_t deltaMs)
{
    _clock.advanceMs(deltaMs);
    pumpTasks();
}

void VirtualPlatform::pumpTasks()
{
    if (!usesDeterministicTaskModel_()) {
        return;
    }
    pumpTasks_();
}

std::vector<VirtualPlatform::TaskSnapshot> VirtualPlatform::taskSnapshots() const
{
    std::vector<TaskSnapshot> snapshots;
    std::lock_guard<std::mutex> schedulerLock(_schedulerMutex);
    std::lock_guard<std::mutex> tasksLock(_tasksMutex);
    snapshots.reserve(_tasks.size());
    for (const auto& entry : _tasks) {
        const auto& task = *entry.second;
        uint32_t notificationValue = 0;
        {
            std::lock_guard<std::mutex> notificationLock(task.notification.mutex);
            notificationValue = task.notification.value;
        }

        snapshots.push_back(TaskSnapshot{
            entry.first,
            task.name,
            toPublicTaskState_(task.state),
            toPublicTaskSleepReason_(task.sleepReason),
            task.wakeDeadlineUs,
            task.running.load(std::memory_order_acquire),
            notificationValue,
        });
    }
    return snapshots;
}

VirtualPlatform::SchedulerSnapshot VirtualPlatform::schedulerSnapshot() const noexcept
{
    SchedulerSnapshot snapshot{};
    snapshot.taskModel = _taskModel;
    snapshot.nowUs = _observedNowUs.load(std::memory_order_acquire);

    std::lock_guard<std::mutex> schedulerLock(_schedulerMutex);
    snapshot.scheduledTask = _scheduledTask;
    snapshot.pumpActive = _pumpActive;

    std::lock_guard<std::mutex> tasksLock(_tasksMutex);
    snapshot.taskCount = _tasks.size();
    for (const auto& entry : _tasks) {
        const auto& task = *entry.second;
        switch (task.state) {
            case TaskWrapper::State::Ready:
                ++snapshot.readyTaskCount;
                break;
            case TaskWrapper::State::Sleeping:
                ++snapshot.sleepingTaskCount;
                if (task.sleepReason == TaskWrapper::SleepReason::PredicateWait) {
                    ++snapshot.predicateWaitTaskCount;
                }
                break;
            case TaskWrapper::State::Running:
            case TaskWrapper::State::Completed:
                break;
        }
    }

    return snapshot;
}

TaskHandle VirtualPlatform::generateHandleLocked()
{
    const auto handleValue = _nextHandleId++;
    return reinterpret_cast<TaskHandle>(handleValue);
}

uint64_t VirtualPlatform::deadlineUsFromTimeoutMs(uint32_t timeoutMs) const
{
    const uint64_t nowUs = _observedNowUs.load(std::memory_order_acquire);
    const uint64_t timeoutUs = static_cast<uint64_t>(timeoutMs) * 1000u;
    if (nowUs > std::numeric_limits<uint64_t>::max() - timeoutUs) {
        return std::numeric_limits<uint64_t>::max();
    }
    return nowUs + timeoutUs;
}

void VirtualPlatform::notifyAllWaiters()
{
    if (usesDeterministicTaskModel_()) {
        {
            std::lock_guard<std::mutex> schedulerLock(_schedulerMutex);
            std::lock_guard<std::mutex> tasksLock(_tasksMutex);
            for (auto& entry : _tasks) {
                auto& task = *entry.second;
                if (!task.running.load(std::memory_order_acquire)) {
                    continue;
                }
                if (task.state == TaskWrapper::State::Sleeping
                    && task.sleepReason == TaskWrapper::SleepReason::PredicateWait) {
                    task.state = TaskWrapper::State::Ready;
                    task.sleepReason = TaskWrapper::SleepReason::None;
                }
            }
        }

        if (currentTask() == nullptr) {
            pumpTasks_();
        }
    }
    _waitCv.notify_all();
    _schedulerCv.notify_all();
}

void VirtualPlatform::onClockAdvanced(uint64_t nowUs)
{
    _observedNowUs.store(nowUs, std::memory_order_release);
    notifyAllWaiters();
    if (usesDeterministicTaskModel_()) {
        pumpTasks_();
    }
}

bool VirtualPlatform::usesDeterministicTaskModel_() const noexcept
{
    return _taskModel == TaskModel::Deterministic;
}

VirtualPlatform::TaskWrapper* VirtualPlatform::currentTaskWrapper_()
{
    const TaskHandle task = currentTask();
    if (!task) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(_tasksMutex);
    const auto it = _tasks.find(task);
    return it == _tasks.end() ? nullptr : it->second.get();
}

void VirtualPlatform::waitForExecutionPermit_(TaskWrapper& task)
{
    std::unique_lock<std::mutex> lock(task.gateMutex);
    task.gateCv.wait(lock, [&task]() {
        return task.executionPermit || !task.running.load(std::memory_order_acquire);
    });
    task.executionPermit = false;
}

void VirtualPlatform::yieldCurrentTaskUntil_(uint64_t wakeDeadlineUs, TaskWrapper::SleepReason reason)
{
    TaskWrapper* task = currentTaskWrapper_();
    const TaskHandle taskHandle = currentTask();
    if (!task || !taskHandle) {
        return;
    }

    {
        std::lock_guard<std::mutex> schedulerLock(_schedulerMutex);
        {
            std::lock_guard<std::mutex> tasksLock(_tasksMutex);
            task->wakeDeadlineUs = wakeDeadlineUs;
            task->state = (wakeDeadlineUs <= _observedNowUs.load(std::memory_order_acquire))
                              ? TaskWrapper::State::Ready
                              : TaskWrapper::State::Sleeping;
            task->sleepReason = (task->state == TaskWrapper::State::Ready)
                                    ? TaskWrapper::SleepReason::None
                                    : reason;
        }
        if (_scheduledTask == taskHandle) {
            _scheduledTask = nullptr;
        }
    }

    _schedulerCv.notify_all();
    waitForExecutionPermit_(*task);
}

void VirtualPlatform::markTaskCompleted_(TaskHandle task)
{
    std::lock_guard<std::mutex> schedulerLock(_schedulerMutex);
    std::lock_guard<std::mutex> tasksLock(_tasksMutex);
    const auto it = _tasks.find(task);
    if (it != _tasks.end()) {
        it->second->state = TaskWrapper::State::Completed;
        it->second->running.store(false, std::memory_order_release);
    }
    if (_scheduledTask == task) {
        _scheduledTask = nullptr;
    }
    _schedulerCv.notify_all();
}

void VirtualPlatform::pumpTasks_()
{
    std::unique_lock<std::mutex> schedulerLock(_schedulerMutex);
    if (_pumpActive) {
        return;
    }

    _pumpActive = true;
    const auto clearPump = [this]() noexcept {
        std::lock_guard<std::mutex> lock(_schedulerMutex);
        _pumpActive = false;
    };

    while (true) {
        TaskHandle nextTask = nullptr;
        TaskWrapper* nextWrapper = nullptr;
        const uint64_t nowUs = _observedNowUs.load(std::memory_order_acquire);

        {
            std::lock_guard<std::mutex> tasksLock(_tasksMutex);
            for (auto& entry : _tasks) {
                auto& wrapper = *entry.second;
                if (!wrapper.running.load(std::memory_order_acquire)) {
                    continue;
                }
                if (wrapper.state == TaskWrapper::State::Ready
                    || (wrapper.state == TaskWrapper::State::Sleeping && wrapper.wakeDeadlineUs <= nowUs)) {
                    nextTask = entry.first;
                    nextWrapper = entry.second.get();
                    wrapper.state = TaskWrapper::State::Running;
                    wrapper.sleepReason = TaskWrapper::SleepReason::None;
                    break;
                }
            }
        }

        if (!nextTask || !nextWrapper) {
            break;
        }

        _scheduledTask = nextTask;
        {
            std::lock_guard<std::mutex> gateLock(nextWrapper->gateMutex);
            nextWrapper->executionPermit = true;
        }
        nextWrapper->gateCv.notify_one();

        _schedulerCv.wait(schedulerLock, [&]() {
            return _scheduledTask != nextTask || !nextWrapper->running.load(std::memory_order_acquire);
        });
    }

    schedulerLock.unlock();
    clearPump();
}

} // namespace platform
} // namespace knx
