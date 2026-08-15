// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_uart.hpp
 * @brief ESP32 UART implementation
 */

#pragma once

#include "knx/platform/uart_interface.hpp"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace knx {
namespace platform {

class Esp32Uart : public UartInterface {
public:
    Esp32Uart();
    virtual ~Esp32Uart();
    
    util::Result<void> init(const UartConfig& config) override;
    void close() override;
    bool isOpen() const override;
    
    size_t available() const override;
    int read() override;
    size_t read(std::span<uint8_t> buffer) override;
    size_t write(uint8_t byte) override;
    size_t write(std::span<const uint8_t> data) override;
    void flush() override;
    void clear() override;
    bool overflow() const override;
    
    void setRxCallback(RxCallback callback, void* context) override;
    
private:
    uart_port_t _uartNum;
    bool _initialized;
    RxCallback _rxCallback;
    void* _rxCallbackContext;
    QueueHandle_t _eventQueue;
    TaskHandle_t _eventTaskHandle;
    bool _overflowFlag;
    
    util::Result<void> startEventTask();
    void stopEventTask();
    static void uartEventTask(void* arg);
};

} // namespace platform
} // namespace knx
