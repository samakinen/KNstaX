// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_spi.cpp
 * @brief ESP32 SPI implementation
 */

#include "esp32_spi.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "knx/util/log.hpp"

static const char* TAG = "KNX_SPI";

namespace knx {
namespace platform {

Esp32Spi::Esp32Spi()
    : _spiDevice(nullptr)
    , _initialized(false)
    , _csPin(-1)
    , _csActiveHigh(false) {
}

Esp32Spi::~Esp32Spi() {
    close();
}

util::Result<void> Esp32Spi::init(const SpiConfig& config) {
    if (_initialized) {
        KNX_LOGW(TAG, "SPI already initialized");
        return util::Result<void>::ok();
    }
    
    // Configure device interface; assume bus already initialized externally on SPI2_HOST
    spi_device_interface_config_t dev_config = {};
    dev_config.clock_speed_hz = static_cast<int>(config.clockHz);
    dev_config.mode = static_cast<uint8_t>(config.mode);
    dev_config.spics_io_num = config.csPin;
    dev_config.queue_size = 4;
    dev_config.flags = 0;
    if (config.bitOrder == SpiBitOrder::LsbFirst) {
        dev_config.flags |= SPI_DEVICE_BIT_LSBFIRST;
    }

    esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev_config, &_spiDevice);
    if (err != ESP_OK) {
        KNX_LOGW(TAG, "SPI device add failed: %s (bus init required)", esp_err_to_name(err));
        _spiDevice = nullptr;
        // Continue with manual CS only
    }

    _csPin = config.csPin;
    _csActiveHigh = config.csActiveHigh;
    _initialized = true;
    
    KNX_LOGI(TAG, "SPI initialized: speed=%u Hz, mode=%d", 
             static_cast<unsigned>(config.clockHz), static_cast<int>(config.mode));
    
    return util::Result<void>::ok();
}

void Esp32Spi::close() {
    if (_initialized && _spiDevice) {
        spi_bus_remove_device(_spiDevice);
        _spiDevice = nullptr;
        _initialized = false;
    }
}

bool Esp32Spi::isOpen() const {
    return _initialized;
}

void Esp32Spi::beginTransaction() {
    setCs(ChipSelectLevel::Assert);
}

void Esp32Spi::endTransaction() {
    setCs(ChipSelectLevel::Deassert);
}

uint8_t Esp32Spi::transfer(uint8_t data) {
    uint8_t rx = 0;
    (void)transfer(std::span<const uint8_t>(&data, 1), std::span<uint8_t>(&rx, 1));
    return rx;
}

size_t Esp32Spi::transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) {
    if (!_initialized) return 0;
    const size_t length = tx.size() ? tx.size() : rx.size();
    if (length == 0) return 0;
    if (!_spiDevice) {
        KNX_LOGW(TAG, "SPI transfer skipped: device not added");
        return 0;
    }

    spi_transaction_t trans = {};
    trans.length = static_cast<int>(length * 8); // in bits
    trans.tx_buffer = tx.empty() ? nullptr : tx.data();
    trans.rx_buffer = rx.empty() ? nullptr : rx.data();

    esp_err_t err = spi_device_polling_transmit(_spiDevice, &trans);
    if (err != ESP_OK) {
        KNX_LOGE(TAG, "SPI transfer failed: %s", esp_err_to_name(err));
        return 0;
    }
    return length;
}

size_t Esp32Spi::transfer(std::span<uint8_t> buffer) {
    return transfer(buffer, buffer);
}

void Esp32Spi::setCs(ChipSelectLevel level) {
    if (_csPin < 0) return;
    const bool asserted = (level == ChipSelectLevel::Assert);
    bool active = asserted ? _csActiveHigh : !_csActiveHigh;
    gpio_set_level(static_cast<gpio_num_t>(_csPin), active ? 1 : 0);
}

} // namespace platform
} // namespace knx
