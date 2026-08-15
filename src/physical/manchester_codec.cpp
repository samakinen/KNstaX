// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file manchester_codec.cpp
 * @brief Implementation of Manchester encoding/decoding for KNX TP1
 */

#include "knx/physical/manchester_codec.hpp"

namespace knx {
namespace physical {

size_t ManchesterCodec::encodeByte(uint8_t byte, std::span<uint8_t> output) {
    if ((!output.data() && !output.empty()) || output.size() < 16) {
        return 0;
    }
    
    uint8_t prevLevel = 1;  // Start with HIGH (idle state)
    size_t outIdx = 0;
    
    // Encode each bit (MSB first)
    for (int i = 7; i >= 0; --i) {
        bool bit = (byte >> i) & 1;
        prevLevel = encodeBit(bit, prevLevel, output.subspan(outIdx, 2));
        outIdx += 2;
    }
    
    return outIdx;  // 16 half-bits for 8 bits
}

size_t ManchesterCodec::encodeBytes(std::span<const uint8_t> data, std::span<uint8_t> output) {
    if ((!data.data() && !data.empty()) || (!output.data() && !output.empty()) || data.empty()) {
        return 0;
    }
    if (output.size() < (data.size() * 16)) {
        return 0;
    }
    
    size_t totalHalfBits = 0;
    for (uint8_t byte : data) {
        totalHalfBits += encodeByte(byte, output.subspan(totalHalfBits));
    }
    
    return totalHalfBits;
}

uint8_t ManchesterCodec::encodeBit(bool bit, uint8_t prevLevel, std::span<uint8_t> output) {
    // Differential Manchester encoding:
    // - Bit 0: transition at start of bit cell
    // - Bit 1: no transition at start of bit cell
    // - Always transition at mid-bit time
    
    uint8_t firstHalf, secondHalf;
    
    if (!bit) {
        // Bit 0: transition at start
        firstHalf = !prevLevel;
        secondHalf = !firstHalf;
    } else {
        // Bit 1: no transition at start, transition at mid
        firstHalf = prevLevel;
        secondHalf = !firstHalf;
    }
    
    if (output.size() >= 2) {
        output[0] = firstHalf;
        output[1] = secondHalf;
    }
    
    return secondHalf;  // New level for next bit
}

util::Result<void> ManchesterCodec::decodeByte(std::span<const uint8_t> input, uint8_t& output) {
    if (!input.data() && !input.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    if (input.size() < 16) {
        return util::ErrorCode::InvalidFrameSize;  // Need at least 16 half-bits for 8 bits
    }
    
    output = 0;
    uint8_t prevLevel = 1;  // Assume idle HIGH before
    
    for (size_t i = 0; i < 8; ++i) {
        size_t idx = i * 2;
        
        uint8_t firstHalf = input[idx];
        uint8_t secondHalf = input[idx + 1];
        
        // Check for valid transition at mid-bit (always required)
        if (firstHalf == secondHalf) {
            return util::ErrorCode::ChecksumError;  // Invalid Manchester: no mid-bit transition
        }
        
        // Decode bit based on start-of-bit transition
        bool bit;
        if (firstHalf != prevLevel) {
            // Transition at start = bit 0
            bit = false;
        } else {
            // No transition at start = bit 1
            bit = true;
        }
        
        output |= static_cast<uint8_t>((bit ? 1u : 0u) << (7 - i));
        prevLevel = secondHalf;
    }
    
    return util::Result<void>::ok();
}

util::Result<size_t> ManchesterCodec::decodeBytes(std::span<const uint8_t> input, std::span<uint8_t> output) {
    if ((!input.data() && !input.empty()) || (!output.data() && !output.empty())) {
        return util::ErrorCode::InvalidParameter;
    }
    if (output.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    if (input.size() < 16) {
        return util::ErrorCode::InvalidFrameSize;
    }
    
    size_t maxBytes = input.size() / 16;  // Each byte needs 16 half-bits
    size_t decodedBytes = 0;
    
    for (size_t i = 0; i < maxBytes && decodedBytes < output.size(); ++i) {
        uint8_t byte;
        auto result = decodeByte(input.subspan(i * 16, 16), byte);
        if (result.isError()) {
            return result.error();
        }
        output[decodedBytes++] = byte;
    }
    
    if (decodedBytes == 0) {
        return util::ErrorCode::InvalidFrameSize;
    }
    return decodedBytes;
}

size_t ManchesterCodec::encodeFrame(uint8_t data, std::span<uint8_t> output) {
    if ((!output.data() && !output.empty()) || output.size() < 22) {
        return 0;
    }
    
    size_t idx = 0;
    uint8_t prevLevel = 1;  // Idle HIGH
    
    // START bit (0)
    prevLevel = encodeBit(false, prevLevel, output.subspan(idx, 2));
    idx += 2;
    
    // DATA bits (8 bits, MSB first)
    for (int i = 7; i >= 0; --i) {
        bool bit = (data >> i) & 1;
        prevLevel = encodeBit(bit, prevLevel, output.subspan(idx, 2));
        idx += 2;
    }
    
    // PARITY bit (even)
    bool parity = calculateParity(data);
    prevLevel = encodeBit(parity, prevLevel, output.subspan(idx, 2));
    idx += 2;
    
    // STOP bit (1) - return to idle HIGH
    prevLevel = encodeBit(true, prevLevel, output.subspan(idx, 2));
    idx += 2;
    
    return idx;  // Total: 11 bits * 2 = 22 half-bits
}

bool ManchesterCodec::calculateParity(uint8_t data) {
    uint8_t count = 0;
    for (int i = 0; i < 8; ++i) {
        count += (data >> i) & 1;
    }
    return (count & 1) == 0;  // Even parity: true if even number of 1s
}

util::Result<void> ManchesterCodec::validate(std::span<const uint8_t> data) {
    if (!data.data() && !data.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    if (data.size() < 2) {
        return util::ErrorCode::InvalidFrameSize;
    }
    
    // Check that transitions occur at mid-bit positions
    // In Manchester, every bit cell must have at least one transition
    for (size_t i = 0; i < data.size() - 1; i += 2) {
        if (i + 1 >= data.size()) {
            return util::ErrorCode::InvalidFrameSize;  // Incomplete bit cell
        }
        
        // Each bit cell (2 half-bits) must have a transition at mid-bit
        if (data[i] == data[i + 1]) {
            return util::ErrorCode::ChecksumError;  // No mid-bit transition
        }
    }

    return util::Result<void>::ok();
}

} // namespace physical
} // namespace knx
