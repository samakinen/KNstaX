// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file manchester_codec.hpp
 * @brief Manchester encoding/decoding for KNX TP1
 * 
 * Implements differential Manchester encoding per KNX TP1 specification:
 * - Idle state: HIGH (recessive)
 * - Bit 0: Transition at start of bit cell
 * - Bit 1: No transition at start of bit cell
 * - Always transition at mid-bit time
 * 
 * Reference: KNX System Specification v2.1, Chapter 3 (Physical Layer TP1)
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <span>

#include "knx/util/result.hpp"

namespace knx {
namespace physical {

/**
 * @brief Manchester encoding for KNX TP1 differential Manchester variant
 * 
 * KNX TP1 uses 9600 Bd with differential Manchester encoding,
 * resulting in 19,200 baud bit rate.
 * 
 * Timing:
 * - Baud rate: 9600 Bd
 * - Bit rate: 19,200 baud (with Manchester)
 * - Bit cell time: 52.083 µs (1/19200)
 * - Half-bit time: 26.042 µs
 */
class ManchesterCodec {
public:
    // Timing constants for KNX TP1
    static constexpr uint32_t BAUD_RATE = 9600;        ///< Baud rate (symbols/sec)
    static constexpr uint32_t BIT_RATE = 19200;        ///< Bit rate with Manchester
    static constexpr uint32_t BIT_CELL_US = 52;        ///< Bit cell time (µs): 52.083
    static constexpr uint32_t HALF_BIT_US = 26;        ///< Half-bit time (µs): 26.042
    
    // Precise timing (nanoseconds for hardware timers)
    static constexpr uint32_t BIT_CELL_NS = 52083;     ///< Bit cell: 52.083 µs
    static constexpr uint32_t HALF_BIT_NS = 26042;     ///< Half-bit: 26.042 µs
    
    /**
     * @brief Encode a single byte to Manchester bit stream
     * 
     * Each byte (8 bits) becomes 16 half-bits in Manchester encoding.
     * 
     * @param byte Input byte to encode
     * @param output Output buffer (must have space for 16 half-bits)
     * @return Number of half-bits generated (always 16)
     */
    static size_t encodeByte(uint8_t byte, std::span<uint8_t> output);
    
    /**
     * @brief Encode multiple bytes to Manchester bit stream
     * 
     * @param data Input data buffer
     * @param output Output buffer (must have space for length * 16 half-bits)
     * @return Number of half-bits generated
     */
    static size_t encodeBytes(std::span<const uint8_t> data, std::span<uint8_t> output);
    
    /**
     * @brief Decode Manchester bit stream to a single byte
     * 
     * @param input Input buffer with sampled half-bits
     * @param output Decoded byte
    * @return Result<void> indicating success or error
     */
    static util::Result<void> decodeByte(std::span<const uint8_t> input, uint8_t& output);
    
    /**
     * @brief Decode Manchester bit stream to multiple bytes
     * 
     * @param input Input buffer with sampled half-bits
     * @param output Output buffer for decoded bytes
    * @return Result with actual decoded byte count on success or error on failure
     */
    static util::Result<size_t> decodeBytes(std::span<const uint8_t> input, std::span<uint8_t> output);
    
    /**
     * @brief Encode 8E1 UART frame with Manchester encoding
     * 
     * Encodes a complete UART frame:
     * - START bit (0)
     * - 8 DATA bits
     * - PARITY bit (even)
     * - STOP bit (1)
     * 
     * @param data Data byte
     * @param output Manchester encoded frame buffer (needs 11*2=22 half-bits minimum)
     * @return Number of half-bits generated
     */
    static size_t encodeFrame(uint8_t data, std::span<uint8_t> output);
    
    /**
     * @brief Calculate even parity bit
     * 
     * @param data Input byte
     * @return true if even number of 1-bits (parity bit = 0), false otherwise
     */
    static bool calculateParity(uint8_t data);
    
    /**
     * @brief Validate Manchester encoding
     * 
     * Checks if the bit stream follows valid Manchester encoding rules:
     * - Transitions occur at expected positions
     * - No invalid patterns
     * 
     * @param data Manchester encoded data
    * @return Result<void> indicating success or error
     */
    static util::Result<void> validate(std::span<const uint8_t> data);

private:
    /**
     * @brief Encode a single bit to Manchester half-bits
     * 
     * @param bit Bit value (0 or 1)
     * @param prevLevel Previous line level (for differential encoding)
     * @param output Output buffer (2 half-bits)
     * @return New line level after encoding
     */
    static uint8_t encodeBit(bool bit, uint8_t prevLevel, std::span<uint8_t> output);
};

} // namespace physical
} // namespace knx
