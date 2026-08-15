// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_frame_codec.cpp
 * @brief Implementation of KNX TP1 frame encoding/decoding
 */

#include "knx/physical/tp1_frame_codec.hpp"
#include "knx/constants.hpp"

namespace knx {
namespace physical {
namespace {
constexpr uint8_t DEST_TYPE_MASK = knx::constants::protocol::DEST_ADDR_TYPE_MASK;
}

size_t Tp1FrameCodec::encode(const Tp1Frame& frame, std::span<uint8_t> output) {
    if ((!output.data() && !output.empty()) || output.size() < getFrameSize(frame.length)) {
        return 0;
    }
    
    // Validate frame before encoding
    if (frame.length > Tp1Frame::MAX_DATA_SIZE) {
        return 0;
    }
    
    size_t idx = 0;
    
    // Control field
    output[idx++] = frame.control;
    
    // Source address (big-endian)
    const uint16_t src = frame.source.raw;
    output[idx++] = static_cast<uint8_t>((src >> 8) & 0xFFu);
    output[idx++] = static_cast<uint8_t>(src & 0xFFu);
    
    // Destination address (big-endian)
    const uint16_t dest = frame.destination.raw;
    output[idx++] = static_cast<uint8_t>((dest >> 8) & 0xFFu);
    output[idx++] = static_cast<uint8_t>(dest & 0xFFu);
    
    // Length field
    uint8_t lengthField = frame.length;
    if (isGroupAddress(frame.destinationType)) {
        lengthField |= DEST_TYPE_MASK;
    }
    output[idx++] = lengthField;
    
    // Data payload
    for (size_t i = 0; i < frame.length; ++i) {
        output[idx++] = frame.data[i];
    }
    
    // Calculate and append checksum
    uint8_t checksum = calculateChecksum(output.first(idx));
    output[idx++] = checksum;
    
    return idx;
}

util::Result<void> Tp1FrameCodec::decode(std::span<const uint8_t> input, Tp1Frame& frame) {
    if (!input.data() && !input.empty()) {
        return util::ErrorCode::InvalidParameter;
    }

    if (input.size() < Tp1Frame::MIN_FRAME_SIZE) {
        return util::ErrorCode::InvalidFrameSize;
    }
    
    size_t idx = 0;
    
    // Control field
    frame.control = input[idx++];
    
    // Source address (big-endian)
    frame.source = IndividualAddress(static_cast<uint16_t>((input[idx] << 8) | input[idx + 1]));
    idx += 2;
    
    // Destination address (big-endian)
    uint16_t dest = static_cast<uint16_t>((input[idx] << 8) | input[idx + 1]);
    frame.destination = GroupAddress(dest);
    idx += 2;
    
    // Length field
    const uint8_t lengthField = input[idx++];
    frame.destinationType = (lengthField & DEST_TYPE_MASK) != 0
        ? AddressType::Group
        : AddressType::Individual;
    frame.length = static_cast<uint8_t>(lengthField & static_cast<uint8_t>(~DEST_TYPE_MASK));
    
    // Validate length
    if (frame.length > Tp1Frame::MAX_DATA_SIZE) {
        return util::ErrorCode::InvalidParameter;
    }
    
    // Check if we have enough data
    size_t expectedSize = getFrameSize(frame.length);
    if (input.size() < expectedSize) {
        return util::ErrorCode::InvalidFrameSize;
    }
    
    // Data payload
    for (size_t i = 0; i < frame.length; ++i) {
        frame.data[i] = input[idx++];
    }
    
    // Checksum
    frame.checksum = input[idx++];
    
    // Validate checksum
    uint8_t calculatedChecksum = calculateChecksum(input.first(idx - 1));
    if (calculatedChecksum != frame.checksum) {
        return util::ErrorCode::ChecksumError;
    }
    
    return util::Result<void>::ok();
}

uint8_t Tp1FrameCodec::calculateChecksum(const Tp1Frame& frame) {
    uint8_t checksum = 0xFF;
    
    checksum ^= frame.control;
    const uint16_t src = frame.source.raw;
    checksum ^= static_cast<uint8_t>((src >> 8) & 0xFFu);
    checksum ^= static_cast<uint8_t>(src & 0xFFu);
    const uint16_t dest = frame.destination.raw;
    checksum ^= static_cast<uint8_t>((dest >> 8) & 0xFFu);
    checksum ^= static_cast<uint8_t>(dest & 0xFFu);
    uint8_t lengthField = frame.length;
    if (isGroupAddress(frame.destinationType)) {
        lengthField |= DEST_TYPE_MASK;
    }
    checksum ^= lengthField;
    
    for (size_t i = 0; i < frame.length; ++i) {
        checksum ^= frame.data[i];
    }
    
    return checksum;
}

uint8_t Tp1FrameCodec::calculateChecksum(std::span<const uint8_t> data) {
    if (!data.data() && !data.empty()) {
        return 0xFF;
    }

    if (data.empty()) {
        return 0xFF;
    }
    
    uint8_t checksum = 0xFF;
    for (uint8_t byte : data) {
        checksum ^= byte;
    }
    
    return checksum;
}

util::Result<void> Tp1FrameCodec::validate(const Tp1Frame& frame) {
    // Check length field
    if (frame.length > Tp1Frame::MAX_DATA_SIZE) {
        return util::ErrorCode::InvalidParameter;
    }
    
    // Verify checksum
    uint8_t calculatedChecksum = calculateChecksum(frame);
    if (calculatedChecksum != frame.checksum) {
        return util::ErrorCode::ChecksumError;
    }

    return util::Result<void>::ok();
}

util::Result<void> Tp1FrameCodec::validate(std::span<const uint8_t> data) {
    if (!data.data() && !data.empty()) {
        return util::ErrorCode::InvalidParameter;
    }

    if (data.size() < Tp1Frame::MIN_FRAME_SIZE) {
        return util::ErrorCode::InvalidFrameSize;
    }
    
    // Extract and validate length field
    uint8_t dataLength = extractLength(data);
    if (dataLength > Tp1Frame::MAX_DATA_SIZE) {
        return util::ErrorCode::InvalidParameter;
    }
    
    // Check if frame size matches
    size_t expectedSize = getFrameSize(dataLength);
    if (data.size() != expectedSize) {
        return util::ErrorCode::InvalidFrameSize;
    }
    
    // Verify checksum
    uint8_t checksum = data.back();
    uint8_t calculatedChecksum = calculateChecksum(data.first(data.size() - 1));
    
    if (checksum != calculatedChecksum) {
        return util::ErrorCode::ChecksumError;
    }

    return util::Result<void>::ok();
}

size_t Tp1FrameCodec::getFrameSize(uint8_t length) {
    // MIN_FRAME_SIZE includes: control(1) + src(2) + dst(2) + len(1) + checksum(1) = 7
    // Add data length to get total
    return Tp1Frame::MIN_FRAME_SIZE + length;
}

uint8_t Tp1FrameCodec::extractLength(std::span<const uint8_t> data) {
    if ((!data.data() && !data.empty()) || data.size() < 6) {
        return 0;
    }
    
    // Length field is at offset 5 (after control + src + dst)
    return static_cast<uint8_t>(data[5] & static_cast<uint8_t>(~DEST_TYPE_MASK));
}

} // namespace physical
} // namespace knx
