// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file cemi.cpp
 * @brief KNXnet/IP cEMI framing helpers implementation
 */

#include "knx/netip/cemi.hpp"
#include "knx/util/log.hpp"
#include "knx/util/bit_ops.hpp"
#include "knx/util/result.hpp"
#include <cstring>

[[maybe_unused]] static const char* TAG = "KNX.cEMI";
namespace bits = knx::util;

namespace knx {
namespace netip {

static constexpr uint8_t CEMI_MC_L_DATA_REQ = 0x11;
static constexpr uint8_t CEMI_MC_L_DATA_IND = 0x29;
static constexpr uint8_t CEMI_MC_L_DATA_CON = 0x2E;

static inline bool isSupportedLDataMessageCode(uint8_t messageCode) {
    switch (messageCode) {
        case CEMI_MC_L_DATA_REQ:
        case CEMI_MC_L_DATA_IND:
        case CEMI_MC_L_DATA_CON:
            return true;
        default:
            return false;
    }
}

// cEMI control field masks (aligned to TP1 semantics where applicable)
static constexpr uint8_t CF1_FRAMEFMT_STD = 0x80;  // 1=standard, 0=extended
static constexpr uint8_t CF1_REPEAT        = 0x20;  // repeat flag (1 = not repeated)
static constexpr uint8_t CF1_BROADCAST     = 0x10;  // 1=broadcast, 0=system broadcast
static constexpr uint8_t CF1_PRIORITY_MASK = 0x0C;  // priority bits
static constexpr uint8_t CF1_ACK_REQ       = 0x02;  // ACK requested
static constexpr uint8_t CF1_CONFIRM       = 0x01;  // confirmation

// CF2 destination address type bit: 1=group address
static constexpr uint8_t CF2_DEST_GROUP    = 0x80;
// CF2 hopcount bits (6..4)
static constexpr uint8_t CF2_HOPCOUNT_MASK = 0x70;
static constexpr uint8_t CF2_HOPCOUNT_SHIFT = 4;

util::Result<size_t> encodeCemiLData(const datalink::LDataFrame& frame,
                                     uint8_t messageCode,
                                     std::span<uint8_t> out) {
    if (!isSupportedLDataMessageCode(messageCode)) {
        return util::ErrorCode::InvalidParameter;
    }

    // cEMI NPDU length is a single byte.
    // TPDU must include at least the 2-byte TPCI/APCI header.
    if (frame.tpdu.size() < 2 || frame.tpdu.size() > 256) {
        return util::ErrorCode::InvalidParameter;
    }

    const size_t encodedSize = encodedCemiLDataSize(frame);
    if (out.size() < encodedSize) {
        return util::ErrorCode::BufferTooSmall;
    }

    size_t idx = 0;

    // Message code + no additional info
    out[idx++] = messageCode;
    out[idx++] = 0x00; // AddInfoLen

    // Control Field 1
    uint8_t cf1 = 0;
    if (frame.standardFrame) cf1 |= CF1_FRAMEFMT_STD;
    if (!frame.repeated)     cf1 |= CF1_REPEAT;  // cEMI repeat bit: 1 = not repeated
    // Broadcast-type flag: only the group destination 0/0/0 travels in the
    // system-broadcast domain (bit = 0); everything else is normal broadcast.
    const bool systemBroadcast = isGroupAddress(frame.destinationType)
        && frame.destination.raw == 0x0000u;
    if (!systemBroadcast)    cf1 |= CF1_BROADCAST;
    cf1 |= (static_cast<uint8_t>(frame.priority) << 2) & CF1_PRIORITY_MASK;
    if (frame.ackRequested)  cf1 |= CF1_ACK_REQ;
    if (frame.confirmation)  cf1 |= CF1_CONFIRM;
    out[idx++] = cf1;

    // Control Field 2: dest type + hop count
    uint8_t cf2 = (isGroupAddress(frame.destinationType) ? CF2_DEST_GROUP : 0x00) |
                  ((frame.hopCount & 0x07) << CF2_HOPCOUNT_SHIFT);
    out[idx++] = cf2;

    // Source address (big-endian)
    uint16_t src = frame.source.raw;
    out[idx++] = bits::getHighByte(src);
    out[idx++] = bits::getLowByte(src);

    // Destination address (big-endian), without type bit (CF2 carries it)
    uint16_t dst = frame.destination.raw;
    out[idx++] = bits::getHighByte(dst);
    out[idx++] = bits::getLowByte(dst);

    // NPDU length: number of bytes following the first TPDU byte.
    // (Some stacks interpret this as APDU length.)
    const size_t tpduLen = frame.tpdu.size();
    const uint8_t npduLen = static_cast<uint8_t>(tpduLen - 1);
    out[idx++] = npduLen;

    // TPDU bytes
    std::memcpy(out.data() + idx, frame.tpdu.data(), tpduLen);
    idx += tpduLen;

    return idx;
}

util::Result<void> decodeCemiLData(std::span<const uint8_t> buffer,
                                   datalink::LDataFrame& frame,
                                   uint8_t& messageCode) {
    // Minimal cEMI L_Data with AddInfoLen=0 and 2-byte TPDU header.
    if (buffer.size() < 11) {
        return util::ErrorCode::InvalidFrameSize;
    }

    size_t idx = 0;
    messageCode = buffer[idx++];

    if (!isSupportedLDataMessageCode(messageCode)) {
        return util::ErrorCode::DecodeFailed;
    }
    uint8_t addInfoLen = buffer[idx++];

    // Skip additional info if present
    if (buffer.size() < idx + addInfoLen + 1) {
        return util::ErrorCode::InvalidFrameSize;
    }
    idx += addInfoLen;

    // Control fields
    uint8_t cf1 = buffer[idx++];
    uint8_t cf2 = buffer[idx++];

    frame.standardFrame = (cf1 & CF1_FRAMEFMT_STD) != 0;
    frame.repeated      = (cf1 & CF1_REPEAT) == 0;  // cEMI repeat bit: 1 = not repeated
    frame.priority      = static_cast<Priority>((cf1 & CF1_PRIORITY_MASK) >> 2);
    frame.ackRequested  = (cf1 & CF1_ACK_REQ) != 0;
    frame.confirmation  = (cf1 & CF1_CONFIRM) != 0;

    frame.destinationType = (cf2 & CF2_DEST_GROUP) != 0
        ? AddressType::Group
        : AddressType::Individual;
    frame.hopCount       = (cf2 & CF2_HOPCOUNT_MASK) >> CF2_HOPCOUNT_SHIFT;

    // In cEMI L_Data, the lower nibble of CF2 is reserved/0 for standard frames
    // (data length is provided explicitly as a separate byte).
    if (frame.standardFrame && (cf2 & 0x0F) != 0) {
        return util::ErrorCode::DecodeFailed;
    }

    // Source address
    if (buffer.size() < idx + 6) { // src(2)+dst(2)+len(1)+tpci(1) minimal
        return util::ErrorCode::InvalidFrameSize;
    }
    uint16_t src = (static_cast<uint16_t>(buffer[idx]) << 8) | buffer[idx + 1];
    frame.source = IndividualAddress(src);
    idx += 2;

    // Destination address
    uint16_t dst = (static_cast<uint16_t>(buffer[idx]) << 8) | buffer[idx + 1];
    frame.destination = GroupAddress(dst);
    idx += 2;

    // NPDU length (APDU length). TPDU total length is NPDU+1 (first TPDU byte carries TPCI/APCI high bits).
    uint8_t npduLen = buffer[idx++];
    const uint16_t tpduLen = static_cast<uint16_t>(npduLen) + 1u;
    if (tpduLen < 2) {
        return util::ErrorCode::DecodeFailed;
    }
    if (buffer.size() < idx + tpduLen) {
        return util::ErrorCode::InvalidFrameSize;
    }

    if (!frame.tpdu.assign(buffer.subspan(idx, tpduLen))) {
        return util::ErrorCode::BufferTooSmall;
    }
    idx += tpduLen;

    // DataLen must match the remaining payload exactly (no trailing bytes).
    if (idx != buffer.size()) {
        return util::ErrorCode::InvalidFrameSize;
    }

    return util::Result<void>::ok();
}

} // namespace netip
} // namespace knx
