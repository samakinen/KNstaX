// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/platform/virtual_platform.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

// AddressSanitizer tracks one stack per thread. Without being told about each
// switch it takes the first frame on a fibre stack for a wild write far below
// the thread's stack, and it never clears the redzones a discarded fibre left
// in its shadow memory.
#if defined(__SANITIZE_ADDRESS__)
#define KNX_VIRTUAL_PLATFORM_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define KNX_VIRTUAL_PLATFORM_ASAN 1
#endif
#endif

#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
#include <sanitizer/asan_interface.h>
#include <sanitizer/common_interface_defs.h>
#endif

namespace knx {
namespace platform {

namespace {

constexpr uint64_t kNoDeadline = std::numeric_limits<uint64_t>::max();

// A run of the scheduler that switches this many times without the driver
// getting control back has tasks handing work to each other forever (a
// taskYield() poll loop waiting for something only time would bring, say).
constexpr uint64_t kLivelockSwitchLimit = 10'000'000u;

#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
// Instrumented frames are several times larger.
constexpr size_t kStackScale = 4u;
#else
constexpr size_t kStackScale = 1u;
#endif

uint64_t saturatingAdd(uint64_t a, uint64_t b)
{
    return (a > kNoDeadline - b) ? kNoDeadline : a + b;
}

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

// ============================================================================
// Fibres
// ============================================================================

struct VirtualPlatform::DriverContext {
    ucontext_t context{};
    // Bounds of the driver's stack, learnt on the first switch into a fibre.
    const void* stackBottom{nullptr};
    size_t stackSize{0};
    void* fakeStack{nullptr};
};

struct VirtualPlatform::Fibre {
    TaskHandle handle{nullptr};
    const char* name{nullptr};
    TaskFunction function;
    uint32_t priority{0};

    ucontext_t context{};
    void* mapping{nullptr};      // the whole mmap, guard page included
    size_t mappingSize{0};
    void* stackBottom{nullptr};  // lowest usable address
    size_t stackSize{0};
    void* fakeStack{nullptr};    // ASan's per-fibre fake stack

    TaskState state{TaskState::Ready};
    TaskSleepReason sleepReason{TaskSleepReason::None};
    uint64_t wakeDeadlineUs{0};
    WaitCondition wait{};
    bool hasWait{false};
    uint64_t suspendSequence{0};
    uint32_t notificationValue{0};

    bool finished{false};
    bool deleted{false};
    std::exception_ptr exception;

    ~Fibre() { releaseStack(); }

    void releaseStack() noexcept
    {
        if (!mapping) {
            return;
        }
#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
        // A fibre discarded in the middle of a call leaves its frames'
        // redzones poisoned; the next mapping at this address would inherit
        // them.
        ASAN_UNPOISON_MEMORY_REGION(stackBottom, stackSize);
#endif
        (void)::munmap(mapping, mappingSize);
        mapping = nullptr;
        stackBottom = nullptr;
    }
};

VirtualPlatform::VirtualPlatform(VirtualTestClock& clock)
    : _clock(clock)
    , _ownerThread(std::this_thread::get_id())
    , _driver(std::make_unique<DriverContext>())
    , _memory(std::make_unique<VirtualRamMemory>())
    , _serialNumber(0x56545031u)
    , _macAddress{0x02, 0x56, 0x54, 0x50, 0x31, 0x01}
{
    _clockObserverId = _clock.addAdvanceObserver([this](uint64_t nowUs) {
        this->onClockAdvanced_(nowUs);
    });
    (void)_memory->init();
}

VirtualPlatform::~VirtualPlatform()
{
    if (_clockObserverId != 0) {
        (void)_clock.removeAdvanceObserver(_clockObserverId);
    }
    // Every fibre is suspended (the destructor runs in the driver context), so
    // dropping them is the FreeRTOS-style discard deleteTask() does.
    _tasks.clear();
}

void VirtualPlatform::requireOwnerThread_() const
{
    if (std::this_thread::get_id() != _ownerThread) {
        std::fprintf(stderr,
                     "VirtualPlatform: called from a thread other than the one that created it; "
                     "the platform is single-threaded, run the code as a task instead\n");
        std::abort();
    }
}

void* VirtualPlatform::generateHandle_()
{
    const auto handleValue = _nextHandleId++;
    return reinterpret_cast<void*>(handleValue);
}

uint64_t VirtualPlatform::deadlineAfterUs_(uint64_t delayUs) const
{
    return saturatingAdd(_clock.nowUs(), delayUs);
}

VirtualPlatform::Fibre* VirtualPlatform::findTask_(TaskHandle task) const
{
    const auto it = _tasks.find(task);
    return it == _tasks.end() ? nullptr : it->second.get();
}

// ============================================================================
// System
// ============================================================================

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

uint32_t VirtualPlatform::uniqueSerialNumber() const
{
    return _serialNumber;
}

void VirtualPlatform::macAddress(std::span<uint8_t, 6> mac) const
{
    std::memcpy(mac.data(), _macAddress, mac.size_bytes());
}

MemoryInterface& VirtualPlatform::memory()
{
    return *_memory;
}

NetworkInterface* VirtualPlatform::network()
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

// ============================================================================
// Time
// ============================================================================

void VirtualPlatform::delay(uint32_t ms)
{
    taskDelay(ms);
}

void VirtualPlatform::delayMicroseconds(uint32_t us)
{
    requireOwnerThread_();
    if (us == 0u) {
        return;
    }
    if (inDriverContext_()) {
        advanceTo_(deadlineAfterUs_(us));
        return;
    }
    sleepCurrentUntil_(deadlineAfterUs_(us));
}

void VirtualPlatform::taskDelay(uint32_t ms)
{
    requireOwnerThread_();
    if (ms == 0u) {
        taskYield();
        return;
    }
    const uint64_t delayUs = static_cast<uint64_t>(ms) * 1000u;
    if (inDriverContext_()) {
        advanceTo_(deadlineAfterUs_(delayUs));
        return;
    }
    sleepCurrentUntil_(deadlineAfterUs_(delayUs));
}

void VirtualPlatform::taskYield()
{
    requireOwnerThread_();
    if (inDriverContext_()) {
        runUntilQuiescent();
        return;
    }
    yieldCurrent_();
}

void VirtualPlatform::advanceTimeUs(uint64_t deltaUs)
{
    requireOwnerThread_();
    const uint64_t targetUs = deadlineAfterUs_(deltaUs);
    if (!inDriverContext_()) {
        _clock.advanceToUs(targetUs);
        return;
    }
    advanceTo_(targetUs);
}

void VirtualPlatform::advanceTimeMs(uint32_t deltaMs)
{
    advanceTimeUs(static_cast<uint64_t>(deltaMs) * 1000u);
}

void VirtualPlatform::advanceTo_(uint64_t targetUs)
{
    runUntilQuiescent();
    while (true) {
        const uint64_t nowUs = _clock.nowUs();
        if (nowUs >= targetUs) {
            break;
        }

        uint64_t nextUs = targetUs;
        for (const auto& entry : _tasks) {
            const Fibre& fibre = *entry.second;
            if (fibre.state == TaskState::Sleeping && fibre.wakeDeadlineUs > nowUs
                && fibre.wakeDeadlineUs < nextUs) {
                nextUs = fibre.wakeDeadlineUs;
            }
        }

        // The observer (onClockAdvanced_) runs what became due.
        _clock.advanceToUs(nextUs);
    }
}

void VirtualPlatform::onClockAdvanced_(uint64_t /*nowUs*/)
{
    requireOwnerThread_();
    if (inDriverContext_()) {
        runUntilQuiescent();
    }
    // From a task: whatever became due runs once that task blocks.
}

// ============================================================================
// Scheduler
// ============================================================================

void VirtualPlatform::wakeTasks_()
{
    if (inDriverContext_()) {
        runUntilQuiescent();
    }
}

bool VirtualPlatform::isRunnable_(const Fibre& fibre, uint64_t nowUs) const
{
    if (fibre.finished) {
        return false;
    }
    switch (fibre.state) {
        case TaskState::Ready:
            return true;
        case TaskState::Sleeping:
            return fibre.wakeDeadlineUs <= nowUs || (fibre.hasWait && fibre.wait());
        case TaskState::Running:
        case TaskState::Completed:
            return false;
    }
    return false;
}

VirtualPlatform::Fibre* VirtualPlatform::pickRunnable_()
{
    const uint64_t nowUs = _clock.nowUs();
    Fibre* best = nullptr;
    for (const auto& entry : _tasks) {
        Fibre& fibre = *entry.second;
        if (!isRunnable_(fibre, nowUs)) {
            continue;
        }
        if (!best || fibre.priority > best->priority
            || (fibre.priority == best->priority && fibre.suspendSequence < best->suspendSequence)) {
            best = &fibre;
        }
    }
    return best;
}

void VirtualPlatform::runUntilQuiescent()
{
    requireOwnerThread_();
    if (!inDriverContext_() || _dispatching) {
        return;
    }

    _dispatching = true;
    struct DispatchGuard {
        bool& flag;
        ~DispatchGuard() { flag = false; }
    } guard{_dispatching};

    uint64_t switches = 0;
    while (Fibre* fibre = pickRunnable_()) {
        if (++switches > kLivelockSwitchLimit) {
            throw std::logic_error(std::string("VirtualPlatform: tasks keep running without time advancing (last: ")
                                   + (fibre->name ? fibre->name : "?") + ")");
        }
        runFibre_(*fibre);
    }
}

void VirtualPlatform::runFibre_(Fibre& fibre)
{
    fibre.state = TaskState::Running;
    fibre.sleepReason = TaskSleepReason::None;
    fibre.hasWait = false;
    _current = &fibre;

#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
    __sanitizer_start_switch_fiber(&_driver->fakeStack, fibre.stackBottom, fibre.stackSize);
#endif
    (void)::swapcontext(&_driver->context, &fibre.context);
#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
    __sanitizer_finish_switch_fiber(_driver->fakeStack, nullptr, nullptr);
#endif

    _current = nullptr;
    if (!fibre.finished) {
        return;
    }

    fibre.state = TaskState::Completed;
    fibre.sleepReason = TaskSleepReason::None;
    std::exception_ptr exception = std::exchange(fibre.exception, nullptr);
    if (fibre.deleted) {
        _tasks.erase(fibre.handle);  // destroys the fibre and its stack
    } else {
        fibre.releaseStack();
    }
    if (exception) {
        std::rethrow_exception(exception);
    }
}

void VirtualPlatform::suspendCurrent_()
{
    Fibre& fibre = *_current;
    fibre.suspendSequence = ++_suspendSequence;

#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
    __sanitizer_start_switch_fiber(&fibre.fakeStack, _driver->stackBottom, _driver->stackSize);
#endif
    (void)::swapcontext(&fibre.context, &_driver->context);
#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
    __sanitizer_finish_switch_fiber(fibre.fakeStack, &_driver->stackBottom, &_driver->stackSize);
#endif
    // Resumed by runFibre_(), which has set _current back to this fibre.
}

void VirtualPlatform::sleepCurrentUntil_(uint64_t deadlineUs)
{
    Fibre& fibre = *_current;
    fibre.state = TaskState::Sleeping;
    fibre.sleepReason = TaskSleepReason::Delay;
    fibre.wakeDeadlineUs = deadlineUs;
    fibre.hasWait = false;
    suspendCurrent_();
}

void VirtualPlatform::yieldCurrent_()
{
    Fibre& fibre = *_current;
    fibre.state = TaskState::Ready;
    fibre.sleepReason = TaskSleepReason::None;
    fibre.hasWait = false;
    suspendCurrent_();
}

void VirtualPlatform::fibreEntry_(unsigned int platformLo, unsigned int platformHi)
{
    const uintptr_t address = (static_cast<uintptr_t>(platformHi) << 16u << 16u)
                              | static_cast<uintptr_t>(platformLo);
    reinterpret_cast<VirtualPlatform*>(address)->fibreMain_();
}

void VirtualPlatform::fibreMain_()
{
#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
    __sanitizer_finish_switch_fiber(nullptr, &_driver->stackBottom, &_driver->stackSize);
#endif
    Fibre& fibre = *_current;

    // Leave the handler before switching away: the C++ runtime keeps the
    // caught-exception stack per thread, not per fibre.
    try {
        fibre.function();
    } catch (...) {
        fibre.exception = std::current_exception();
    }

    fibre.finished = true;
#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
    __sanitizer_start_switch_fiber(nullptr, _driver->stackBottom, _driver->stackSize);
#endif
    (void)::setcontext(&_driver->context);
    std::abort();  // setcontext() does not return
}

// ============================================================================
// Tasks
// ============================================================================

TaskHandle VirtualPlatform::createTask(const TaskConfig& config)
{
    requireOwnerThread_();
    if (!config.function) {
        return nullptr;
    }

    auto fibre = std::make_unique<Fibre>();
    fibre->name = config.name;
    fibre->function = config.function;
    fibre->priority = config.priority;

    const size_t pageSize = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    size_t stackSize = std::max<size_t>(config.stackSize, kMinTaskStackBytes) * kStackScale;
    stackSize = (stackSize + pageSize - 1u) / pageSize * pageSize;
    const size_t mappingSize = stackSize + pageSize;  // + a guard page below
    void* mapping = ::mmap(nullptr, mappingSize, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (mapping == MAP_FAILED) {
        return nullptr;
    }
    fibre->mapping = mapping;
    fibre->mappingSize = mappingSize;
    (void)::mprotect(mapping, pageSize, PROT_NONE);
    fibre->stackBottom = static_cast<uint8_t*>(mapping) + pageSize;
    fibre->stackSize = stackSize;

    if (::getcontext(&fibre->context) != 0) {
        return nullptr;
    }
    fibre->context.uc_stack.ss_sp = fibre->stackBottom;
    fibre->context.uc_stack.ss_size = fibre->stackSize;
    fibre->context.uc_link = nullptr;
    const auto address = reinterpret_cast<uintptr_t>(this);
    // makecontext() passes int-sized arguments only. void(*)() is the type it
    // takes, and the one function-pointer cast -Wcast-function-type accepts.
    ::makecontext(&fibre->context,
                  reinterpret_cast<void (*)()>(&VirtualPlatform::fibreEntry_),
                  2,
                  static_cast<unsigned int>(address & 0xFFFFFFFFu),
                  static_cast<unsigned int>((address >> 16u >> 16u) & 0xFFFFFFFFu));

    const TaskHandle handle = generateHandle_();
    fibre->handle = handle;
    fibre->state = TaskState::Ready;
    fibre->suspendSequence = ++_suspendSequence;
    _tasks.emplace(handle, std::move(fibre));

    wakeTasks_();
    return handle;
}

void VirtualPlatform::deleteTask(TaskHandle task)
{
    requireOwnerThread_();
    Fibre* fibre = task ? findTask_(task) : _current;
    if (!fibre) {
        return;
    }

    if (fibre == _current) {
        // Deleting itself: never comes back. runFibre_() frees the stack once
        // it is off it.
        fibre->deleted = true;
        fibre->finished = true;
#if defined(KNX_VIRTUAL_PLATFORM_ASAN)
        __sanitizer_start_switch_fiber(nullptr, _driver->stackBottom, _driver->stackSize);
#endif
        (void)::setcontext(&_driver->context);
        std::abort();
    }

    _tasks.erase(fibre->handle);
}

TaskHandle VirtualPlatform::currentTask()
{
    return _current ? _current->handle : nullptr;
}

// ============================================================================
// Blocking waits
// ============================================================================

template <typename Predicate>
bool VirtualPlatform::waitFor_(uint32_t timeoutMs, const Predicate& predicate)
{
    const WaitCondition condition{
        &predicate,
        [](const void* object) { return (*static_cast<const Predicate*>(object))(); },
    };
    return waitForCondition_(timeoutMs, condition);
}

bool VirtualPlatform::waitForCondition_(uint32_t timeoutMs, WaitCondition condition)
{
    requireOwnerThread_();
    if (condition()) {
        return true;
    }
    if (timeoutMs == 0u) {
        return false;
    }

    if (inDriverContext_()) {
        runUntilQuiescent();
        if (condition()) {
            return true;
        }
        throw std::logic_error(
            "VirtualPlatform: a blocking wait outside any task needs virtual time to pass; "
            "advance the time or run the caller as a task");
    }

    const uint64_t deadlineUs = (timeoutMs == UINT32_MAX)
                                    ? kNoDeadline
                                    : deadlineAfterUs_(static_cast<uint64_t>(timeoutMs) * 1000u);
    Fibre& fibre = *_current;
    while (true) {
        fibre.state = TaskState::Sleeping;
        fibre.sleepReason = TaskSleepReason::PredicateWait;
        fibre.wakeDeadlineUs = deadlineUs;
        fibre.wait = condition;
        fibre.hasWait = true;
        suspendCurrent_();
        fibre.hasWait = false;

        if (condition()) {
            return true;
        }
        if (_clock.nowUs() >= deadlineUs) {
            return false;
        }
    }
}

// ============================================================================
// Mutexes
// ============================================================================

MutexHandle VirtualPlatform::createMutex()
{
    requireOwnerThread_();
    const MutexHandle handle = generateHandle_();
    _mutexes.emplace(handle, std::make_unique<VirtualMutex>());
    return handle;
}

void VirtualPlatform::deleteMutex(MutexHandle mutex)
{
    requireOwnerThread_();
    _mutexes.erase(mutex);
}

util::Result<void> VirtualPlatform::lockMutex(MutexHandle mutex, uint32_t timeout_ms)
{
    const auto it = _mutexes.find(mutex);
    if (it == _mutexes.end()) {
        return util::ErrorCode::InvalidParameter;
    }
    VirtualMutex* state = it->second.get();

    // The driver context counts as one owner (handle nullptr).
    const TaskHandle self = currentTask();
    const auto available = [state, self]() { return !state->locked || state->owner == self; };
    if (!waitFor_(timeout_ms, available)) {
        return util::ErrorCode::Timeout;
    }

    state->locked = true;
    state->owner = self;
    return util::Result<void>::ok();
}

void VirtualPlatform::unlockMutex(MutexHandle mutex)
{
    requireOwnerThread_();
    const auto it = _mutexes.find(mutex);
    if (it == _mutexes.end()) {
        return;
    }
    it->second->locked = false;
    it->second->owner = nullptr;
    wakeTasks_();
}

// ============================================================================
// Queues
// ============================================================================

QueueHandle VirtualPlatform::createQueue(size_t itemSize, size_t length)
{
    requireOwnerThread_();
    if (itemSize == 0u || length == 0u) {
        return nullptr;
    }

    const QueueHandle handle = generateHandle_();
    auto queue = std::make_unique<VirtualQueue>();
    queue->itemSize = itemSize;
    queue->capacity = length;
    _queues.emplace(handle, std::move(queue));
    return handle;
}

void VirtualPlatform::deleteQueue(QueueHandle queue)
{
    requireOwnerThread_();
    _queues.erase(queue);
}

util::Result<void> VirtualPlatform::queueSend(QueueHandle queue, const void* item, uint32_t timeoutMs)
{
    if (!item) {
        return util::ErrorCode::InvalidParameter;
    }
    const auto it = _queues.find(queue);
    if (it == _queues.end()) {
        return util::ErrorCode::InvalidParameter;
    }
    VirtualQueue* state = it->second.get();

    const auto hasRoom = [state]() { return state->items.size() < state->capacity; };
    if (!waitFor_(timeoutMs, hasRoom)) {
        return util::ErrorCode::Timeout;
    }

    std::vector<uint8_t> payload(state->itemSize);
    std::memcpy(payload.data(), item, state->itemSize);
    state->items.push_back(std::move(payload));
    wakeTasks_();
    return util::Result<void>::ok();
}

util::Result<void> VirtualPlatform::queueReceive(QueueHandle queue, void* item, uint32_t timeoutMs)
{
    if (!item) {
        return util::ErrorCode::InvalidParameter;
    }
    const auto it = _queues.find(queue);
    if (it == _queues.end()) {
        return util::ErrorCode::InvalidParameter;
    }
    VirtualQueue* state = it->second.get();

    const auto hasItem = [state]() { return !state->items.empty(); };
    if (!waitFor_(timeoutMs, hasItem)) {
        return util::ErrorCode::Timeout;
    }

    std::memcpy(item, state->items.front().data(), state->itemSize);
    state->items.pop_front();
    wakeTasks_();
    return util::Result<void>::ok();
}

size_t VirtualPlatform::queueCount(QueueHandle queue)
{
    const auto it = _queues.find(queue);
    return it == _queues.end() ? 0u : it->second->items.size();
}

// ============================================================================
// Semaphores
// ============================================================================

SemaphoreHandle VirtualPlatform::createBinarySemaphore()
{
    requireOwnerThread_();
    const SemaphoreHandle handle = generateHandle_();
    _semaphores.emplace(handle, std::make_unique<VirtualSemaphore>());
    return handle;
}

void VirtualPlatform::deleteSemaphore(SemaphoreHandle semaphore)
{
    requireOwnerThread_();
    _semaphores.erase(semaphore);
}

util::Result<void> VirtualPlatform::semaphoreGive(SemaphoreHandle semaphore)
{
    requireOwnerThread_();
    const auto it = _semaphores.find(semaphore);
    if (it == _semaphores.end()) {
        return util::ErrorCode::InvalidParameter;
    }
    VirtualSemaphore& state = *it->second;
    if (state.count < std::numeric_limits<uint32_t>::max()) {
        ++state.count;
    }
    wakeTasks_();
    return util::Result<void>::ok();
}

util::Result<void> VirtualPlatform::semaphoreTake(SemaphoreHandle semaphore, uint32_t timeoutMs)
{
    const auto it = _semaphores.find(semaphore);
    if (it == _semaphores.end()) {
        return util::ErrorCode::InvalidParameter;
    }
    VirtualSemaphore* state = it->second.get();

    const auto available = [state]() { return state->count > 0u; };
    if (!waitFor_(timeoutMs, available)) {
        return util::ErrorCode::Timeout;
    }

    --state->count;
    return util::Result<void>::ok();
}

// ============================================================================
// Event groups
// ============================================================================

EventGroupHandle VirtualPlatform::createEventGroup()
{
    requireOwnerThread_();
    const EventGroupHandle handle = generateHandle_();
    _eventGroups.emplace(handle, std::make_unique<VirtualEventGroup>());
    return handle;
}

void VirtualPlatform::deleteEventGroup(EventGroupHandle eventGroup)
{
    requireOwnerThread_();
    _eventGroups.erase(eventGroup);
}

void VirtualPlatform::eventGroupSetBits(EventGroupHandle eventGroup, uint32_t bits)
{
    requireOwnerThread_();
    const auto it = _eventGroups.find(eventGroup);
    if (it == _eventGroups.end()) {
        return;
    }
    it->second->bits |= bits;
    wakeTasks_();
}

void VirtualPlatform::eventGroupClearBits(EventGroupHandle eventGroup, uint32_t bits)
{
    requireOwnerThread_();
    const auto it = _eventGroups.find(eventGroup);
    if (it == _eventGroups.end()) {
        return;
    }
    it->second->bits &= ~bits;
}

uint32_t VirtualPlatform::eventGroupWaitBits(EventGroupHandle eventGroup,
                                             uint32_t bits,
                                             EventGroupClearMode clearOnExit,
                                             EventGroupWaitMode waitAll,
                                             uint32_t timeoutMs)
{
    const auto it = _eventGroups.find(eventGroup);
    if (it == _eventGroups.end()) {
        return 0u;
    }
    VirtualEventGroup* state = it->second.get();

    const auto satisfied = [state, bits, waitAll]() {
        if (waitAll == EventGroupWaitMode::All) {
            return (state->bits & bits) == bits;
        }
        return (state->bits & bits) != 0u;
    };
    if (!waitFor_(timeoutMs, satisfied)) {
        return state->bits & bits;
    }

    const uint32_t observedBits = state->bits & bits;
    if (clearOnExit == EventGroupClearMode::Clear) {
        state->bits &= ~bits;
    }
    return observedBits;
}

// ============================================================================
// Task notifications
// ============================================================================

util::Result<void> VirtualPlatform::taskNotifyGive(TaskHandle task)
{
    requireOwnerThread_();
    Fibre* fibre = task ? findTask_(task) : _current;
    if (!fibre) {
        return util::ErrorCode::InvalidParameter;
    }
    if (fibre->notificationValue < std::numeric_limits<uint32_t>::max()) {
        ++fibre->notificationValue;
    }
    wakeTasks_();
    return util::Result<void>::ok();
}

util::Result<void> VirtualPlatform::taskNotifyGiveFromISR(TaskHandle task)
{
    return taskNotifyGive(task);
}

uint32_t VirtualPlatform::taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t timeout_ms)
{
    requireOwnerThread_();
    Fibre* fibre = _current;
    if (!fibre) {
        return 0u;
    }

    const auto notified = [fibre]() { return fibre->notificationValue > 0u; };
    if (!waitFor_(timeout_ms, notified)) {
        return 0u;
    }

    const uint32_t value = fibre->notificationValue;
    if (clearMode == TaskNotifyClearMode::Clear) {
        fibre->notificationValue = 0u;
    } else {
        --fibre->notificationValue;
    }
    return value;
}

// ============================================================================
// Test helpers
// ============================================================================

size_t VirtualPlatform::countBlocked_(TaskSleepReason reason) const
{
    size_t count = 0;
    for (const auto& entry : _tasks) {
        const Fibre& fibre = *entry.second;
        if (fibre.state == TaskState::Sleeping && fibre.sleepReason == reason) {
            ++count;
        }
    }
    return count;
}

bool VirtualPlatform::waitUntilActiveDelayCalls(size_t n)
{
    runUntilQuiescent();
    return countBlocked_(TaskSleepReason::Delay) >= n;
}

bool VirtualPlatform::waitUntilActiveWaiters(size_t n)
{
    runUntilQuiescent();
    return countBlocked_(TaskSleepReason::PredicateWait) >= n;
}

std::vector<VirtualPlatform::TaskSnapshot> VirtualPlatform::taskSnapshots() const
{
    std::vector<TaskSnapshot> snapshots;
    snapshots.reserve(_tasks.size());
    for (const auto& entry : _tasks) {
        const Fibre& fibre = *entry.second;
        snapshots.push_back(TaskSnapshot{
            entry.first,
            fibre.name,
            fibre.state,
            fibre.sleepReason,
            fibre.wakeDeadlineUs,
            !fibre.finished,
            fibre.notificationValue,
        });
    }
    return snapshots;
}

VirtualPlatform::SchedulerSnapshot VirtualPlatform::schedulerSnapshot() const noexcept
{
    SchedulerSnapshot snapshot{};
    snapshot.nowUs = _clock.nowUs();
    snapshot.scheduledTask = _current ? _current->handle : nullptr;
    snapshot.pumpActive = _dispatching;
    snapshot.taskCount = _tasks.size();
    for (const auto& entry : _tasks) {
        const Fibre& fibre = *entry.second;
        switch (fibre.state) {
            case TaskState::Ready:
                ++snapshot.readyTaskCount;
                break;
            case TaskState::Sleeping:
                ++snapshot.sleepingTaskCount;
                if (fibre.sleepReason == TaskSleepReason::PredicateWait) {
                    ++snapshot.predicateWaitTaskCount;
                }
                break;
            case TaskState::Running:
            case TaskState::Completed:
                break;
        }
    }
    return snapshot;
}

} // namespace platform
} // namespace knx
