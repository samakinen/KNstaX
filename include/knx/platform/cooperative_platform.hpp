// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file cooperative_platform.hpp
 * @brief Platform for single-threaded, RTOS-free targets (bare-metal Cortex-M and similar)
 *
 * `CooperativePlatform` implements `platform::Platform` for a target that runs
 * one main loop plus interrupts. The product supplies the few things only the
 * board knows (time, serial number, restart, a log sink) through
 * `CooperativeHooks`; everything else is fixed behaviour for one thread:
 *
 * - **Tasks** are refused: `createTask()` returns `nullptr`. The TP1 data link
 *   layer must run with `enableRxTask = false` and is driven by
 *   `processRxAvailable()` from the main loop.
 * - **Mutexes** are no-ops that always succeed. A lock cannot be contended
 *   when nothing preempts the holder. Data shared with an interrupt must use a
 *   critical section instead.
 * - **Queues** are fixed rings allocated in `createQueue()`. Call it during
 *   initialisation only; a send to a full ring or a receive from an empty one
 *   fails immediately because nothing could make progress while waiting.
 * - **Event groups and semaphores** are small counters with the same
 *   no-waiting rule.
 * - **Task notification** from an interrupt sets a flag that the main loop
 *   reads with `consumeNotification()`.
 *
 * @thread_safety Single thread only. The one exception is
 *                `taskNotifyGiveFromISR()`, which only stores to a volatile
 *                flag and may be called from an interrupt handler.
 */

#pragma once

#include "knx/platform/platform.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace knx::platform {

/**
 * @brief Board services the cooperative platform cannot supply itself
 *
 * All pointers except `millis` and `micros` may be null; the matching platform
 * call then does the documented fallback.
 */
struct CooperativeHooks {
    void* context{nullptr};
    /// Milliseconds since boot. Required.
    uint32_t (*millis)(void* context){nullptr};
    /// Microseconds since boot. Required.
    uint64_t (*micros)(void* context){nullptr};
    /// Called while `delay()` waits, for example to execute `WFI`. Null: busy-wait.
    void (*idle)(void* context){nullptr};
    /// Unique 32-bit device number. Null: 0.
    uint32_t (*serialNumber)(void* context){nullptr};
    /// Reset the MCU. Null: `restart()` spins forever.
    void (*restart)(void* context){nullptr};
    /// Unrecoverable error. Null: `fatalError()` spins forever.
    void (*fatalError)(void* context){nullptr};
    /// Receives one formatted log line (no trailing newline). Null: log output is dropped.
    /// Never called in a build with `CONFIG_KNX_LOG_LEVEL` 0, which links no formatting code.
    void (*logLine)(void* context, const char* line){nullptr};
};

/**
 * @brief `Platform` for one thread of execution (see file comment)
 */
class CooperativePlatform final : public Platform {
public:
    explicit CooperativePlatform(const CooperativeHooks& hooks);
    ~CooperativePlatform() override;

    CooperativePlatform(const CooperativePlatform&) = delete;
    CooperativePlatform& operator=(const CooperativePlatform&) = delete;

    /**
     * @brief Read and clear the "notified" flag set by `taskNotifyGive*()`
     * @thread_safety Main context only.
     */
    bool consumeNotification();

    void restart() override;
    void fatalError() override;
    uint32_t millis() const override;
    uint64_t micros() const override;
    void delay(uint32_t ms) override;
    void delayMicroseconds(uint32_t us) override;
    uint32_t uniqueSerialNumber() const override;
    void macAddress(std::span<uint8_t, 6> mac) const override;
    NetworkInterface* network() override;

    TaskHandle createTask(const TaskConfig& config) override;
    void deleteTask(TaskHandle task) override;
    TaskHandle currentTask() override;
    void taskDelay(uint32_t ms) override;
    void taskYield() override;
    util::Result<void> taskNotifyGive(TaskHandle task) override;
    util::Result<void> taskNotifyGiveFromISR(TaskHandle task) override;
    uint32_t taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t timeout_ms = UINT32_MAX) override;

    MutexHandle createMutex() override;
    void deleteMutex(MutexHandle mutex) override;
    util::Result<void> lockMutex(MutexHandle mutex, uint32_t timeout_ms = UINT32_MAX) override;
    void unlockMutex(MutexHandle mutex) override;

    QueueHandle createQueue(size_t itemSize, size_t length) override;
    void deleteQueue(QueueHandle queue) override;
    util::Result<void> queueSend(QueueHandle queue, const void* item, uint32_t timeout_ms = 0) override;
    util::Result<void> queueReceive(QueueHandle queue, void* item, uint32_t timeout_ms = 0) override;
    size_t queueCount(QueueHandle queue) override;

    EventGroupHandle createEventGroup() override;
    void deleteEventGroup(EventGroupHandle group) override;
    void eventGroupSetBits(EventGroupHandle group, uint32_t bits) override;
    void eventGroupClearBits(EventGroupHandle group, uint32_t bits) override;
    uint32_t eventGroupWaitBits(EventGroupHandle group, uint32_t bitsToWaitFor,
                                EventGroupClearMode clearOnExit, EventGroupWaitMode waitForAll,
                                uint32_t timeout_ms = UINT32_MAX) override;

    SemaphoreHandle createBinarySemaphore() override;
    void deleteSemaphore(SemaphoreHandle sem) override;
    util::Result<void> semaphoreGive(SemaphoreHandle sem) override;
    util::Result<void> semaphoreTake(SemaphoreHandle sem, uint32_t timeout_ms = 0) override;

    void log(const char* level, const char* tag, const char* format, ...) override;

private:
    struct Ring;
    struct EventGroup;
    struct Semaphore;

    CooperativeHooks _hooks;
    volatile bool _notified{false};
};

}  // namespace knx::platform
