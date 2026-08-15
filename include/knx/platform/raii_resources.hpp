// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file raii_resources.hpp
 * @brief RAII wrappers for platform resources (mutexes, queues, semaphores)
 * 
 * Provides automatic resource management for platform handles to ensure
 * proper cleanup and exception safety.
 */

#pragma once

#include "knx/platform/platform.hpp"
#include <cstdint>

namespace knx {
namespace platform {

/**
 * @brief RAII wrapper for mutex handle
 * 
 * Automatically locks on construction and unlocks on destruction.
 * Provides exception-safe mutex locking.
 * 
 * @thread_safety Thread-safe - uses platform mutex primitives
 */
class MutexLock {
public:
    /**
     * @brief Construct and lock mutex
     * @param platform Platform instance
     * @param mutex Mutex handle to lock
     * @param timeout_ms Timeout in milliseconds (UINT32_MAX = wait forever)
     */
    MutexLock(MutexPlatform& platform, MutexHandle mutex, uint32_t timeout_ms = UINT32_MAX)
        : _platform(platform)
        , _mutex(mutex)
        , _locked(false)
    {
        _locked = _platform.lockMutex(_mutex, timeout_ms).isOk();
    }
    
    /**
     * @brief Destructor - automatically unlocks if locked
     */
    ~MutexLock() {
        if (_locked) {
            _platform.unlockMutex(_mutex);
        }
    }
    
    // Non-copyable
    MutexLock(const MutexLock&) = delete;
    MutexLock& operator=(const MutexLock&) = delete;
    
    // Movable
    MutexLock(MutexLock&& other) noexcept
        : _platform(other._platform)
        , _mutex(other._mutex)
        , _locked(other._locked)
    {
        other._locked = false;
    }
    
    /**
     * @brief Check if mutex was successfully locked
     * @return true if locked, false if timeout occurred
     */
    bool isLocked() const { return _locked; }
    
    /**
     * @brief Explicit unlock (called automatically in destructor)
     */
    void unlock() {
        if (_locked) {
            _platform.unlockMutex(_mutex);
            _locked = false;
        }
    }

private:
    MutexPlatform& _platform;
    MutexHandle _mutex;
    bool _locked;
};

/**
 * @brief RAII wrapper for mutex handle (ownership)
 * 
 * Automatically creates and deletes a mutex. The mutex is locked
 * on construction and unlocked on destruction.
 * 
 * @thread_safety Thread-safe - uses platform mutex primitives
 */
class Mutex {
public:
    /**
     * @brief Construct and create mutex
     * @param platform Platform instance
     */
    explicit Mutex(MutexPlatform& platform)
        : _platform(platform)
        , _mutex(_platform.createMutex())
        , _locked(false)
    {
    }
    
    /**
     * @brief Destructor - automatically unlocks and deletes mutex
     */
    ~Mutex() {
        if (_mutex) {
            if (_locked) {
                _platform.unlockMutex(_mutex);
            }
            _platform.deleteMutex(_mutex);
        }
    }
    
    // Non-copyable
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    
    // Movable
    Mutex(Mutex&& other) noexcept
        : _platform(other._platform)
        , _mutex(other._mutex)
        , _locked(other._locked)
    {
        other._mutex = nullptr;
        other._locked = false;
    }
    
    /**
     * @brief Lock the mutex
     * @param timeout_ms Timeout in milliseconds (UINT32_MAX = wait forever)
     * @return true if locked, false if timeout occurred
     */
    util::Result<void> lock(uint32_t timeout_ms = UINT32_MAX) {
        if (!_mutex || _locked) {
            return util::Result<void>::err(util::ErrorCode::OperationFailed);
        }
        auto res = _platform.lockMutex(_mutex, timeout_ms);
        _locked = res.isOk();
        return res;
    }

    
    /**
     * @brief Unlock the mutex
     */
    void unlock() {
        if (_mutex && _locked) {
            _platform.unlockMutex(_mutex);
            _locked = false;
        }
    }
    
    /**
     * @brief Get the underlying mutex handle
     * @return Mutex handle (nullptr if invalid)
     */
    MutexHandle handle() const { return _mutex; }
    
    /**
     * @brief Check if mutex is currently locked
     * @return true if locked
     */
    bool isLocked() const { return _locked; }

private:
    MutexPlatform& _platform;
    MutexHandle _mutex;
    bool _locked;
};

/**
 * @brief RAII wrapper for queue handle (ownership)
 * 
 * Automatically creates and deletes a queue.
 * 
 * @thread_safety Thread-safe - uses platform queue primitives
 */
class Queue {
public:
    /**
     * @brief Construct and create queue
     * @param platform Platform instance
     * @param itemSize Size of each queue item in bytes
     * @param length Maximum number of items in queue
     */
    Queue(QueuePlatform& platform, size_t itemSize, size_t length)
        : _platform(platform)
        , _queue(_platform.createQueue(itemSize, length))
    {
    }
    
    /**
     * @brief Destructor - automatically deletes queue
     */
    ~Queue() {
        if (_queue) {
            _platform.deleteQueue(_queue);
        }
    }
    
    // Non-copyable
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;
    
    // Movable
    Queue(Queue&& other) noexcept
        : _platform(other._platform)
        , _queue(other._queue)
    {
        other._queue = nullptr;
    }
    
    /**
     * @brief Send item to queue
     * @param item Pointer to item data
     * @param timeout_ms Timeout in milliseconds (0 = non-blocking)
     * @return true if sent, false on timeout or error
     */
    util::Result<void> send(const void* item, uint32_t timeout_ms = 0) {
        if (!_queue) {
            return util::Result<void>::err(util::ErrorCode::NotInitialized);
        }
        return _platform.queueSend(_queue, item, timeout_ms);
    }

    
    /**
     * @brief Receive item from queue
     * @param item Pointer to buffer for received item
     * @param timeout_ms Timeout in milliseconds (0 = non-blocking)
     * @return true if received, false on timeout or error
     */
    util::Result<void> receive(void* item, uint32_t timeout_ms = 0) {
        if (!_queue) {
            return util::Result<void>::err(util::ErrorCode::NotInitialized);
        }
        return _platform.queueReceive(_queue, item, timeout_ms);
    }

    
    /**
     * @brief Get number of items currently in queue
     * @return Number of items
     */
    size_t count() const {
        if (!_queue) {
            return 0;
        }
        return _platform.queueCount(_queue);
    }
    
    /**
     * @brief Get the underlying queue handle
     * @return Queue handle (nullptr if invalid)
     */
    QueueHandle handle() const { return _queue; }

private:
    QueuePlatform& _platform;
    QueueHandle _queue;
};

/**
 * @brief RAII wrapper for semaphore handle (ownership)
 * 
 * Automatically creates and deletes a binary semaphore.
 * 
 * @thread_safety Thread-safe - uses platform semaphore primitives
 */
class Semaphore {
public:
    /**
     * @brief Construct and create semaphore
     * @param platform Platform instance
     */
    explicit Semaphore(Platform& platform)
        : _platform(platform)
        , _semaphore(_platform.createBinarySemaphore())
    {
    }
    
    /**
     * @brief Destructor - automatically deletes semaphore
     */
    ~Semaphore() {
        if (_semaphore) {
            _platform.deleteSemaphore(_semaphore);
        }
    }
    
    // Non-copyable
    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;
    
    // Movable
    Semaphore(Semaphore&& other) noexcept
        : _platform(other._platform)
        , _semaphore(other._semaphore)
    {
        other._semaphore = nullptr;
    }
    
    /**
     * @brief Give (signal) the semaphore
     * @return true if successful
     */
    util::Result<void> give() {
        if (!_semaphore) {
            return util::Result<void>::err(util::ErrorCode::NotInitialized);
        }
        return _platform.semaphoreGive(_semaphore);
    }

    
    /**
     * @brief Take (wait for) the semaphore
     * @param timeout_ms Timeout in milliseconds (0 = non-blocking, UINT32_MAX = wait forever)
     * @return true if taken, false on timeout or error
     */
    util::Result<void> take(uint32_t timeout_ms = UINT32_MAX) {
        if (!_semaphore) {
            return util::Result<void>::err(util::ErrorCode::NotInitialized);
        }
        return _platform.semaphoreTake(_semaphore, timeout_ms);
    }

    
    /**
     * @brief Get the underlying semaphore handle
     * @return Semaphore handle (nullptr if invalid)
     */
    SemaphoreHandle handle() const { return _semaphore; }

private:
    Platform& _platform;
    SemaphoreHandle _semaphore;
};

} // namespace platform
} // namespace knx
