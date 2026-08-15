// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_frame_codec.hpp
 * @brief KNX TP1 frame encoding/decoding
 * 
 * Implements KNX TP1 frame structure per specification:
 * - Control field
 * - Source address (16-bit)
 * - Destination address (16-bit)
 * - Length field
 * - Data payload (0-15 bytes)
 * - XOR checksum
 * 
 * Reference: KNX System Specification v2.1, Chapter 3 (TP1 Data Link Layer)
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <span>

#include "knx/types.hpp"
#include "knx/util/result.hpp"

namespace knx {
namespace physical {

/**
 * @brief KNX TP1 frame structure
 * 
 * Frame format (excluding START/STOP bits):
 * ┌─────────┬─────────┬─────────┬────────┬──────┬────────────┐
 * │ CONTROL │ SOURCE  │  DEST   │ LENGTH │ DATA │  CHECKSUM  │
 * │ 1 byte  │ 2 bytes │ 2 bytes │ 1 byte │ 0-15 │   1 byte   │
 * └─────────┴─────────┴─────────┴────────┴──────┴────────────┘
 */
struct Tp1Frame {
    uint8_t control;           ///< Control field
    IndividualAddress source;  ///< Source individual address
    GroupAddress destination;  ///< Destination address (raw per address type)
    AddressType destinationType; ///< Destination address type
    uint8_t length;            ///< Data length (actual bytes, 0-15)
    uint8_t data[16];          ///< Payload (max 15 bytes + header byte)
    uint8_t checksum;          ///< XOR checksum
    
    static constexpr size_t MIN_FRAME_SIZE = 7;   ///< Control+Src+Dst+Len+Checksum
    static constexpr size_t MAX_FRAME_SIZE = 23;  ///< MIN + 16 data bytes
    static constexpr size_t MAX_DATA_SIZE = 16;   ///< Max data field size
    
    /**
     * @brief Default constructor
     */
    Tp1Frame() 
        : control(0)
        , source()
        , destination()
        , destinationType(AddressType::Individual)
        , length(0)
        , data{0}
        , checksum(0)
    {}
};

/**
 * @brief KNX TP1 frame encoder/decoder
 * 
 * Handles conversion between Tp1Frame structures and byte streams
 * with proper checksum calculation and validation.
 */
class Tp1FrameCodec {
public:
    /**
     * @brief Encode KNX TP1 frame to byte stream
     * 
     * Encodes frame to byte array with proper field order and checksum.
     * 
     * @param frame Frame to encode
     * @param output Output buffer (must have space for MAX_FRAME_SIZE)
     * @return Number of bytes encoded (0 on error)
     */
    static size_t encode(const Tp1Frame& frame, std::span<uint8_t> output);
    
    /**
     * @brief Decode byte stream to KNX TP1 frame
     * 
     * Decodes byte array to frame structure, validating checksum.
     * 
     * @param input Input buffer
     * @param frame Output frame structure
     * @return true if valid frame decoded
     */
    static util::Result<void> decode(std::span<const uint8_t> input, Tp1Frame& frame);
    
    /**
     * @brief Calculate XOR checksum for frame
     * 
     * Checksum is XOR of all bytes except checksum field itself.
     * 
     * @param frame Frame to calculate checksum for
     * @return Calculated checksum byte
     */
    static uint8_t calculateChecksum(const Tp1Frame& frame);
    
    /**
     * @brief Calculate XOR checksum for byte stream
     * 
     * @param data Data buffer
     * @return Calculated checksum byte
     */
    static uint8_t calculateChecksum(std::span<const uint8_t> data);
    
    /**
     * @brief Validate frame structure
     * 
     * Checks:
     * - Length field is valid (0-15)
     * - Checksum is correct
     * - Frame size is consistent
     * 
     * @param frame Frame to validate
     * @return true if frame is valid
     */
    static util::Result<void> validate(const Tp1Frame& frame);
    
    /**
     * @brief Validate frame from byte stream
     * 
     * @param data Byte stream
     * @return true if valid frame
     */
    static util::Result<void> validate(std::span<const uint8_t> data);
    
    /**
     * @brief Get expected frame size from length field
     * 
     * @param length Data length field value
     * @return Expected total frame size in bytes
     */
    static size_t getFrameSize(uint8_t length);
    
    /**
     * @brief Extract length field from byte stream
     * 
     * @param data Byte stream (must have at least 6 bytes)
     * @return Data length value
     */
    static uint8_t extractLength(std::span<const uint8_t> data);
};

} // namespace physical
} // namespace knx
