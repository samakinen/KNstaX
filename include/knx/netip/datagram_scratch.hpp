// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

class PacketWriter {
public:
    explicit constexpr PacketWriter(std::span<uint8_t> buffer) noexcept : buffer_(buffer) {}

    constexpr util::Result<void> reset(size_t initialSize = 0) noexcept
    {
        if (initialSize > buffer_.size()) return util::ErrorCode::BufferTooSmall;
        size_ = initialSize;
        return util::Result<void>::ok();
    }

    constexpr util::Result<void> push(uint8_t byte) noexcept
    {
        if (size_ >= buffer_.size()) return util::ErrorCode::BufferTooSmall;
        buffer_[size_++] = byte;
        return util::Result<void>::ok();
    }

    constexpr util::Result<void> write(std::span<const uint8_t> bytes) noexcept
    {
        if ((size_ + bytes.size()) > buffer_.size()) return util::ErrorCode::BufferTooSmall;
        for (size_t i = 0; i < bytes.size(); ++i) {
            buffer_[size_ + i] = bytes[i];
        }
        size_ += bytes.size();
        return util::Result<void>::ok();
    }

    constexpr std::span<uint8_t> span() noexcept { return buffer_.first(size_); }
    constexpr std::span<const uint8_t> span() const noexcept { return buffer_.first(size_); }
    constexpr size_t size() const noexcept { return size_; }
    constexpr size_t capacity() const noexcept { return buffer_.size(); }

private:
    std::span<uint8_t> buffer_;
    size_t size_{0};
};

template <size_t Capacity>
struct DatagramBuffer {
    static constexpr size_t kCapacity = Capacity;

    std::array<uint8_t, kCapacity> bytes{};

    constexpr std::span<uint8_t> span() noexcept { return bytes; }
    constexpr std::span<const uint8_t> span() const noexcept { return bytes; }
};

template <size_t MaxDatagramLen, size_t SecureOverhead>
struct SecureDatagramBuffer {
    static constexpr size_t kCapacity = MaxDatagramLen + SecureOverhead;

    std::array<uint8_t, kCapacity> bytes{};

    constexpr std::span<uint8_t> span() noexcept { return bytes; }
    constexpr std::span<const uint8_t> span() const noexcept { return bytes; }
};

} // namespace netip
} // namespace knx