// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file frame_codec.hpp
 * @brief Shared TP1 frame encoding/decoding utilities
 * 
 * Extracted from Tp1DataLinkLayer to avoid code duplication between
 * RTOS and stub implementations.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include "knx/util/result.hpp"

namespace knx {
namespace datalink {

// Forward declarations
struct LDataFrame;

/**
 * @brief TP1 frame codec utilities
 * 
 * Provides platform-agnostic frame encoding/decoding to prevent
 * duplication between TP1 data link layer implementations.
 */
class FrameCodec {
public:
    static util::Result<size_t> encodeFrame(const LDataFrame& frame, std::span<uint8_t> buffer);

    static util::Result<void> decodeFrame(std::span<const uint8_t> buffer, LDataFrame& frame);

    static uint8_t calculateChecksum(std::span<const uint8_t> data);

    static util::Result<void> verifyChecksum(std::span<const uint8_t> data);
};

} // namespace datalink
} // namespace knx
