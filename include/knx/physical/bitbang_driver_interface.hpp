// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bitbang_driver_interface.hpp
 * @brief Abstraction layer for KNX TP bit-bang drivers
 * 
 * This interface abstracts third-party bit-bang implementations,
 * allowing the stack to work with different drivers without
 * direct dependencies on their internal implementations.
 */

#pragma once

#include "knx/physical/link_state.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"
#include <span>
#include <cstdint>
#include <cstddef>

namespace knx {
namespace physical {

/**
 * @brief Configuration for bit-bang driver
 * 
 * KNX TP1 timing specifications:
 * - Baud rate: 9600 Bd (symbols per second)
 * - Bit rate: 19,200 baud (with Manchester encoding)
 * - Bit cell time: 52.083 µs (1/19200)
 * - Half-bit time: 26.042 µs (Manchester transition)
 */
struct BitBangConfig {
    uint8_t txPin;          ///< GPIO pin for TX
    uint8_t rxPin;          ///< GPIO pin for RX
    uint32_t baudRate;      ///< Baud rate (9600 Bd for KNX TP1)
    uint32_t bitRate;       ///< Actual bit rate with Manchester (19200 for TP1)
    bool enablePullup;      ///< Enable pull-up on RX pin
    bool txDominantHigh;    ///< Drive TX GPIO high for dominant bus level
    bool rxDominantHigh;    ///< Treat RX GPIO high as dominant bus level
    
    // Calculated timing constants (microseconds)
    uint32_t bitCellUs;     ///< Bit cell time in µs (52.083 for TP1)
    uint32_t halfBitUs;     ///< Half-bit time in µs (26.042 for TP1)
    uint32_t serialBitTimeUs;        ///< UART-style bit time in µs on the TP1 transceiver side
    uint32_t zeroActiveTimeUs;       ///< Dominant pulse width for transmitted logical zero
    uint32_t zeroEqualizationTimeUs; ///< Recessive equalization width after dominant pulse
    uint32_t bitSamplingOffsetUs;    ///< Receive sample point after detected edge
    uint32_t startBitValidationTimeUs; ///< Bus must stay dominant this long to confirm a start bit
    
    // High-precision timing (nanoseconds for hardware timers)
    uint32_t bitCellNs;     ///< Bit cell time in ns (52083 for TP1)
    uint32_t halfBitNs;     ///< Half-bit time in ns (26042 for TP1)

    /// Optional transceiver link-health input (STKNX KNX_OK and equivalents).
    /// Default-inert: unconfigured means the driver reports LinkState::Unknown
    /// and behaves exactly as it did before the signal existed.
    LinkSignalConfig linkSignal{};

    BitBangConfig()
        : txPin(0xFF)         // Invalid pin marker (no GPIO assigned)
        , rxPin(0xFF)         // Invalid pin marker (no GPIO assigned)
        , baudRate(9600)      // KNX TP1 baud rate
        , bitRate(19200)      // With Manchester encoding
        , enablePullup(true)
        , txDominantHigh(true)
        , rxDominantHigh(true)
        , bitCellUs(52)       // 52.083 µs
        , halfBitUs(26)       // 26.042 µs
        , serialBitTimeUs(104)
        , zeroActiveTimeUs(35)
        , zeroEqualizationTimeUs(69)
        , bitSamplingOffsetUs(52)  // mid-bit-cell; see bitbang_board_profile.hpp
        , startBitValidationTimeUs(15)
        , bitCellNs(52083)    // Precise timing
        , halfBitNs(26042)    // Precise timing
    {}
    
    /**
     * @brief Validate bit-bang configuration parameters
     * @param validateGpio Whether to validate GPIO pin assignments (false for platform-independent code)
     * @return Result<void> indicating success or specific error
     */
    util::Result<void> validate(bool validateGpio = true) const {
        // Validate GPIO pins only if requested (hardware-specific drivers)
        if (validateGpio) {
            // Skip validation if pins are not assigned (0xFF marker)
            if (txPin != 0xFF && rxPin != 0xFF) {
                // Validate GPIO pins (ESP32-C6 has GPIOs 0-30)
                // Platform-specific, but 0-63 is safe range for most MCUs
                if (txPin > 63) {
                    return util::Result<void>::err(util::ErrorCode::InvalidParameter);
                }
                if (rxPin > 63) {
                    return util::Result<void>::err(util::ErrorCode::InvalidParameter);
                }
                
                // TX and RX must be different pins
                if (txPin == rxPin) {
                    return util::Result<void>::err(util::ErrorCode::InvalidParameter);
                }
            }

            if (linkSignal.isConfigured()) {
                if (linkSignal.pin > 63) {
                    return util::Result<void>::err(util::ErrorCode::InvalidParameter);
                }
                if (linkSignal.pin == txPin || linkSignal.pin == rxPin) {
                    return util::Result<void>::err(util::ErrorCode::InvalidParameter);
                }
            }
        }

        // A zero down-debounce would let a single sag of the bus voltage take
        // the link Down; the pre-debounce power-fail hook exists for boards
        // that genuinely need an immediate reaction.
        if (linkSignal.isConfigured() && linkSignal.downDebounceUs == 0) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        
        // Validate baud rate (KNX TP1 uses 9600 Bd)
        if (baudRate != 9600) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        
        // Validate bit rate (must be 19200 for Manchester encoded TP1)
        if (bitRate != 19200) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        
        // Validate timing consistency
        uint32_t expectedBitCellUs = 1000000 / bitRate;
        if (bitCellUs < expectedBitCellUs - 1 || bitCellUs > expectedBitCellUs + 1) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        if (serialBitTimeUs == 0 || zeroActiveTimeUs == 0 || zeroEqualizationTimeUs == 0) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        if ((zeroActiveTimeUs + zeroEqualizationTimeUs) != serialBitTimeUs) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        if (bitSamplingOffsetUs == 0 || bitSamplingOffsetUs >= serialBitTimeUs) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }

        if (startBitValidationTimeUs == 0
            || startBitValidationTimeUs >= zeroActiveTimeUs
            || startBitValidationTimeUs >= bitSamplingOffsetUs) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        
        return util::Result<void>::ok();
    }
};

/**
 * @brief Driver state
 */
enum class DriverState : uint8_t {
    Uninitialized,
    Idle,
    Receiving,
    Transmitting,
    Error
};

/**
 * @brief Callback for received frames
 * @param frame Frame data buffer valid for the duration of the callback
 * @param context User context pointer
 */
using RxCallback = util::InplaceFunction<void(std::span<const uint8_t> frame, void* context), 64>;
using QueueNotifyCallback = void(*)(void* context);

/**
 * @brief Abstract interface for TP1 bit-bang drivers
 * 
 * This interface must be implemented by any bit-bang driver
 * to be compatible with the KNX stack.
 */
class BitBangDriverInterface {
public:
    virtual ~BitBangDriverInterface() = default;
    
    // Delete copy and move operations (abstract base class)
    BitBangDriverInterface(const BitBangDriverInterface&) = delete;
    BitBangDriverInterface& operator=(const BitBangDriverInterface&) = delete;
    BitBangDriverInterface(BitBangDriverInterface&&) = delete;
    BitBangDriverInterface& operator=(BitBangDriverInterface&&) = delete;
    
    /**
     * @brief Initialize the driver
     * @param config Driver configuration
     * @return Result<void> indicating success or error
     */
    virtual util::Result<void> init(const BitBangConfig& config) = 0;
    
    /**
     * @brief Close and cleanup the driver
     */
    virtual void close() = 0;
    
    /**
     * @brief Send a frame
     * @param frame Frame data to send
     * @return Result<void> indicating success or error
     */
    virtual util::Result<void> send(std::span<const uint8_t> frame) = 0;
    
    /**
     * @brief Set receive callback
     * @param callback Function to call when frame is received
     * @param context User context passed to callback
     */
    virtual void setRxCallback(RxCallback callback, void* context) = 0;

    /**
     * @brief Set queue notify callback for ISR-enqueued messages
     * @param callback ISR-safe callback invoked when new driver messages are queued
     * @param context User context passed to callback
     */
    virtual void setQueueNotifyCallback(QueueNotifyCallback callback, void* context) = 0;
    
    /**
     * @brief Get current driver state
     */
    virtual DriverState getState() const = 0;
    
    /**
     * @brief Reset the driver to idle state
     */
    virtual void reset() = 0;
    
    /**
     * @brief Check if medium is idle (no dominant signal)
     * 
     * Required for CSMA/CA (Carrier Sense Multiple Access with Collision Avoidance)
     * before transmission. KNX requires minimum idle time of 50 bit cells.
     * 
     * @return true if medium is idle (HIGH/recessive)
     */
    virtual bool isMediumIdle() const = 0;
    
    /**
     * @brief Check for collision during transmission
     * 
     * Detects collision by comparing transmitted bit with received bit.
     * If they differ, a collision occurred (another device is transmitting).
     * 
     * @return true if collision detected
     */
    virtual bool isCollisionDetected() const = 0;
    
    /**
     * @brief Abort current transmission
     * 
     * Called when collision is detected to immediately stop transmission.
     */
    virtual void abortTransmission() = 0;
    
    /**
     * @brief Get minimum idle time before transmission (microseconds)
     * 
     * KNX TP1 requires 50 bit times idle before transmission:
     * 50 * 52.083 µs = 2604.15 µs
     */
    static constexpr uint32_t MIN_IDLE_TIME_US = 2604;

protected:
    // Allow derived classes to construct
    BitBangDriverInterface() = default;
    
    /**
     * @brief Process driver tasks (call periodically from main loop)
     * 
     * Some drivers may need periodic processing for timing or
     * state management. Call this from the main application loop.
     */
    virtual void process() = 0;
    
    /**
     * @brief Get driver statistics (optional)
     */
    struct Statistics {
        uint32_t txFrames{0};
        uint32_t rxFrames{0};
        uint32_t txErrors{0};
        uint32_t rxErrors{0};
        uint32_t collisions{0};
    };
    
    virtual Statistics getStatistics() const {
        return Statistics{};
    }
    
    /**
     * @brief Get driver version string
     */
    virtual const char* getVersion() const = 0;
};

/**
 * @brief Factory function type for creating driver instances
 */
using BitBangDriverFactory = BitBangDriverInterface* (*)();

} // namespace physical
} // namespace knx
