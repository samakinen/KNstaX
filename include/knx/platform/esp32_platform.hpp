// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_platform.hpp
 * @brief ESP32 platform implementation
 * 
 * Implements the platform abstraction for ESP32 using ESP-IDF
 */

#pragma once

#include "knx/platform/freertos_platform.hpp"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"
#include <memory>

namespace knx {
namespace platform {

// Forward declarations
class Esp32Memory;
class Esp32Network;
class Esp32Uart;
class Esp32Spi;

/**
 * @brief ESP32 platform implementation
 */
class Esp32Platform : public FreeRtosPlatform {
public:
    Esp32Platform();
    virtual ~Esp32Platform();
    
    // System Control
    void restart() override;
    void fatalError() override;
    uint32_t uniqueSerialNumber() const override;
    void macAddress(std::span<uint8_t, 6> mac) const override;
    void randomBytes(std::span<uint8_t> out) override;
    
    // Memory
    MemoryInterface& memory() override;
    
    // Hardware Interfaces
    NetworkInterface* network() override;
    UartInterface* uart() override;
    SpiInterface* spi() override;
    
    // Logging
    void log(const char* level, const char* tag, const char* format, ...) override;
    
private:
    std::unique_ptr<Esp32Memory> _memory;
    std::unique_ptr<Esp32Network> _network;
    std::unique_ptr<Esp32Uart> _uart;
    std::unique_ptr<Esp32Spi> _spi;
    
    uint32_t _serialNumber;
    uint8_t _macAddress[6];
    
    void initSerialNumber();
};

} // namespace platform
} // namespace knx
