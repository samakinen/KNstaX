// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file freertos_platform.hpp
 * @brief FreeRTOS platform base implementation
 * 
 * Provides FreeRTOS-specific implementations of threading primitives
 */

#pragma once

#include "knx/platform/platform.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include <cstdarg>

namespace knx {
namespace platform {

namespace detail {

inline TickType_t millisecondsToDelayTicks(uint32_t ms)
{
    if (ms == 0u) {
        return 0;
    }

    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks == 0 ? 1 : ticks;
}

} // namespace detail

/**
 * @brief FreeRTOS platform base class
 * 
 * Implements threading and synchronization primitives using FreeRTOS API.
 * Hardware-specific functionality (memory, network, etc.) must be implemented
 * by derived classes.
 */
class FreeRtosPlatform : public Platform {
public:
    FreeRtosPlatform() = default;
    virtual ~FreeRtosPlatform() = default;
    
    // ========================================================================
    // System Control
    // ========================================================================
    
    uint32_t millis() const override {
        return xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
    
    uint64_t micros() const override {
        return esp_timer_get_time();
    }
    
    void delay(uint32_t ms) override {
        vTaskDelay(detail::millisecondsToDelayTicks(ms));
    }
    
    void delayMicroseconds(uint32_t us) override {
        esp_rom_delay_us(us);
    }
    
    // ========================================================================
    // Threading
    // ========================================================================
    
    TaskHandle createTask(const TaskConfig& config) override {
        TaskHandle_t handle = nullptr;
        
        // Create wrapper for std::function
        auto* func = new TaskFunction(config.function);
        
        BaseType_t result = xTaskCreate(
            taskWrapper,
            config.name,
            config.stackSize,
            func,
            config.priority,
            &handle
        );
        
        return (result == pdPASS) ? handle : nullptr;
    }
    
    void deleteTask(TaskHandle task) override {
        if (task) {
            vTaskDelete(static_cast<TaskHandle_t>(task));
        }
    }
    
    TaskHandle currentTask() override {
        return xTaskGetCurrentTaskHandle();
    }
    
    void taskDelay(uint32_t ms) override {
        vTaskDelay(detail::millisecondsToDelayTicks(ms));
    }
    
    void taskYield() override {
        taskYIELD();
    }
    
    // ========================================================================
    // Synchronization
    // ========================================================================
    
    MutexHandle createMutex() override {
        return xSemaphoreCreateMutex();
    }
    
    void deleteMutex(MutexHandle mutex) override {
        if (mutex) {
            vSemaphoreDelete(static_cast<SemaphoreHandle_t>(mutex));
        }
    }
    
    util::Result<void> lockMutex(MutexHandle mutex, uint32_t timeout_ms) override {
        if (!mutex) return util::ErrorCode::InvalidParameter;
        
        TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        return xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex), ticks) == pdTRUE
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::Timeout);
    }
    
    void unlockMutex(MutexHandle mutex) override {
        if (mutex) {
            xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex));
        }
    }
    
    QueueHandle createQueue(size_t itemSize, size_t length) override {
        return xQueueCreate(length, itemSize);
    }
    
    void deleteQueue(QueueHandle queue) override {
        if (queue) {
            vQueueDelete(static_cast<QueueHandle_t>(queue));
        }
    }
    
    util::Result<void> queueSend(QueueHandle queue, const void* item, uint32_t timeout_ms) override {
        if (!queue || !item) return util::ErrorCode::InvalidParameter;
        
        TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        return xQueueSend(static_cast<QueueHandle_t>(queue), item, ticks) == pdTRUE
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::Timeout);
    }
    
    util::Result<void> queueReceive(QueueHandle queue, void* item, uint32_t timeout_ms) override {
        if (!queue || !item) return util::ErrorCode::InvalidParameter;
        
        TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        return xQueueReceive(static_cast<QueueHandle_t>(queue), item, ticks) == pdTRUE
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::Timeout);
    }
    
    size_t queueCount(QueueHandle queue) override {
        if (!queue) return 0;
        return uxQueueMessagesWaiting(static_cast<QueueHandle_t>(queue));
    }
    
    EventGroupHandle createEventGroup() override {
        return xEventGroupCreate();
    }
    
    void deleteEventGroup(EventGroupHandle group) override {
        if (group) {
            vEventGroupDelete(static_cast<EventGroupHandle_t>(group));
        }
    }
    
    void eventGroupSetBits(EventGroupHandle group, uint32_t bits) override {
        if (group) {
            xEventGroupSetBits(static_cast<EventGroupHandle_t>(group), bits);
        }
    }
    
    void eventGroupClearBits(EventGroupHandle group, uint32_t bits) override {
        if (group) {
            xEventGroupClearBits(static_cast<EventGroupHandle_t>(group), bits);
        }
    }
    
    uint32_t eventGroupWaitBits(EventGroupHandle group, uint32_t bitsToWaitFor,
                                EventGroupClearMode clearOnExit, EventGroupWaitMode waitForAll,
                                uint32_t timeout_ms) override {
        if (!group) return 0;
        
        TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        return xEventGroupWaitBits(
            static_cast<EventGroupHandle_t>(group),
            bitsToWaitFor,
            (clearOnExit == EventGroupClearMode::Clear) ? pdTRUE : pdFALSE,
            (waitForAll == EventGroupWaitMode::All) ? pdTRUE : pdFALSE,
            ticks
        );
    }
    
    SemaphoreHandle createBinarySemaphore() override {
        return xSemaphoreCreateBinary();
    }
    
    void deleteSemaphore(SemaphoreHandle sem) override {
        if (sem) {
            vSemaphoreDelete(static_cast<SemaphoreHandle_t>(sem));
        }
    }
    
    util::Result<void> semaphoreGive(SemaphoreHandle sem) override {
        if (!sem) return util::ErrorCode::InvalidParameter;
        return xSemaphoreGive(static_cast<SemaphoreHandle_t>(sem)) == pdTRUE
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::OperationFailed);
    }
    
    util::Result<void> semaphoreTake(SemaphoreHandle sem, uint32_t timeout_ms) override {
        if (!sem) return util::ErrorCode::InvalidParameter;
        
        TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        return xSemaphoreTake(static_cast<SemaphoreHandle_t>(sem), ticks) == pdTRUE
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::Timeout);
    }
    
    util::Result<void> taskNotifyGive(TaskHandle task) override {
        const BaseType_t result = task
            ? xTaskNotifyGive(static_cast<TaskHandle_t>(task))
            : xTaskNotifyGive(xTaskGetCurrentTaskHandle());
        // xTaskNotifyGive returns pdTRUE when a task was resumed from waiting,
        // and pdFALSE when the notification value was simply incremented.
        // Both are valid completion cases for a notify-give operation.
        return (result == pdTRUE || result == pdFALSE)
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::OperationFailed);
    }

    util::Result<void> taskNotifyGiveFromISR(TaskHandle task) override {
        BaseType_t higherPriorityTaskWoken = pdFALSE;
        if (task) {
            vTaskNotifyGiveFromISR(static_cast<TaskHandle_t>(task), &higherPriorityTaskWoken);
        } else {
            vTaskNotifyGiveFromISR(xTaskGetCurrentTaskHandle(), &higherPriorityTaskWoken);
        }
        if (higherPriorityTaskWoken == pdTRUE) {
            portYIELD_FROM_ISR(higherPriorityTaskWoken);
        }
        return util::Result<void>::ok();
    }
    
    uint32_t taskNotifyTake(TaskNotifyClearMode clearMode, uint32_t timeout_ms) override {
        TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
        const BaseType_t clearOnExit = (clearMode == TaskNotifyClearMode::Clear) ? pdTRUE : pdFALSE;
        return ulTaskNotifyTake(clearOnExit, ticks);
    }
    
protected:
    /**
     * @brief Task wrapper function
     */
    static void taskWrapper(void* param) {
        auto* func = static_cast<TaskFunction*>(param);
        (*func)();
        delete func;
        vTaskDelete(nullptr);
    }
};

} // namespace platform
} // namespace knx
