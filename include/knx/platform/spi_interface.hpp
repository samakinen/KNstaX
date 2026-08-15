// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file spi_interface.hpp
 * @brief SPI abstraction interface
 * 
 * Provides abstraction for SPI communication (for RF transceivers like CC1101)
 */

#pragma once

#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <cstddef>
#include <span>

namespace knx {
namespace platform {

/**
 * @brief SPI mode
 */
enum class SpiMode : uint8_t {
    Mode0 = 0,  ///< CPOL=0, CPHA=0
    Mode1 = 1,  ///< CPOL=0, CPHA=1
    Mode2 = 2,  ///< CPOL=1, CPHA=0
    Mode3 = 3   ///< CPOL=1, CPHA=1
};

/**
 * @brief SPI bit order
 */
enum class SpiBitOrder : uint8_t {
    MsbFirst,
    LsbFirst
};

/**
 * @brief Chip select level
 */
enum class ChipSelectLevel : uint8_t {
    Deassert = 0,
    Assert = 1
};

/**
 * @brief SPI configuration
 */
struct SpiConfig {
    uint32_t clockHz{1000000};          ///< SPI clock frequency in Hz
    SpiMode mode{SpiMode::Mode0};       ///< SPI mode
    SpiBitOrder bitOrder{SpiBitOrder::MsbFirst};  ///< Bit order
    int csPin{-1};                      ///< Manual GPIO CS pin (>= 0 enables GPIO-controlled CS; < 0 disables manual CS)
    bool csActiveHigh{false};           ///< CS active level
};

/**
 * @brief SPI interface abstraction
 */
class SpiInterface {
public:
    virtual ~SpiInterface() = default;
    
    /**
     * @brief Initialize SPI (Result-based)
     * @param config SPI configuration
     * @return Result<void> indicating success or error
     */
    virtual util::Result<void> init(const SpiConfig& config) = 0;
    
    /**
     * @brief Close SPI
     */
    virtual void close() = 0;
    
    /**
     * @brief Check if SPI is initialized
     */
    virtual bool isOpen() const = 0;
    
    /**
     * @brief Begin SPI transaction (assert CS if configured)
     */
    virtual void beginTransaction() = 0;
    
    /**
     * @brief End SPI transaction (deassert CS if configured)
     */
    virtual void endTransaction() = 0;
    
    /**
     * @brief Transfer single byte
     * @param data Byte to send
     * @return Received byte
     */
    virtual uint8_t transfer(uint8_t data) = 0;
    
    /**
     * @brief Transfer multiple bytes
     * @param tx Transmit buffer (or empty to send 0x00)
     * @param rx Receive buffer (or empty to discard)
     * @return Number of bytes transferred
     */
    virtual size_t transfer(std::span<const uint8_t> tx, std::span<uint8_t> rx) = 0;

    /**
     * @brief Transfer data (in-place)
     * @param buffer Buffer for both TX and RX
     * @return Number of bytes transferred
     */
    virtual size_t transfer(std::span<uint8_t> buffer) = 0;

    /**
     * @brief Convenience write-only transfer
     * @param data Data to transmit
     * @return Number of bytes written
     */
    virtual size_t write(std::span<const uint8_t> data) {
        return transfer(data, std::span<uint8_t>{});
    }

    /**
     * @brief Convenience read-only transfer
     * @param buffer Buffer to receive into
     * @return Number of bytes read
     */
    virtual size_t read(std::span<uint8_t> buffer) {
        return transfer(std::span<const uint8_t>{}, buffer);
    }
    
    /**
     * @brief Manually control CS pin
        * @param level Assert or deassert chip select
     */
        virtual void setCs(ChipSelectLevel level) = 0;
    
protected:
    SpiInterface() = default;
};

} // namespace platform
} // namespace knx
