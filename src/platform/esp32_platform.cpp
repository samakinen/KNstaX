// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_platform.cpp
 * @brief ESP32 platform implementation
 */

#include "knx/platform/esp32_platform.hpp"
#include "esp32_memory.hpp"
#include "esp32_network.hpp"
#include "esp32_uart.hpp"
#include "esp32_spi.hpp"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "knx/util/log.hpp"
#include <cstring>

static const char* TAG = "KNX.Platform";

namespace knx {
namespace platform {

Esp32Platform::Esp32Platform()
    : FreeRtosPlatform()
    , _memory(std::make_unique<Esp32Memory>())
    , _network(nullptr)
    , _uart(nullptr)
    , _spi(nullptr)
    , _serialNumber(0)
{
    std::memset(_macAddress, 0, sizeof(_macAddress));
    initSerialNumber();
    
    KNX_LOGD(TAG, "ESP32 Platform initialized");
    KNX_LOGD(TAG, "Serial Number: 0x%08X", static_cast<unsigned int>(_serialNumber));
}

Esp32Platform::~Esp32Platform() {
    KNX_LOGD(TAG, "ESP32 Platform destroyed");
}

void Esp32Platform::restart() {
    KNX_LOGW(TAG, "Restarting system...");
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow logs to flush
    esp_restart();
}

void Esp32Platform::fatalError() {
    KNX_LOGE(TAG, "FATAL ERROR - System halted");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

uint32_t Esp32Platform::uniqueSerialNumber() const {
    return _serialNumber;
}

void Esp32Platform::randomBytes(std::span<uint8_t> out) {
    // esp_fill_random draws from the hardware RNG once RF or the ADC is
    // running, and from a PRNG seeded by it otherwise.
    if (!out.empty()) {
        esp_fill_random(out.data(), out.size());
    }
}

void Esp32Platform::macAddress(std::span<uint8_t, 6> mac) const {
    std::memcpy(mac.data(), _macAddress, mac.size_bytes());
}

MemoryInterface& Esp32Platform::memory() {
    return *_memory;
}

NetworkInterface* Esp32Platform::network() {
    if (!_network) {
        _network = std::make_unique<Esp32Network>();
    }
    return _network.get();
}

UartInterface* Esp32Platform::uart() {
    if (!_uart) {
        _uart = std::make_unique<Esp32Uart>();
    }
    return _uart.get();
}

SpiInterface* Esp32Platform::spi() {
    if (!_spi) {
        _spi = std::make_unique<Esp32Spi>();
    }
    return _spi.get();
}

void Esp32Platform::log(const char* level, const char* tag, const char* format, ...) {
    va_list args;
    va_start(args, format);
    
    // Convert level string to ESP log level
    esp_log_level_t esp_level = ESP_LOG_INFO;
    if (std::strcmp(level, "ERROR") == 0) {
        esp_level = ESP_LOG_ERROR;
    } else if (std::strcmp(level, "WARN") == 0) {
        esp_level = ESP_LOG_WARN;
    } else if (std::strcmp(level, "INFO") == 0) {
        esp_level = ESP_LOG_INFO;
    } else if (std::strcmp(level, "DEBUG") == 0) {
        esp_level = ESP_LOG_DEBUG;
    } else if (std::strcmp(level, "VERBOSE") == 0) {
        esp_level = ESP_LOG_VERBOSE;
    }
    
    esp_log_writev(esp_level, tag, format, args);
    va_end(args);
}

void Esp32Platform::initSerialNumber() {
    // Get MAC address for serial number derivation
    esp_err_t err = esp_efuse_mac_get_default(_macAddress);
    if (err != ESP_OK) {
        KNX_LOGW(TAG, "Failed to get MAC address: %s", esp_err_to_name(err));
        // Use chip ID as fallback
        _serialNumber = esp_random();
    } else {
        // Derive 32-bit serial from 48-bit MAC
        _serialNumber = (_macAddress[2] << 24) |
                       (_macAddress[3] << 16) |
                       (_macAddress[4] << 8) |
                       _macAddress[5];
    }
}

} // namespace platform
} // namespace knx
