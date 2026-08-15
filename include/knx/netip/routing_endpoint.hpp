// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/netip_config.hpp"
#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/netip_security.hpp"
#include "knx/types.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include <atomic>
#include <memory>

namespace knx {
namespace netip {

class RoutingEndpoint {
public:
    struct Options {
        IpAddress multicastGroup{IpAddress::fromOctets(224, 0, 23, 12)};
        NetIpPort port{NetIpPort(config::kDefaultPort)};
        IpAddress interfaceAddress{IpAddress(0)};
        uint8_t ttl{1};
        bool loopback{true};
        NetIpSecurity* security{nullptr};
    };

    RoutingEndpoint();
    ~RoutingEndpoint();

    RoutingEndpoint(const RoutingEndpoint&) = delete;
    RoutingEndpoint& operator=(const RoutingEndpoint&) = delete;

    util::Result<void> open(platform::NetworkInterface& network, const Options& options);

    void setTimingPlatform(platform::TimingPlatform* timingPlatform) noexcept { timingPlatform_ = timingPlatform; }
    platform::TimingPlatform* timingPlatform() const noexcept { return timingPlatform_; }

    void close();
    bool isOpen() const;

    util::Result<void> sendRoutingIndication(std::span<const uint8_t> cemi);

    // Sends a ROUTING_LOST_MESSAGE (lost-message counter) to multicast.
    util::Result<void> sendRoutingLostMessage(uint16_t lostCount);

    // Receives a single UDP datagram and decodes it as ROUTING_INDICATION.
    // Returns true only when a valid routing indication is received and decoded.
    util::Result<size_t> receiveRoutingIndication(std::span<uint8_t> cemiOut, uint32_t timeoutMs);

    uint64_t routingLostMessagesSeen() const { return lostMessagesSeen_.load(); }
    uint64_t routingLostCountTotal() const { return lostCountTotal_.load(); }

private:
    std::unique_ptr<platform::UdpSocket> sock_;
    Options options_;
    IpAddress multicastAddr_{IpAddress(0)};
    IpAddress interfaceAddr_{IpAddress(0)};

    DatagramBuffer<config::kUdpBufferSize> rxBuffer_{};
    DatagramBuffer<config::kUdpBufferSize> securePlainBuffer_{};
    SecureDatagramBuffer<config::kUdpBufferSize, ip_secure::SecureWrapper::kOverhead> secureWrapBuffer_{};
    platform::TimingPlatform* timingPlatform_{nullptr};

    std::atomic<uint64_t> lostMessagesSeen_{0};
    std::atomic<uint64_t> lostCountTotal_{0};
};

} // namespace netip
} // namespace knx
