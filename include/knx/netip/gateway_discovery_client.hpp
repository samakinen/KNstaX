// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/gateway_info.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/types.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace knx {
namespace netip {

class GatewayDiscoveryClient {
public:
    static util::Result<void> parseSearchResponsePacket(std::span<const uint8_t> data, GatewayInfo& info);
    static util::Result<void> parseDescriptionResponsePacket(std::span<const uint8_t> data, GatewayInfo& info);

    util::Result<void> beginDiscover(platform::NetworkInterface& network, int timeoutMs = 2000, size_t maxGateways = 10);
    util::Result<util::OperationProgressState> pollDiscover();
    std::vector<GatewayInfo> discover(platform::NetworkInterface& network, int timeoutMs = 2000, size_t maxGateways = 10);
    void setTimingPlatform(platform::TimingPlatform* timingPlatform) noexcept { timingPlatform_ = timingPlatform; }
    platform::TimingPlatform* timingPlatform() const noexcept { return timingPlatform_; }
    const std::vector<GatewayInfo>& discoveredGateways() const noexcept { return discoveredGateways_; }
    std::vector<GatewayInfo> takeDiscoveredGateways();
    bool isDiscovering() const noexcept { return discoverOperation_.active; }

    util::Result<void> getDescription(platform::NetworkInterface& network,
                                      IpAddress host,
                                      NetIpPort port,
                                      int timeoutMs,
                                      GatewayInfo& info);

private:
    struct DiscoverOperation {
        bool active{false};
        uint32_t startTimeMs{0};
        int timeoutMs{0};
        size_t maxGateways{0};

        constexpr void reset() noexcept
        {
            active = false;
            startTimeMs = 0;
            timeoutMs = 0;
            maxGateways = 0;
        }
    };

    util::Result<void> sendSearchRequest(platform::UdpSocket& socket,
                                         IpAddress localAddr);
    void finishDiscovery() noexcept;

    static constexpr size_t kMaxDatagramLen = 1500;

    std::unique_ptr<platform::UdpSocket> discoverSocket_{};
    std::vector<GatewayInfo> discoveredGateways_{};
    DiscoverOperation discoverOperation_{};
    platform::TimingPlatform* timingPlatform_{nullptr};
    DatagramBuffer<kMaxDatagramLen> frameBuffer_{};
    DatagramBuffer<kMaxDatagramLen> scratchBuffer_{};
};

} // namespace netip
} // namespace knx