// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "knx/util/result.hpp"

namespace knx {
namespace util {

inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

// Parses hex into bytes. Ignores whitespace.
inline Result<size_t> parseHex(std::string_view hex, std::span<uint8_t> out) {
    if (hex.empty()) {
        return util::Result<size_t>(util::ErrorCode::InvalidParameter);
    }

    size_t written = 0;
    int high = -1;
    for (char c : hex) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        const int n = hexNibble(c);
        if (n < 0) return util::Result<size_t>(util::ErrorCode::DecodeFailed);
        if (high < 0) {
            high = n;
        } else {
            if (written >= out.size()) return util::Result<size_t>(util::ErrorCode::BufferTooSmall);
            out[written++] = static_cast<uint8_t>((high << 4) | n);
            high = -1;
        }
    }

    if (high >= 0) {
        return util::Result<size_t>(util::ErrorCode::InvalidFrameSize);
    }
    return util::Result<size_t>(written);
}

inline std::string toHex(std::span<const uint8_t> data) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (size_t i = 0; i < data.size(); ++i) {
        const uint8_t b = data[i];
        s.push_back(digits[(b >> 4) & 0x0F]);
        s.push_back(digits[b & 0x0F]);
    }
    return s;
}

inline std::string formatHexBytes(std::span<const uint8_t> data) {
    static const char* digits = "0123456789ABCDEF";
    std::string output;
    output.reserve((data.size() * 3u) + 2u);
    output.push_back('[');

    for (size_t index = 0; index < data.size(); ++index) {
        if (index != 0u) {
            output.push_back(' ');
        }

        const uint8_t value = data[index];
        output.push_back(digits[(value >> 4) & 0x0Fu]);
        output.push_back(digits[value & 0x0Fu]);
    }

    output.push_back(']');
    return output;
}

} // namespace util
} // namespace knx
