// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file frame_codec.cpp
 * @brief Shared TP1 frame encoding/decoding implementation
 */

#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/util/bit_ops.hpp"
#include "knx/util/hex.hpp"
#include "knx/util/log.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/constants.hpp"
#include <cstring>
#include <span>

static const char* TAG = "KNX.FrameCodec";

using namespace knx::constants::protocol;
namespace bits = knx::util;

namespace knx {
namespace datalink {

uint8_t FrameCodec::calculateChecksum(std::span<const uint8_t> data) {
    if (!data.data() && !data.empty()) {
        return 0xFF;
    }

    uint8_t checksum = 0xFF;
    for (uint8_t byte : data) {
        checksum ^= byte;
    }
    return checksum;
}

util::Result<void> FrameCodec::verifyChecksum(std::span<const uint8_t> data) {
    if (!data.data() && !data.empty()) {
        return util::ErrorCode::InvalidParameter;
    }

    if (data.size() < 1) return util::ErrorCode::InvalidFrameSize;

    uint8_t checksum = calculateChecksum(data.first(data.size() - 1));
    if (checksum != data.back()) {
        return util::ErrorCode::ChecksumError;
    }
    return util::Result<void>::ok();
}

util::Result<size_t> FrameCodec::encodeFrame(const LDataFrame& frame, std::span<uint8_t> buffer) {
    if (!buffer.data() && !buffer.empty()) {
        return util::ErrorCode::InvalidParameter;
    }

    // Validate frame before encoding
    if (!frame.isValid()) {
        KNX_LOGW(TAG, "Frame validation failed before encoding");
        return util::ErrorCode::InvalidParameter;
    }

    const size_t tpduLength = frame.tpdu.size();
    if (tpduLength == 0 || tpduLength > 256) {
        KNX_LOGW(TAG, "TPDU length out of range: %zu", tpduLength);
        return util::ErrorCode::InvalidFrameSize;
    }
    // Standard frames carry up to 16 TPDU octets (4-bit length field); longer
    // TPDUs are sent as L_Data_Extended with an 8-bit length field.
    const bool extended = tpduLength > 16;
    const size_t requiredSize = (extended ? 8u : 7u) + tpduLength;
    if (buffer.size() < requiredSize) return util::ErrorCode::BufferTooSmall;

    size_t idx = 0;

    // Control field. Bit 7 selects the frame format (1 = standard, 0 = extended);
    // bit 4 is always 1 in L_DATA frames per KNX 03_02_02.
    uint8_t ctrl = 0;
    if (!extended) ctrl |= CTRL_FRAME_TYPE_MASK;
    // Wire r-bit is inverted: 1 = NOT repeated (03_02_02 §2.2.2).
    if (!frame.repeated) ctrl |= CTRL_REPEAT_MASK;
    ctrl |= CTRL_BROADCAST_MASK;
    ctrl |= (static_cast<uint8_t>(frame.priority) << 2) & CTRL_PRIORITY_MASK;
    // KNOWN DEVIATION: 03_02_02 defines ctrl bits 1..0 as fixed 0 on the TP1
    // wire; this codec carries the cEMI-style ack-request/confirm flags there
    // because Tp1MacPhysical derives its "await L_ACK" decision from bit 1 of
    // the encoded frame. Certified receivers ignore these bits (verified in
    // full ETS6 commissioning on a real bus). Removing them requires plumbing
    // the ack expectation out-of-band from the DL to the MAC.
    if (frame.ackRequested) ctrl |= CTRL_ACK_MASK;
    if (frame.confirmation) ctrl |= CTRL_CONFIRM_MASK;
    buffer[idx++] = ctrl;

    if (extended) {
        // CTRLE: address type (bit 7), hop count (bits 6..4), format 0000.
        uint8_t ctrle = static_cast<uint8_t>((frame.hopCount & 0x07) << 4);
        if (isGroupAddress(frame.destinationType)) {
            ctrle |= 0x80;
        }
        buffer[idx++] = ctrle;
    }

    // Source address (16 bits)
    const uint16_t src = frame.source.raw;
    buffer[idx++] = bits::getHighByte(src);
    buffer[idx++] = bits::getLowByte(src);

    // Destination address (16 bits)
    const uint16_t dest = frame.destination.raw;
    buffer[idx++] = bits::getHighByte(dest);
    buffer[idx++] = bits::getLowByte(dest);

    if (extended) {
        buffer[idx++] = static_cast<uint8_t>(tpduLength - 1u);
    } else {
        const uint8_t dataLength = static_cast<uint8_t>(tpduLength - 1u);
        uint8_t lengthField = static_cast<uint8_t>((frame.hopCount << 4) | (dataLength & 0x0F));
        if (isGroupAddress(frame.destinationType)) {
            lengthField |= DEST_ADDR_TYPE_MASK;
        }
        buffer[idx++] = lengthField;
    }
    
    // TPDU bytes
    for (size_t i = 0; i < frame.tpdu.size(); i++) {
        buffer[idx++] = frame.tpdu[i];
    }
    
    // Checksum (XOR of all bytes)
    uint8_t checksum = calculateChecksum(buffer.first(idx));
    buffer[idx++] = checksum;
    
    return idx;  // Return encoded length
}

util::Result<void> FrameCodec::decodeFrame(std::span<const uint8_t> buffer, LDataFrame& frame) {
        if (!buffer.data() && !buffer.empty()) {
            return util::ErrorCode::InvalidParameter;
        }

    if (buffer.size() < 7) return util::ErrorCode::InvalidFrameSize;

    auto checksumResult = verifyChecksum(buffer);
    if (checksumResult.isError()) {
        const uint8_t expectedChecksum = calculateChecksum(buffer.first(buffer.size() - 1));
        KNX_LOGD(TAG,
                 "Checksum mismatch len=%zu expected=0x%02X actual=0x%02X frame=%s",
                 buffer.size(),
                 expectedChecksum,
                 buffer.back(),
                 util::formatHexBytes(buffer).c_str());
        KNX_LOGW(TAG, "Checksum mismatch");
        return checksumResult.error();
    }

    size_t idx = 0;

    uint8_t ctrl = buffer[idx++];

    if ((ctrl & 0x40) != 0) {
        KNX_LOGW(TAG, "Reserved control bit set");
        return util::ErrorCode::InvalidFrameSize;
    }

    frame.standardFrame = (ctrl & CTRL_FRAME_TYPE_MASK) != 0;
    frame.repeated = (ctrl & CTRL_REPEAT_MASK) == 0;  // wire r-bit: 1 = not repeated
    frame.priority = static_cast<Priority>((ctrl & CTRL_PRIORITY_MASK) >> 2);
    frame.ackRequested = (ctrl & CTRL_ACK_MASK) != 0;
    frame.confirmation = (ctrl & CTRL_CONFIRM_MASK) != 0;

    size_t tpduLength = 0;
    if (!frame.standardFrame) {
        // L_Data_Extended (KNX 03.02.02): CTRL | CTRLE | SA(2) | DA(2) |
        // LEN(8-bit APDU length) | TPDU | FCS. CTRLE carries the address type
        // (bit 7), hop count (bits 6..4) and extended frame format (bits 3..0,
        // only 0000 = point-to-point/standard group is supported).
        if (buffer.size() < 9) {
            return util::ErrorCode::InvalidFrameSize;
        }
        const uint8_t ctrle = buffer[idx++];
        if ((ctrle & 0x0F) != 0) {
            KNX_LOGW(TAG, "Unsupported extended frame format 0x%02X", ctrle & 0x0F);
            return util::ErrorCode::InvalidFrameSize;
        }
        frame.destinationType = (ctrle & 0x80) != 0 ? AddressType::Group
                                                    : AddressType::Individual;
        frame.hopCount = (ctrle >> 4) & 0x07;

        frame.source = IndividualAddress(static_cast<uint16_t>((buffer[idx] << 8) | buffer[idx + 1]));
        idx += 2;
        frame.destination = GroupAddress(static_cast<uint16_t>((buffer[idx] << 8) | buffer[idx + 1]));
        idx += 2;

        tpduLength = static_cast<size_t>(buffer[idx++]) + 1u;
    } else {
        frame.source = IndividualAddress(static_cast<uint16_t>((buffer[idx] << 8) | buffer[idx + 1]));
        idx += 2;
        frame.destination = GroupAddress(static_cast<uint16_t>((buffer[idx] << 8) | buffer[idx + 1]));
        idx += 2;

        const uint8_t lengthField = buffer[idx++];
        frame.destinationType = (lengthField & DEST_ADDR_TYPE_MASK) != 0
            ? AddressType::Group
            : AddressType::Individual;
        frame.hopCount = (lengthField >> 4) & 0x07;
        tpduLength = static_cast<size_t>(lengthField & 0x0F) + 1u;
    }

    if (buffer.size() != (idx + tpduLength + 1u)) {
        KNX_LOGD(TAG, "Length mismatch frame=%s", util::formatHexBytes(buffer).c_str());
        KNX_LOGW(TAG, "Frame length mismatch: have=%zu expected=%zu", buffer.size(),
                 (idx + tpduLength + 1u));
        return util::ErrorCode::InvalidFrameSize;
    }

    if (frame.hopCount > 7) {
        KNX_LOGW(TAG, "Hop count exceeds 3-bit field: %d", frame.hopCount);
        return util::ErrorCode::InvalidFrameSize;
    }

    if (tpduLength < 1u) {
        KNX_LOGW(TAG, "TPDU length too small: %zu", tpduLength);
        return util::ErrorCode::InvalidFrameSize;
    }

    frame.tpdu.assign(std::span<const uint8_t>(buffer.data() + idx, tpduLength));

    if (!frame.isValid()) {
        KNX_LOGD(TAG, "Decoded invalid frame bytes=%s", util::formatHexBytes(buffer).c_str());
        KNX_LOGW(TAG, "Decoded frame failed validation");
        return util::ErrorCode::DecodeFailed;
    }

    return util::Result<void>::ok();
}

} // namespace datalink
} // namespace knx
