// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_spi.hpp
 * @brief ESP32 SPI implementation
 */

#pragma once

#include "knx/platform/spi_interface.hpp"
#include "driver/spi_master.h"

namespace knx {
namespace platform {

class Esp32Spi : public SpiInterface {
public:
    Esp32Spi();
    virtual ~Esp32Spi();
    
    util::Result<void> init(const SpiConfig& config) override;
    void close() override;
    bool isOpen() const override;
    
    void beginTransaction() override;
    void endTransaction() override;
    uint8_t transfer(uint8_t data) override;
    size_t transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) override;
    size_t transfer(std::span<uint8_t> buffer) override;
    void setCs(ChipSelectLevel level) override;
    
private:
    spi_device_handle_t _spiDevice;
    bool _initialized;
    int _csPin;
    bool _csActiveHigh;
};

} // namespace platform
} // namespace knx
