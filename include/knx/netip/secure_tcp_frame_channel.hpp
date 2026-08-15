// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/secure_packet_codec.hpp"
#include "knx/netip/tcp_frame_channel.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

class SecureTcpFrameChannel {
public:
    SecureTcpFrameChannel(platform::TcpSocket& socket,
                          NetIpSecurity& security,
                          std::span<uint8_t> wrapScratch,
                          std::span<uint8_t> unwrapScratch) noexcept
        : socket_(socket)
        , codec_(security, wrapScratch, unwrapScratch)
    {
    }

    util::Result<bool> waitReadable(int timeoutMs,
                                    platform::TimingPlatform* timingPlatform = nullptr) const
    {
        return TcpFrameChannel::waitReadable(socket_, timeoutMs, timingPlatform);
    }

    util::Result<void> sendFrame(std::span<const uint8_t> frame)
    {
        auto wrapped = codec_.wrap(frame);
        if (wrapped.isError()) return wrapped.error();
        return TcpFrameChannel::writeAll(socket_, wrapped.value());
    }

    util::Result<size_t> receiveFrame(std::span<uint8_t> outFrame)
    {
        auto received = TcpFrameChannel::receiveFrame(socket_, outFrame);
        if (received.isError()) return received.error();
        return codec_.unwrapInPlace(outFrame.first(received.value()), outFrame);
    }

private:
    platform::TcpSocket& socket_;
    SecurePacketCodec codec_;
};

} // namespace netip
} // namespace knx