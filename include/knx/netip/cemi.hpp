// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file cemi.hpp
 * @brief KNXnet/IP cEMI framing helpers
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include "knx/util/result.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"

namespace knx {
namespace netip {

inline constexpr size_t kCemiLDataHeaderSize = 9;
inline constexpr size_t kMaxCemiTpduSize = 256;
inline constexpr size_t kMaxCemiLDataSize = kCemiLDataHeaderSize + kMaxCemiTpduSize;

[[nodiscard]] inline constexpr size_t encodedCemiLDataSize(size_t tpduSize) noexcept
{
    return kCemiLDataHeaderSize + tpduSize;
}

[[nodiscard]] inline constexpr size_t encodedCemiLDataSize(const datalink::LDataFrame& frame) noexcept
{
    return encodedCemiLDataSize(frame.tpdu.size());
}

/**
 * @brief Encode a cEMI L_Data frame into caller-managed storage.
 *
 * The caller provides the cEMI message code (e.g., L_Data.ind or L_Data.req).
 * This helper creates a minimal cEMI payload with:
 *   [MessageCode][AddInfoLen=0][CF1][CF2][SrcHi][SrcLo][DstHi][DstLo][DataLen][TPDU...]
 * It ignores additional info blocks and omits TP1 checksum by design.
 */
util::Result<size_t> encodeCemiLData(const datalink::LDataFrame& frame,
                                     uint8_t messageCode,
                                     std::span<uint8_t> out);

/**
 * @brief Decode a cEMI L_Data frame into an LDataFrame.
 *
 * Returns the message code via output parameter. Assumes AddInfoLen=0 or skips
 * any additional info bytes. Performs basic bounds checks.
 */
util::Result<void> decodeCemiLData(std::span<const uint8_t> buffer,
                                   datalink::LDataFrame& frame,
                                   uint8_t& messageCode);

} // namespace netip
} // namespace knx
