// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <cstdint>
#include <string>
#include <span>

#include "knx/util/result.hpp"

namespace knx {
namespace ets {

/**
 * @brief Format validation utilities for ETS configuration data
 * 
 * Provides checksum calculations and format validation according to KNX standards.
 * Implements CRC-16-CCITT and CRC-32 algorithms for data integrity verification.
 */
class FormatValidator {
public:
    /**
     * @brief CRC-16-CCITT calculation (polynomial 0x1021, initial 0xFFFF)
     * 
     * Used for header checksums in ETS configuration format.
     * 
     * @param data Pointer to data buffer
     * @param length Number of bytes to process
     * @return 16-bit CRC checksum
     */
    static uint16_t crc16_ccitt(std::span<const uint8_t> data);
    
    /**
     * @brief CRC-32 calculation (polynomial 0x04C11DB7)
     * 
     * Used for payload checksums in ETS configuration format.
     * 
     * @param data Pointer to data buffer
     * @param length Number of bytes to process
     * @return 32-bit CRC checksum
     */
    static uint32_t crc32(std::span<const uint8_t> data);
    
    /**
     * @brief Validate format magic number and basic structure
     * 
     * @param buffer Configuration data buffer
     * @param size Buffer size in bytes
    * @return Result<void> indicating success or error
     */
    static util::Result<void> isValidFormat(std::span<const uint8_t> buffer);
    
    /**
    * @brief Detect format version from buffer
     * 
     * @param buffer Configuration data buffer
     * @param size Buffer size in bytes
    * @return Format version number (0 = unknown/unsupported)
     */
    static uint8_t detectFormatVersion(std::span<const uint8_t> buffer);
    
    /**
     * @brief Validation result structure with detailed diagnostics
     */
    struct ValidationResult {
        bool valid;                     ///< Overall validation success
        std::string errorMessage;       ///< Human-readable error description
        uint32_t bytesProcessed;        ///< Number of bytes successfully processed
        uint8_t detectedVersion;        ///< Detected format version
        bool checksumValid;             ///< Checksum validation result
        
        ValidationResult() 
            : valid(false), bytesProcessed(0), detectedVersion(0), checksumValid(false) {}
    };
    
    /**
     * @brief Perform comprehensive validation with detailed reporting
     * 
     * @param buffer Configuration data buffer
     * @param size Buffer size in bytes
     * @return ValidationResult with detailed diagnostics
     */
    static ValidationResult validateFull(std::span<const uint8_t> buffer);

private:
    // Format constants
    static constexpr uint16_t FORMAT_MAGIC = 0xAEDD;
    static constexpr uint8_t CURRENT_VERSION = 1;
    static constexpr uint32_t MIN_HEADER_SIZE = 20;  // V1 header is 20 bytes
    
    // CRC lookup tables for performance
    static const uint16_t crc16_table[256];
    static const uint32_t crc32_table[256];
};

} // namespace ets
} // namespace knx
