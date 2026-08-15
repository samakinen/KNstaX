// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/secure_packet_codec.hpp"
#include "knx/netip/udp_datagram_channel.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

class SecureUdpDatagramChannel {
public:
    SecureUdpDatagramChannel(platform::UdpSocket& socket,
                             NetIpSecurity& security,
                             std::span<uint8_t> wrapScratch,
                             std::span<uint8_t> unwrapScratch) noexcept
        : socket_(socket)
        , codec_(security, wrapScratch, unwrapScratch)
    {
    }

    util::Result<void> send(const UdpDatagramEndpoint& remote, std::span<const uint8_t> payload)
    {
        if (!socket_.isOpen()) return util::ErrorCode::NotInitialized;
        if (remote.addr.isZero() || !remote.port.isValid()) return util::ErrorCode::InvalidAddress;

        auto wrapped = codec_.wrap(payload);
        if (wrapped.isError()) return wrapped.error();

        const int sent = socket_.send(remote.addr, remote.port.value(), wrapped.value());
        if (sent != static_cast<int>(wrapped.value().size())) return util::ErrorCode::TransmissionFailed;
        return util::Result<void>::ok();
    }

    util::Result<size_t> receive(std::span<uint8_t> plainBuffer, IpAddress& srcAddr, uint16_t& srcPort)
    {
        if (!socket_.isOpen()) return util::ErrorCode::NotInitialized;

        const int received = socket_.receive(plainBuffer, srcAddr, srcPort);
        if (received < 0) return util::ErrorCode::TransmissionFailed;

        return codec_.unwrapInPlace(plainBuffer.first(static_cast<size_t>(received)), plainBuffer);
    }

    util::Result<size_t> exchange(const UdpDatagramEndpoint& remote,
                                  std::span<const uint8_t> request,
                                  std::span<uint8_t> responseBuffer,
                                  int timeoutMs)
    {
        auto sendResult = send(remote, request);
        if (sendResult.isError()) return sendResult.error();

        auto waitResult = UdpDatagramChannel::waitReadable(socket_, timeoutMs);
        if (waitResult.isError()) return waitResult.error();
        if (!waitResult.value()) return util::ErrorCode::Timeout;

        IpAddress srcAddr(0);
        uint16_t srcPort = 0;
        auto receiveResult = receive(responseBuffer, srcAddr, srcPort);
        if (receiveResult.isError()) return receiveResult.error();
        if (srcAddr != remote.addr || srcPort != remote.port.value()) return util::ErrorCode::OperationFailed;
        return receiveResult.value();
    }

private:
    platform::UdpSocket& socket_;
    SecurePacketCodec codec_;
};

} // namespace netip
} // namespace knx