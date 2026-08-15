// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/tcp_frame_channel.hpp"

#include "knx/netip/detail/polling.hpp"
#include "knx/netip/header_codec.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace knx {
namespace netip {

util::Result<bool> TcpFrameChannel::waitReadable(platform::TcpSocket& socket,
                                                 int timeoutMs,
                                                 platform::TimingPlatform* timingPlatform)
{
    if (!socket.isOpen()) return util::ErrorCode::NotInitialized;

    return detail::waitUntilReadable(timingPlatform, timeoutMs, [&socket]() {
        return socket.available() > 0;
    });
}

util::Result<void> TcpFrameChannel::writeAll(platform::TcpSocket& socket, std::span<const uint8_t> data)
{
    if (!socket.isOpen()) return util::ErrorCode::NotInitialized;

    size_t sentTotal = 0;
    while (sentTotal < data.size()) {
        const int sent = socket.send(data.subspan(sentTotal));
        if (sent <= 0) return util::ErrorCode::TransmissionFailed;
        sentTotal += static_cast<size_t>(sent);
    }
    return util::Result<void>::ok();
}

util::Result<void> TcpFrameChannel::readExact(platform::TcpSocket& socket, std::span<uint8_t> out)
{
    if (!socket.isOpen()) return util::ErrorCode::NotInitialized;
    if (out.data() == nullptr || out.empty()) return util::ErrorCode::InvalidParameter;

    size_t receivedTotal = 0;
    while (receivedTotal < out.size()) {
        const int received = socket.receive(out.subspan(receivedTotal));
        if (received <= 0) return util::ErrorCode::TransmissionFailed;
        receivedTotal += static_cast<size_t>(received);
    }
    return util::Result<void>::ok();
}

util::Result<void> TcpFrameChannel::sendFrame(platform::TcpSocket& socket,
                                              std::span<const uint8_t> frame)
{
    if (!socket.isOpen()) return util::ErrorCode::NotInitialized;
    if (frame.data() == nullptr && !frame.empty()) return util::ErrorCode::InvalidParameter;

    return writeAll(socket, frame);
}

util::Result<size_t> TcpFrameChannel::receiveFrame(platform::TcpSocket& socket,
                                                   std::span<uint8_t> frameBuffer)
{
    return readFrameRaw(socket, frameBuffer);
}

util::Result<size_t> TcpFrameChannel::readFrameRaw(platform::TcpSocket& socket, std::span<uint8_t> outFrame)
{
    if (!socket.isOpen()) return util::ErrorCode::NotInitialized;
    if (outFrame.size() < KnxNetIpCodec::kHeaderLen) return util::ErrorCode::BufferTooSmall;

    std::array<uint8_t, KnxNetIpCodec::kHeaderLen> headerBytes{};
    auto headerResult = readExact(socket, headerBytes);
    if (headerResult.isError()) return headerResult.error();

    const uint16_t totalLen = static_cast<uint16_t>((static_cast<uint16_t>(headerBytes[4]) << 8) | headerBytes[5]);
    if (totalLen < KnxNetIpCodec::kHeaderLen) return util::ErrorCode::InvalidFrameSize;
    if (static_cast<size_t>(totalLen) > outFrame.size()) return util::ErrorCode::BufferTooSmall;

    std::memcpy(outFrame.data(), headerBytes.data(), headerBytes.size());

    const size_t remaining = static_cast<size_t>(totalLen) - headerBytes.size();
    if (remaining > 0) {
        auto bodyResult = readExact(socket, outFrame.subspan(headerBytes.size(), remaining));
        if (bodyResult.isError()) return bodyResult.error();
    }

    return static_cast<size_t>(totalLen);
}

} // namespace netip
} // namespace knx