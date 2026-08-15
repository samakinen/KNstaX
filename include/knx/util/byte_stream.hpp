// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file byte_stream.hpp
 * @brief ByteReader/ByteWriter utilities for KNX property encoding
 */

#pragma once

#include "knx/util/result.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace knx {
namespace util {

class ByteReader {
public:
    explicit ByteReader(std::span<const std::byte> data)
        : _data(data), _offset(0) {}

    explicit ByteReader(std::span<const uint8_t> data)
        : ByteReader(std::as_bytes(data)) {}

    size_t remaining() const { return (_offset <= _data.size()) ? (_data.size() - _offset) : 0; }
    size_t position() const { return _offset; }

    Result<uint8_t> u8() {
        if (remaining() < 1) {
            return ErrorCode::BufferTooSmall;
        }
        return std::to_integer<uint8_t>(_data[_offset++]);
    }

    Result<uint16_t> u16be() {
        if (remaining() < 2) {
            return ErrorCode::BufferTooSmall;
        }
        const uint16_t value = (static_cast<uint16_t>(std::to_integer<uint8_t>(_data[_offset])) << 8)
                     | static_cast<uint16_t>(std::to_integer<uint8_t>(_data[_offset + 1]));
        _offset += 2;
        return value;
    }

    Result<uint32_t> u32be() {
        if (remaining() < 4) {
            return ErrorCode::BufferTooSmall;
        }
        const uint32_t value = (static_cast<uint32_t>(std::to_integer<uint8_t>(_data[_offset])) << 24)
                             | (static_cast<uint32_t>(std::to_integer<uint8_t>(_data[_offset + 1])) << 16)
                             | (static_cast<uint32_t>(std::to_integer<uint8_t>(_data[_offset + 2])) << 8)
                             | static_cast<uint32_t>(std::to_integer<uint8_t>(_data[_offset + 3]));
        _offset += 4;
        return value;
    }

    Result<void> readBytes(std::span<std::byte> out) {
        if (remaining() < out.size()) {
            return Result<void>::err(ErrorCode::BufferTooSmall);
        }
        std::ranges::copy(_data.subspan(_offset, out.size()), out.begin());
        _offset += out.size();
        return Result<void>::ok();
    }

    Result<void> readBytes(std::span<uint8_t> out) {
        return readBytes(std::as_writable_bytes(out));
    }

    bool skip(size_t length) {
        if (remaining() < length) {
            return false;
        }
        _offset += length;
        return true;
    }

private:
    std::span<const std::byte> _data;
    size_t _offset;
};

class ByteWriter {
public:
    explicit ByteWriter(std::span<std::byte> data)
        : _data(data), _offset(0) {}

    explicit ByteWriter(std::span<uint8_t> data)
        : ByteWriter(std::as_writable_bytes(data)) {}

    size_t remaining() const { return (_offset <= _data.size()) ? (_data.size() - _offset) : 0; }
    size_t written() const { return _offset; }

    Result<void> u8(uint8_t value) {
        if (remaining() < 1) {
            return Result<void>::err(ErrorCode::BufferTooSmall);
        }
        _data[_offset++] = static_cast<std::byte>(value);
        return Result<void>::ok();
    }

    Result<void> u16be(uint16_t value) {
        if (remaining() < 2) {
            return Result<void>::err(ErrorCode::BufferTooSmall);
        }
        _data[_offset++] = static_cast<std::byte>((value >> 8) & 0xFFu);
        _data[_offset++] = static_cast<std::byte>(value & 0xFFu);
        return Result<void>::ok();
    }

    Result<void> u32be(uint32_t value) {
        if (remaining() < 4) {
            return Result<void>::err(ErrorCode::BufferTooSmall);
        }
        _data[_offset++] = static_cast<std::byte>((value >> 24) & 0xFFu);
        _data[_offset++] = static_cast<std::byte>((value >> 16) & 0xFFu);
        _data[_offset++] = static_cast<std::byte>((value >> 8) & 0xFFu);
        _data[_offset++] = static_cast<std::byte>(value & 0xFFu);
        return Result<void>::ok();
    }

    Result<void> writeBytes(std::span<const std::byte> data) {
        if (remaining() < data.size()) {
            return Result<void>::err(ErrorCode::BufferTooSmall);
        }
        std::ranges::copy(data, _data.begin() + static_cast<std::ptrdiff_t>(_offset));
        _offset += data.size();
        return Result<void>::ok();
    }

    Result<void> writeBytes(std::span<const uint8_t> data) {
        return writeBytes(std::as_bytes(data));
    }

private:
    std::span<std::byte> _data;
    size_t _offset;
};

} // namespace util
} // namespace knx
