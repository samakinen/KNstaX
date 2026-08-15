// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file uart_interface.hpp
 * @brief UART abstraction interface
 * 
 * Provides abstraction for UART communication (for TP1 TPUART interfaces)
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
 * @brief UART configuration
 */
struct UartConfig {
    uint32_t baudRate{19200};      ///< Baud rate (19200 for TPUART)
    uint8_t dataBits{8};            ///< Data bits (8 for TPUART)
    uint8_t stopBits{1};            ///< Stop bits (1 for TPUART)
    enum class Parity {
        None,
        Even,
        Odd
    } parity{Parity::Even};         ///< Parity (Even for TPUART)
    
    bool flowControl{false};        ///< Hardware flow control
    size_t rxBufferSize{256};       ///< RX buffer size
    size_t txBufferSize{256};       ///< TX buffer size
    
    /**
     * @brief Validate UART configuration parameters
     * @return Result<void> indicating success or specific error
     */
    util::Result<void> validate() const {
        // Validate baud rate (common values: 300-921600)
        if (baudRate < 300 || baudRate > 921600) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        
        // Validate data bits (5, 6, 7, or 8)
        if (dataBits < 5 || dataBits > 8) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        
        // Validate stop bits (1 or 2)
        if (stopBits < 1 || stopBits > 2) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        
        // Validate buffer sizes (must be > 0 and reasonable)
        if (rxBufferSize == 0 || rxBufferSize > 65536) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        if (txBufferSize == 0 || txBufferSize > 65536) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        
        return util::Result<void>::ok();
    }
};

/**
 * @brief UART interface abstraction
 */
class UartInterface {
public:
    virtual ~UartInterface() = default;
    
    // Delete copy and move operations (abstract base class)
    UartInterface(const UartInterface&) = delete;
    UartInterface& operator=(const UartInterface&) = delete;
    UartInterface(UartInterface&&) = delete;
    UartInterface& operator=(UartInterface&&) = delete;
    
    /**
     * @brief Initialize UART (Result-based)
     * @param config UART configuration
     * @return Result<void> indicating success or error
     */
    virtual util::Result<void> init(const UartConfig& config) = 0;
    
    /**
     * @brief Close UART
     */
    virtual void close() = 0;
    
    /**
     * @brief Check if UART is initialized
     */
    virtual bool isOpen() const = 0;
    
    /**
     * @brief Get number of bytes available to read
     */
    virtual size_t available() const = 0;
    
    /**
     * @brief Read single byte
     * @return Byte value, or -1 if no data available
     */
    virtual int read() = 0;
    
    /**
     * @brief Read multiple bytes
     * @param buffer Destination buffer span
     * @return Number of bytes actually read
     */
    virtual size_t read(std::span<uint8_t> buffer) = 0;
    
    /**
     * @brief Write single byte
     * @param byte Byte to write
     * @return Number of bytes written (1 or 0)
     */
    virtual size_t write(uint8_t byte) = 0;
    
    /**
     * @brief Write multiple bytes
     * @param data Source buffer span
     * @return Number of bytes actually written
     */
    virtual size_t write(std::span<const uint8_t> data) = 0;
    
    /**
     * @brief Flush TX buffer
     */
    virtual void flush() = 0;
    
    /**
     * @brief Clear RX buffer
     */
    virtual void clear() = 0;
    
    /**
     * @brief Check for overflow condition
     */
    virtual bool overflow() const = 0;
    
    /**
        * @brief Set RX callback (called from platform RX context)
     * @param callback Callback function
     */
    using RxCallback = void (*)(void* context);
    virtual void setRxCallback(RxCallback callback, void* context) = 0;
    
protected:
    UartInterface() = default;
};

} // namespace platform
} // namespace knx
