// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file platform.hpp
 * @brief Platform abstraction interface
 * 
 * Defines the interface that must be implemented by platform-specific code.
 * This abstraction allows the KNX stack to run on different hardware and RTOS platforms.
 */

#pragma once

#include "knx/types.hpp"
#include "knx/config.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace knx {
namespace platform {

// Forward declarations
class MemoryInterface;
class NetworkInterface;
class UartInterface;
class SpiInterface;

// ============================================================================
// Threading Primitives
// ============================================================================

using TaskHandle = void*;
using MutexHandle = void*;
using QueueHandle = void*;
using EventGroupHandle = void*;
using SemaphoreHandle = void*;
using TimerHandle = void*;

using TaskFunction = std::function<void()>;

struct TaskConfig {
    const char* name;
    TaskFunction function;
    uint32_t stackSize;
    uint32_t priority;
    void* parameter{nullptr};
};

enum class EventGroupClearMode : uint8_t {
    Keep = 0,
    Clear = 1
};

enum class EventGroupWaitMode : uint8_t {
    Any = 0,
    All = 1
};

enum class TaskNotifyClearMode : uint8_t {
    Keep = 0,
    Clear = 1
};

// ============================================================================
// Narrow Service Interfaces
// ============================================================================

class TimingPlatform {
public:
    virtual ~TimingPlatform() = default;

    /**
     * @brief Get milliseconds since boot
     */
    virtual uint32_t millis() const = 0;

    /**
     * @brief Get microseconds since boot
     */
    virtual uint64_t micros() const = 0;

    /**
     * @brief Delay for specified milliseconds
     */
    virtual void delay(uint32_t ms) = 0;

    /**
     * @brief Delay for specified microseconds
     */
    virtual void delayMicroseconds(uint32_t us) = 0;

protected:
    TimingPlatform() = default;
};

class QueuePlatform {
public:
    virtual ~QueuePlatform() = default;

    /**
     * @brief Create a queue
     */
    virtual QueueHandle createQueue(size_t itemSize, size_t length) = 0;

    /**
     * @brief Delete a queue
     */
    virtual void deleteQueue(QueueHandle queue) = 0;

    /**
     * @brief Send to queue
     */
    virtual util::Result<void> queueSend(QueueHandle queue, const void* item, uint32_t timeout_ms = 0) = 0;

    /**
     * @brief Receive from queue
     */
    virtual util::Result<void> queueReceive(QueueHandle queue, void* item, uint32_t timeout_ms = 0) = 0;

    /**
     * @brief Get number of items in queue
     */
    virtual size_t queueCount(QueueHandle queue) = 0;

protected:
    QueuePlatform() = default;
};

class MutexPlatform {
public:
    virtual ~MutexPlatform() = default;

    /**
     * @brief Create a mutex
     */
    virtual MutexHandle createMutex() = 0;

    /**
     * @brief Delete a mutex
     */
    virtual void deleteMutex(MutexHandle mutex) = 0;

    /**
     * @brief Lock a mutex
     */
    virtual util::Result<void> lockMutex(MutexHandle mutex, uint32_t timeout_ms = UINT32_MAX) = 0;

    /**
     * @brief Unlock a mutex
     */
    virtual void unlockMutex(MutexHandle mutex) = 0;

protected:
    MutexPlatform() = default;
};

class TaskPlatform {
public:
    virtual ~TaskPlatform() = default;

    /**
     * @brief Create a task
     */
    virtual TaskHandle createTask(const TaskConfig& config) = 0;

    /**
     * @brief Delete a task
     */
    virtual void deleteTask(TaskHandle task) = 0;

    /**
     * @brief Get current task handle
     */
    virtual TaskHandle currentTask() = 0;

    /**
     * @brief Delay current task
     */
    virtual void taskDelay(uint32_t ms) = 0;

    /**
     * @brief Yield current task
     */
    virtual void taskYield() = 0;

    /**
     * @brief Notify a task
     */
    virtual util::Result<void> taskNotifyGive(TaskHandle task) = 0;

    /**
     * @brief ISR-safe notify a task
     */
    virtual util::Result<void> taskNotifyGiveFromISR(TaskHandle task) = 0;

    /**
     * @brief Wait for task notification
     */
    virtual uint32_t taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t timeout_ms = UINT32_MAX) = 0;

protected:
    TaskPlatform() = default;
};

class TaskingPlatform : public TaskPlatform, public MutexPlatform {
protected:
    TaskingPlatform() = default;
};

// ============================================================================
// Platform Interface
// ============================================================================

/**
 * @brief Abstract platform interface
 * 
 * This class provides the abstraction layer between the KNX stack and
 * the underlying hardware and RTOS. Implementations must provide concrete
 * implementations for memory, networking, timing, and threading functionality.
 */
class Platform : public TimingPlatform, public QueuePlatform, public TaskingPlatform {
public:
    virtual ~Platform() = default;
    
    // ========================================================================
    // System Control
    // ========================================================================
    
    /**
     * @brief Restart the system
     */
    virtual void restart() = 0;
    
    /**
     * @brief Handle fatal error (halt or restart)
     */
    virtual void fatalError() = 0;
    
    /**
     * @brief Get milliseconds since boot
     */
    virtual uint32_t millis() const override = 0;
    
    /**
     * @brief Get microseconds since boot
     */
    virtual uint64_t micros() const override = 0;
    
    /**
     * @brief Delay for specified milliseconds
     */
    virtual void delay(uint32_t ms) override = 0;
    
    /**
     * @brief Delay for specified microseconds
     */
    virtual void delayMicroseconds(uint32_t us) override = 0;
    
    /**
     * @brief Get unique serial number
     */
    virtual uint32_t uniqueSerialNumber() const = 0;
    
    /**
     * @brief Get MAC address (for IP platforms)
     */
    virtual void macAddress(std::span<uint8_t, 6> mac) const = 0;

    /**
     * @brief Fill @p out with unpredictable octets.
     *
     * Used for KNX Secure nonces — the Random of an S-A_Sync_Response, whose
     * only requirement is never to repeat for a given key.  Platforms with a
     * hardware or OS CSPRNG must override this; the fallback below mixes the
     * clock, the device serial and a call counter, which is good enough for a
     * nonce but is *not* a source for key material.
     */
    virtual void randomBytes(std::span<uint8_t> out) {
        static uint64_t counter = 0;
        uint64_t state = micros() ^ (static_cast<uint64_t>(uniqueSerialNumber()) << 32) ^ (++counter * 0x9E3779B97F4A7C15ULL);
        for (auto& byte : out) {
            // SplitMix64
            state += 0x9E3779B97F4A7C15ULL;
            uint64_t z = state;
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
            byte = static_cast<uint8_t>((z ^ (z >> 31)) & 0xFFu);
        }
    }
    
    // ========================================================================
    // Memory Access
    // ========================================================================
    
    /**
     * @brief Get memory interface
     */
    virtual MemoryInterface& memory() = 0;
    
    // ========================================================================
    // Hardware Interfaces
    // ========================================================================
    
    /**
     * @brief Get network interface (for IP platforms)
     * @return nullptr if network not available
     */
    virtual NetworkInterface* network() = 0;
    
    /**
     * @brief Get UART interface (for TP1 platforms)
     * @return nullptr if UART not available
     */
    virtual UartInterface* uart() = 0;
    
    /**
     * @brief Get SPI interface (for RF platforms)
     * @return nullptr if SPI not available
     */
    virtual SpiInterface* spi() = 0;
    
    // ========================================================================
    // Threading
    // ========================================================================
    
    /**
     * @brief Create a task
     * @return Task handle or nullptr on failure
     */
    virtual TaskHandle createTask(const TaskConfig& config) override = 0;
    
    /**
     * @brief Delete a task
     */
    virtual void deleteTask(TaskHandle task) override = 0;
    
    /**
     * @brief Get current task handle
     */
    virtual TaskHandle currentTask() override = 0;
    
    /**
     * @brief Delay current task
     */
    virtual void taskDelay(uint32_t ms) override = 0;
    
    /**
     * @brief Yield current task
     */
    virtual void taskYield() override = 0;
    
    // ========================================================================
    // Synchronization
    // ========================================================================
    
    /**
     * @brief Create a mutex
     */
    virtual MutexHandle createMutex() override = 0;
    
    /**
     * @brief Delete a mutex
     */
    virtual void deleteMutex(MutexHandle mutex) override = 0;
    
    /**
     * @brief Lock a mutex
     * @param timeout_ms Timeout in milliseconds (0 = no wait, UINT32_MAX = infinite)
     * @return true if locked, false on timeout
     */
    virtual util::Result<void> lockMutex(MutexHandle mutex, uint32_t timeout_ms = UINT32_MAX) override = 0;
    
    /**
     * @brief Unlock a mutex
     */
    virtual void unlockMutex(MutexHandle mutex) override = 0;
    
    /**
     * @brief Create a queue
     */
    virtual QueueHandle createQueue(size_t itemSize, size_t length) override = 0;
    
    /**
     * @brief Delete a queue
     */
    virtual void deleteQueue(QueueHandle queue) override = 0;
    
    /**
     * @brief Send to queue
     * @return true if sent, false on timeout
     */
    virtual util::Result<void> queueSend(QueueHandle queue, const void* item, uint32_t timeout_ms = 0) override = 0;
    
    /**
     * @brief Receive from queue
     * @return true if received, false on timeout
     */
    virtual util::Result<void> queueReceive(QueueHandle queue, void* item, uint32_t timeout_ms = 0) override = 0;
    
    /**
     * @brief Get number of items in queue
     */
    virtual size_t queueCount(QueueHandle queue) override = 0;
    
    /**
     * @brief Create event group
     */
    virtual EventGroupHandle createEventGroup() = 0;
    
    /**
     * @brief Delete event group
     */
    virtual void deleteEventGroup(EventGroupHandle group) = 0;
    
    /**
     * @brief Set event bits
     */
    virtual void eventGroupSetBits(EventGroupHandle group, uint32_t bits) = 0;
    
    /**
     * @brief Clear event bits
     */
    virtual void eventGroupClearBits(EventGroupHandle group, uint32_t bits) = 0;
    
    /**
     * @brief Wait for event bits
     * @return Bits that caused the wait to end
     */
    virtual uint32_t eventGroupWaitBits(EventGroupHandle group, uint32_t bitsToWaitFor,
                                        EventGroupClearMode clearOnExit, EventGroupWaitMode waitForAll,
                                        uint32_t timeout_ms = UINT32_MAX) = 0;
    
    /**
     * @brief Create binary semaphore
     */
    virtual SemaphoreHandle createBinarySemaphore() = 0;
    
    /**
     * @brief Delete semaphore
     */
    virtual void deleteSemaphore(SemaphoreHandle sem) = 0;
    
    /**
     * @brief Give semaphore
     */
    virtual util::Result<void> semaphoreGive(SemaphoreHandle sem) = 0;
    
    /**
     * @brief Take semaphore
     */
    virtual util::Result<void> semaphoreTake(SemaphoreHandle sem, uint32_t timeout_ms = 0) = 0;
    
    /**
     * @brief Notify a task (increment notification value)
     * @param task Task handle to notify (nullptr = current task)
     * @return true if notification sent successfully
     */
    virtual util::Result<void> taskNotifyGive(TaskHandle task) override = 0;
    
    /**
     * @brief Wait for task notification
        * @param clearMode Clear notification value on exit
     * @param timeout_ms Timeout in milliseconds (UINT32_MAX = infinite)
     * @return Notification value
     */
        virtual uint32_t taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t timeout_ms = UINT32_MAX) override = 0;
    
    // ========================================================================
    // Logging
    // ========================================================================
    
    /**
     * @brief Log a message
     */
    virtual void log(const char* level, const char* tag, const char* format, ...) = 0;
    
protected:
    Platform() = default;
};

// ============================================================================
// RAII Helpers
// ============================================================================

/**
 * @brief RAII mutex lock guard
 */
class MutexGuard {
public:
    MutexGuard(Platform& platform, MutexHandle mutex, uint32_t timeout_ms = UINT32_MAX)
        : _platform(platform), _mutex(mutex), _locked(false) {
        _locked = _platform.lockMutex(_mutex, timeout_ms).isOk();
    }
    
    ~MutexGuard() {
        if (_locked) {
            _platform.unlockMutex(_mutex);
        }
    }
    
    bool isLocked() const { return _locked; }
    
    // Non-copyable, non-movable
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
    MutexGuard(MutexGuard&&) = delete;
    MutexGuard& operator=(MutexGuard&&) = delete;
    
private:
    Platform& _platform;
    MutexHandle _mutex;
    bool _locked;
};

} // namespace platform
} // namespace knx
