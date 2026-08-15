// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/tunneling_server_endpoint.hpp"
#include "knx/util/result.hpp"

#include <atomic>

namespace knx {
namespace network {

class Tp1IpInterfaceBridge {
public:
    struct Statistics {
        uint64_t tp1ToIpForwarded{0};
        uint64_t ipToTp1Forwarded{0};
        uint64_t tp1ToIpDropped{0};
        uint64_t ipToTp1Dropped{0};
    };

    Tp1IpInterfaceBridge(datalink::Tp1DataLinkLayer& tp1Line,
                         netip::TunnelingServerEndpoint& tunnelingServer);

    util::Result<void> init();
    void close();

    Statistics statistics() const;

private:
    void onTp1Frame(const datalink::LDataFrame& frame);
    void onTunnelingCemi(ChannelId channelId, std::span<const uint8_t> cemi);

    datalink::Tp1DataLinkLayer& tp1Line_;
    netip::TunnelingServerEndpoint& tunnelingServer_;

    std::atomic<bool> initialized_{false};
    std::atomic<uint64_t> tp1ToIpForwarded_{0};
    std::atomic<uint64_t> ipToTp1Forwarded_{0};
    std::atomic<uint64_t> tp1ToIpDropped_{0};
    std::atomic<uint64_t> ipToTp1Dropped_{0};
};

} // namespace network
} // namespace knx
