// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file manchester_codec.hpp
 * @brief KNX TP1 bit timing constants
 * 
 * Used to configure the bit-bang driver, which does its own bit encoding.
 * 
 * Reference: KNX System Specification v2.1, Chapter 3 (Physical Layer TP1)
 */

#pragma once

#include <cstdint>

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
};

} // namespace physical
} // namespace knx
