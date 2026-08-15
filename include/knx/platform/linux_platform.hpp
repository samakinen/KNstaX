// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file linux_platform.hpp
 * @brief Linux/POSIX platform implementation
 * 
 * Implements the platform abstraction for Linux using POSIX threads and standard C++
 * Allows KNX stack to run on x86 Linux for testing and development
 */

#pragma once

#include "knx/platform/platform.hpp"
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <queue>
#include <atomic>
#include <map>
#include <array>

namespace knx {
namespace platform {

// Forward declarations
class LinuxMemory;
class LinuxNetwork;
class LinuxUart;
class LinuxSpi;

/**
 * @brief Linux platform implementation using POSIX threads
 * 
 * This implementation uses:
 * - std::thread for task management
 * - std::mutex for synchronization
 * - std::condition_variable for event signaling
 * - std::chrono for timing
 * - /dev/urandom for random numbers
 * 
 * Suitable for:
 * - Unit testing without hardware
 * - Development on Linux workstations
 * - CI/CD pipelines
 * - Simulation and prototyping
 */
class LinuxPlatform : public Platform {
public:
    LinuxPlatform();
    virtual ~LinuxPlatform();
    
    // ========================================================================
    // System Control
    // ========================================================================
    
    void restart() override;
    void fatalError() override;
    uint32_t millis() const override;
    uint64_t micros() const override;
    void delay(uint32_t ms) override;
    void delayMicroseconds(uint32_t us) override;
    uint32_t uniqueSerialNumber() const override;
    void macAddress(std::span<uint8_t, 6> mac) const override;
    void randomBytes(std::span<uint8_t> out) override;
    
    // ========================================================================
    // Threading
    // ========================================================================
    
    TaskHandle createTask(const TaskConfig& config) override;
    void deleteTask(TaskHandle task) override;
    TaskHandle currentTask() override;
    void taskDelay(uint32_t ms) override;
    void taskYield() override;
    
    // ========================================================================
    // Synchronization
    // ========================================================================
    
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
    uint32_t eventGroupWaitBits(EventGroupHandle eventGroup, uint32_t bits, 
                                 EventGroupClearMode clearOnExit, EventGroupWaitMode waitAll, uint32_t timeoutMs) override;

    util::Result<void> taskNotifyGive(TaskHandle task) override;
    util::Result<void> taskNotifyGiveFromISR(TaskHandle task) override;
    uint32_t taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t timeout_ms = UINT32_MAX) override;
    
    // ========================================================================
    // Hardware Interfaces
    // ========================================================================
    
    MemoryInterface& memory() override;
    NetworkInterface* network() override;
    UartInterface* uart() override;
    SpiInterface* spi() override;
    
    // ========================================================================
    // Logging
    // ========================================================================
    
    void log(const char* level, const char* tag, const char* format, ...) override;
    
private:
    // System timing
    std::chrono::steady_clock::time_point _startTime;
    
    // Task management
    struct TaskNotification {
        std::mutex mutex;
        std::condition_variable cv;
        uint32_t value{0};
    };

    struct TaskWrapper {
        std::thread thread;
        TaskFunction function;
        std::atomic<bool> running{true};
        TaskNotification notification;
        
        TaskWrapper(TaskFunction func) : function(func) {}
    };
    
    std::map<TaskHandle, std::unique_ptr<TaskWrapper>> _tasks;
    std::mutex _tasksMutex;
    uint64_t _nextTaskId{1};
    
    // Synchronization primitives
    struct PosixMutex {
        std::timed_mutex mutex;
    };
    
    struct PosixQueue {
        std::queue<std::vector<uint8_t>> items;
        std::mutex mutex;
        std::condition_variable notEmpty;
        std::condition_variable notFull;
        size_t itemSize;
        size_t itemCount;
        
        PosixQueue(size_t size, size_t count) 
            : itemSize(size), itemCount(count) {}
    };
    
    struct PosixSemaphore {
        std::mutex mutex;
        std::condition_variable cv;
        uint32_t count;
        
        PosixSemaphore(uint32_t initialCount) : count(initialCount) {}
    };
    
    struct PosixEventGroup {
        std::mutex mutex;
        std::condition_variable cv;
        uint32_t bits{0};
    };
    
    std::map<MutexHandle, std::unique_ptr<PosixMutex>> _mutexes;
    std::map<QueueHandle, std::unique_ptr<PosixQueue>> _queues;
    std::map<SemaphoreHandle, std::unique_ptr<PosixSemaphore>> _semaphores;
    std::map<EventGroupHandle, std::unique_ptr<PosixEventGroup>> _eventGroups;
    
    std::mutex _syncMutex;
    uint64_t _nextHandleId{1};
    
    // Hardware interfaces
    std::unique_ptr<LinuxMemory> _memory;
    std::unique_ptr<LinuxNetwork> _network;
    std::unique_ptr<LinuxUart> _uart;
    std::unique_ptr<LinuxSpi> _spi;
    
    // System info
    uint32_t _serialNumber;
    std::array<uint8_t, 6> _macAddress{};
    
    void initSystemInfo();
    TaskHandle generateTaskHandle();
    MutexHandle generateMutexHandle();
    QueueHandle generateQueueHandle();
    SemaphoreHandle generateSemaphoreHandle();
    EventGroupHandle generateEventGroupHandle();
};

} // namespace platform
} // namespace knx

