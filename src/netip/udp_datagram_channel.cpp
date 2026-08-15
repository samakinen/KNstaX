// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/udp_datagram_channel.hpp"
#include "knx/netip/detail/polling.hpp"

namespace knx {
namespace netip {

util::Result<bool> UdpDatagramChannel::waitReadable(platform::UdpSocket& socket,
                                                    int timeoutMs,
                                                    platform::TimingPlatform* timingPlatform)
{
    if (!socket.isOpen()) return util::ErrorCode::NotInitialized;

    return detail::waitUntilReadable(timingPlatform, timeoutMs, [&socket]() {
        return socket.available() > 0;
    });
}

util::Result<size_t> UdpDatagramChannel::exchange(platform::UdpSocket& socket,
                                                  const UdpDatagramEndpoint& remote,
                                                  std::span<const uint8_t> request,
                                                  std::span<uint8_t> responseBuffer,
                                                  int timeoutMs,
                                                  platform::TimingPlatform* timingPlatform)
{
    if (!socket.isOpen()) return util::ErrorCode::NotInitialized;
    if (remote.addr.isZero() || !remote.port.isValid()) return util::ErrorCode::InvalidAddress;

    const int sent = socket.send(remote.addr, remote.port.value(), request);
    if (sent != static_cast<int>(request.size())) return util::ErrorCode::TransmissionFailed;

    auto waitResult = waitReadable(socket, timeoutMs, timingPlatform);
    if (waitResult.isError()) return waitResult.error();
    if (!waitResult.value()) return util::ErrorCode::Timeout;

    IpAddress srcAddr(0);
    uint16_t srcPort = 0;
    const int received = socket.receive(responseBuffer, srcAddr, srcPort);
    if (received < 0) return util::ErrorCode::TransmissionFailed;
    if (srcAddr != remote.addr || srcPort != remote.port.value()) return util::ErrorCode::OperationFailed;

    return static_cast<size_t>(received);
}

} // namespace netip
} // namespace knx