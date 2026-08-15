// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/network/tp1_ip_interface_bridge.hpp"

#include "knx/netip/cemi.hpp"
#include "knx/util/log.hpp"

#include <array>

namespace knx {
namespace network {

static const char* TAG = "KNX.Tp1IpIf";

Tp1IpInterfaceBridge::Tp1IpInterfaceBridge(datalink::Tp1DataLinkLayer& tp1Line,
                                           netip::TunnelingServerEndpoint& tunnelingServer)
    : tp1Line_(tp1Line)
    , tunnelingServer_(tunnelingServer)
{
}

util::Result<void> Tp1IpInterfaceBridge::init()
{
    if (initialized_.exchange(true)) {
        return util::Result<void>::ok();
    }

    tp1Line_.setPromiscuousMode(datalink::PromiscuousMode::Enable);
    tp1Line_.setReceiveCallback([this](const datalink::LDataFrame& frame) {
        onTp1Frame(frame);
    });

    tunnelingServer_.setReceiveCallback([this](ChannelId channelId, std::span<const uint8_t> cemi) {
        onTunnelingCemi(channelId, cemi);
    });

    return util::Result<void>::ok();
}

void Tp1IpInterfaceBridge::close()
{
    if (!initialized_.exchange(false)) {
        return;
    }

    tp1Line_.setReceiveCallback(nullptr);
    tunnelingServer_.setReceiveCallback({});
}

Tp1IpInterfaceBridge::Statistics Tp1IpInterfaceBridge::statistics() const
{
    return Statistics{
        .tp1ToIpForwarded = tp1ToIpForwarded_.load(),
        .ipToTp1Forwarded = ipToTp1Forwarded_.load(),
        .tp1ToIpDropped = tp1ToIpDropped_.load(),
        .ipToTp1Dropped = ipToTp1Dropped_.load(),
    };
}

void Tp1IpInterfaceBridge::onTp1Frame(const datalink::LDataFrame& frame)
{
    std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
    auto encodeResult = netip::encodeCemiLData(frame, 0x29, cemi);
    if (encodeResult.isError()) {
        tp1ToIpDropped_.fetch_add(1);
        return;
    }

    auto sendResult = tunnelingServer_.sendCemiToAll(std::span<const uint8_t>(cemi.data(), encodeResult.value()));
    if (sendResult.isError()) {
        tp1ToIpDropped_.fetch_add(1);
        KNX_LOGW(TAG, "TP1->IP fanout failed: %d", static_cast<int>(sendResult.error()));
        return;
    }

    tp1ToIpForwarded_.fetch_add(sendResult.value());
}

void Tp1IpInterfaceBridge::onTunnelingCemi(ChannelId channelId, std::span<const uint8_t> cemi)
{
    datalink::LDataFrame frame;
    uint8_t messageCode = 0;
    auto decodeResult = netip::decodeCemiLData(cemi, frame, messageCode);
    if (decodeResult.isError()) {
        ipToTp1Dropped_.fetch_add(1);
        KNX_LOGW(TAG, "IP->TP1 decode failed on channel=%u: %d",
                 static_cast<unsigned>(channelId.value()),
                 static_cast<int>(decodeResult.error()));
        return;
    }

    auto sendResult = tp1Line_.sendFrame(frame);
    if (sendResult.isError()) {
        ipToTp1Dropped_.fetch_add(1);
        KNX_LOGW(TAG, "IP->TP1 send failed on channel=%u: %d",
                 static_cast<unsigned>(channelId.value()),
                 static_cast<int>(sendResult.error()));
        return;
    }

    ipToTp1Forwarded_.fetch_add(1);
}

} // namespace network
} // namespace knx
