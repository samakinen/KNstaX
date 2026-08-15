// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bit_ops.hpp
 * @brief Bit manipulation utilities for KNX protocol handling
 * 
 * Provides type-safe, constexpr bit manipulation functions to replace
 * scattered bit operations throughout the codebase. All functions are
 * inline and constexpr for zero-overhead abstraction.
 */

#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace knx {
namespace util {

template <typename T>
concept BitIntegral = std::integral<T>;

// ============================================================================
// Bit Extraction Functions
// ============================================================================

/**
 * @brief Extract a byte from a multi-byte value at specified position
 * 
 * @tparam T Integer type (uint16_t, uint32_t, etc.)
 * @param value Source value
 * @param byteIndex Byte position (0 = LSB)
 * @return Extracted byte
 * 
 * @example
 * uint16_t val = 0xABCD;
 * getByte(val, 0) == 0xCD  // LSB
 * getByte(val, 1) == 0xAB  // MSB
 */
template<BitIntegral T>
constexpr uint8_t getByte(T value, uint8_t byteIndex) {
    return static_cast<uint8_t>((value >> (byteIndex * 8)) & 0xFF);
}

/**
 * @brief Extract high byte from 16-bit value
 */
constexpr uint8_t getHighByte(uint16_t value) {
    return static_cast<uint8_t>((value >> 8) & 0xFF);
}

/**
 * @brief Extract low byte from 16-bit value
 */
constexpr uint8_t getLowByte(uint16_t value) {
    return static_cast<uint8_t>(value & 0xFF);
}

/**
 * @brief Extract bits from a value using a mask
 * 
 * @tparam T Integer type
 * @param value Source value
 * @param mask Bit mask defining which bits to extract
 * @return Extracted bits (not shifted)
 * 
 * @example
 * uint8_t val = 0b11010110;
 * extractBits(val, 0xF0) == 0xD0  // Upper nibble
 * extractBits(val, 0x0F) == 0x06  // Lower nibble
 */
template<BitIntegral T>
constexpr T extractBits(T value, T mask) {
    return value & mask;
}

/**
 * @brief Extract and shift bits to position 0
 * 
 * Automatically calculates the shift amount based on the mask's position.
 * 
 * @tparam T Integer type
 * @param value Source value
 * @param mask Bit mask
 * @return Extracted bits shifted to LSB position
 * 
 * @example
 * uint8_t val = 0b11010110;
 * extractAndShift(val, 0xF0) == 0x0D  // Upper nibble shifted down
 * extractAndShift(val, 0x0E) == 0x03  // Bits 1-3 shifted down
 */
template<BitIntegral T>
constexpr T extractAndShift(T value, T mask) {
    if (mask == 0) return 0;
    
    // Find position of first set bit (trailing zeros)
    T shiftAmount = 0;
    T tempMask = mask;
    while ((tempMask & 1) == 0) {
        tempMask >>= 1;
        ++shiftAmount;
    }
    
    return (value & mask) >> shiftAmount;
}

// ============================================================================
// Bit Setting Functions
// ============================================================================

/**
 * @brief Set specific bits in a value
 * 
 * @tparam T Integer type
 * @param value Source value
 * @param bits Bits to set
 * @param mask Mask indicating which bits to modify
 * @return Modified value
 * 
 * @example
 * uint8_t val = 0b00000000;
 * setBits(val, 0xF0, 0xF0) == 0xF0  // Set upper nibble
 * setBits(val, 0x0D, 0x0F) == 0x0D  // Set lower nibble
 */
template<BitIntegral T>
constexpr T setBits(T value, T bits, T mask) {
    return (value & ~mask) | (bits & mask);
}

/**
 * @brief Set bits by shifting value to mask position
 * 
 * Automatically shifts the bits value to align with the mask.
 * 
 * @tparam T Integer type
 * @param value Current value
 * @param bits Bits to insert (at LSB position)
 * @param mask Mask indicating where to insert
 * @return Modified value
 * 
 * @example
 * uint8_t val = 0b00000000;
 * setBitsShifted(val, 0x0D, 0xF0) == 0xD0  // 0x0D shifted to upper nibble
 * setBitsShifted(val, 0x03, 0x0E) == 0x06  // 0x03 shifted to bits 1-3
 */
template<BitIntegral T>
constexpr T setBitsShifted(T value, T bits, T mask) {
    if (mask == 0) return value;
    
    // Find position of first set bit
    T shiftAmount = 0;
    T tempMask = mask;
    while ((tempMask & 1) == 0) {
        tempMask >>= 1;
        ++shiftAmount;
    }
    
    return (value & ~mask) | ((bits << shiftAmount) & mask);
}

/**
 * @brief Compose a 16-bit value from two bytes
 * 
 * @param highByte Most significant byte
 * @param lowByte Least significant byte
 * @return Composed 16-bit value
 */
constexpr uint16_t makeWord(uint8_t highByte, uint8_t lowByte) {
    return (static_cast<uint16_t>(highByte) << 8) | lowByte;
}

/**
 * @brief Compose a 32-bit value from four bytes
 * 
 * @param byte3 Most significant byte
 * @param byte2 Second byte
 * @param byte1 Third byte
 * @param byte0 Least significant byte
 * @return Composed 32-bit value
 */
constexpr uint32_t makeDword(uint8_t byte3, uint8_t byte2, uint8_t byte1, uint8_t byte0) {
    return (static_cast<uint32_t>(byte3) << 24) |
           (static_cast<uint32_t>(byte2) << 16) |
           (static_cast<uint32_t>(byte1) << 8) |
           static_cast<uint32_t>(byte0);
}

// ============================================================================
// Bit Testing Functions
// ============================================================================

/**
 * @brief Check if specific bits are set
 * 
 * @tparam T Integer type
 * @param value Value to test
 * @param mask Bit mask
 * @return true if all masked bits are set
 */
template<BitIntegral T>
constexpr bool areBitsSet(T value, T mask) {
    return (value & mask) == mask;
}

/**
 * @brief Check if any of the masked bits are set
 * 
 * @tparam T Integer type
 * @param value Value to test
 * @param mask Bit mask
 * @return true if any masked bits are set
 */
template<BitIntegral T>
constexpr bool anyBitsSet(T value, T mask) {
    return (value & mask) != 0;
}

/**
 * @brief Check if all masked bits are clear
 * 
 * @tparam T Integer type
 * @param value Value to test
 * @param mask Bit mask
 * @return true if all masked bits are clear
 */
template<BitIntegral T>
constexpr bool areBitsClear(T value, T mask) {
    return (value & mask) == 0;
}

// ============================================================================
// Bit Counting Functions
// ============================================================================

/**
 * @brief Count number of set bits (population count)
 * 
 * @tparam T Integer type
 * @param value Value to count
 * @return Number of set bits
 */
template<BitIntegral T>
constexpr int countSetBits(T value) {
    int count = 0;
    while (value) {
        count += (value & 1);
        value >>= 1;
    }
    return count;
}

/**
 * @brief Find position of first set bit (0-indexed)
 * 
 * @tparam T Integer type
 * @param value Value to search
 * @return Position of first set bit, or -1 if no bits are set
 */
template<BitIntegral T>
constexpr int findFirstSet(T value) {
    if (value == 0) return -1;
    
    int pos = 0;
    while ((value & 1) == 0) {
        value >>= 1;
        ++pos;
    }
    return pos;
}

// ============================================================================
// Nibble Operations
// ============================================================================

/**
 * @brief Extract high nibble (upper 4 bits) from byte
 */
constexpr uint8_t getHighNibble(uint8_t value) {
    return (value >> 4) & 0x0F;
}

/**
 * @brief Extract low nibble (lower 4 bits) from byte
 */
constexpr uint8_t getLowNibble(uint8_t value) {
    return value & 0x0F;
}

/**
 * @brief Compose byte from two nibbles
 * 
 * @param high High nibble (4 bits)
 * @param low Low nibble (4 bits)
 * @return Composed byte
 */
constexpr uint8_t makeNibbles(uint8_t high, uint8_t low) {
    return ((high & 0x0F) << 4) | (low & 0x0F);
}

/**
 * @brief Swap nibbles in a byte
 * 
 * @param value Byte to swap
 * @return Byte with swapped nibbles
 */
constexpr uint8_t swapNibbles(uint8_t value) {
    return ((value & 0x0F) << 4) | ((value >> 4) & 0x0F);
}

// ============================================================================
// Endianness Utilities
// ============================================================================

/**
 * @brief Swap byte order of 16-bit value
 */
constexpr uint16_t swapBytes16(uint16_t value) {
    return ((value & 0xFF) << 8) | ((value >> 8) & 0xFF);
}

/**
 * @brief Swap byte order of 32-bit value
 */
constexpr uint32_t swapBytes32(uint32_t value) {
    return ((value & 0xFF) << 24) |
           ((value & 0xFF00) << 8) |
           ((value & 0xFF0000) >> 8) |
           ((value >> 24) & 0xFF);
}

// ============================================================================
// Byte Encoding Helpers
// ============================================================================

constexpr void storeWordBE(std::span<uint8_t, 2> out, uint16_t value) noexcept {
    out[0] = getHighByte(value);
    out[1] = getLowByte(value);
}

constexpr void storeDwordBE(std::span<uint8_t, 4> out, uint32_t value) noexcept {
    out[0] = getByte(value, 3);
    out[1] = getByte(value, 2);
    out[2] = getByte(value, 1);
    out[3] = getByte(value, 0);
}

[[nodiscard]] constexpr std::array<uint8_t, 2> encodeWord(uint16_t value) noexcept {
    std::array<uint8_t, 2> out{};
    storeWordBE(std::span<uint8_t, 2>(out), value);
    return out;
}

[[nodiscard]] constexpr std::array<uint8_t, 4> encodeDword(uint32_t value) noexcept {
    std::array<uint8_t, 4> out{};
    storeDwordBE(std::span<uint8_t, 4>(out), value);
    return out;
}

/**
 * @brief Encode 16-bit value to vector (big-endian)
 * Replaces: data.clear(); data.push_back(getHighByte(v)); data.push_back(getLowByte(v));
 */
inline void encodeWord(std::vector<uint8_t>& data, uint16_t value) {
    data.resize(2);
    storeWordBE(std::span<uint8_t, 2>(data.data(), 2), value);
}

/**
 * @brief Encode 32-bit value to vector (big-endian)
 * Replaces: data.clear(); data.push_back(getByte(v,3)); ... data.push_back(getByte(v,0));
 */
inline void encodeDword(std::vector<uint8_t>& data, uint32_t value) {
    data.resize(4);
    storeDwordBE(std::span<uint8_t, 4>(data.data(), 4), value);
}

/**
 * @brief Append 16-bit value to vector (big-endian)
 */
inline void appendWord(std::vector<uint8_t>& data, uint16_t value) {
    const auto encoded = encodeWord(value);
    data.insert(data.end(), encoded.begin(), encoded.end());
}

/**
 * @brief Append 32-bit value to vector (big-endian)
 */
inline void appendDword(std::vector<uint8_t>& data, uint32_t value) {
    const auto encoded = encodeDword(value);
    data.insert(data.end(), encoded.begin(), encoded.end());
}

} // namespace util
} // namespace knx
