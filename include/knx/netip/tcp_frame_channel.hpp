// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

class TcpFrameChannel {
public:
    static util::Result<bool> waitReadable(platform::TcpSocket& socket,
                                           int timeoutMs,
                                           platform::TimingPlatform* timingPlatform = nullptr);

    static util::Result<void> writeAll(platform::TcpSocket& socket, std::span<const uint8_t> data);
    static util::Result<void> readExact(platform::TcpSocket& socket, std::span<uint8_t> out);

    static util::Result<void> sendFrame(platform::TcpSocket& socket,
                                    std::span<const uint8_t> frame);

    static util::Result<size_t> receiveFrame(platform::TcpSocket& socket,
                                        std::span<uint8_t> frameBuffer);

private:
    static util::Result<size_t> readFrameRaw(platform::TcpSocket& socket, std::span<uint8_t> outFrame);
};

} // namespace netip
} // namespace knx