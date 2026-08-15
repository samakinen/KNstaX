// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/control_packet_codec.hpp"

namespace knx {
namespace netip {

class TunnelingControlBuilder {
public:
    static IpAddress selectUdpAdvertisedLocalAddr(IpAddress localAddr, IpAddress remoteAddr)
    {
        if (localAddr.isZero()) return IpAddress(0);
        if (remoteAddr.isLoopback() && !localAddr.isLoopback()) return IpAddress(0);
        return localAddr;
    }

    static control_packet::HpaiEndpoint makeUdpControlEndpoint(IpAddress localAddr,
                                                               IpAddress remoteAddr,
                                                               uint16_t localPort)
    {
        return control_packet::HpaiEndpoint{
            control_packet::HpaiProtocol::Udp,
            selectUdpAdvertisedLocalAddr(localAddr, remoteAddr),
            localPort,
        };
    }

    static control_packet::HpaiEndpoint makeTcpControlEndpoint(IpAddress localAddr, uint16_t localPort)
    {
        return control_packet::HpaiEndpoint{control_packet::HpaiProtocol::Tcp, localAddr, localPort};
    }

    static util::Result<void> encodeConnectionRequest(PacketWriter& writer,
                                                      const control_packet::HpaiEndpoint& endpoint)
    {
        return control_packet::Codec::encodeConnectionRequest(writer, endpoint, endpoint);
    }

    static util::Result<void> encodeConnectionStateRequest(PacketWriter& writer,
                                                           uint8_t channelId,
                                                           const control_packet::HpaiEndpoint& endpoint)
    {
        return control_packet::Codec::encodeConnectionStateRequest(writer, channelId, endpoint);
    }

    static util::Result<void> encodeDisconnectRequest(PacketWriter& writer,
                                                      uint8_t channelId,
                                                      const control_packet::HpaiEndpoint& endpoint)
    {
        return control_packet::Codec::encodeDisconnectRequest(writer, channelId, endpoint);
    }
};

} // namespace netip
} // namespace knx