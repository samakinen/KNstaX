// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file ip_routing_physical.hpp
 * @brief TP1 physical layer adapter backed by KNXnet/IP Routing (multicast)
 */

#pragma once

#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/netip/netip_security.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/types.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <array>
#include <mutex>
#include <string>
#include <span>
#include <vector>

#include "knx/util/result.hpp"

namespace knx {
namespace netip {
class RoutingEndpoint;
}
namespace physical {

class IpRoutingPhysical {
public:
    using ProgressState = util::OperationProgressState;

    IpRoutingPhysical();
    ~IpRoutingPhysical();

    IpRoutingPhysical(const IpRoutingPhysical&) = delete;
    IpRoutingPhysical& operator=(const IpRoutingPhysical&) = delete;

    // Must be called before init().
    void setMulticast(IpAddress multicastGroup,
                      NetIpPort port,
                      IpAddress interfaceAddress = IpAddress(0));

    // Must be called before init(). Required for cross-platform builds.
    void setNetworkInterface(platform::NetworkInterface* network);
    void setTimingPlatform(platform::TimingPlatform* timingPlatform);

    // Optional multicast socket behavior; must be set before init().
    void setMulticastSocketOptions(uint8_t ttl, Toggle loopback);

#if KNX_SECURE_ENABLED
    // Optional: enable KNX/IP Secure routing (SecureWrapper over multicast).
    // Must be called before init().
    void setSecureRouting(const std::array<uint8_t, 16>& groupKey,
                          const std::array<uint8_t, 6>& serial,
                          const std::array<uint8_t, 2>& tag,
                          uint64_t initialSeq = 1);
#endif

    // Bounds to avoid unbounded memory usage under multicast storms.
    void setMaxRxQueueDepth(size_t maxFrames);

    util::Result<void> init();
    void close();
    bool isOpen() const;

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame);
    util::Result<uint32_t> beginTransmit(std::span<const uint8_t> frame);
    util::Result<ProgressState> pollTransmit(uint32_t sequence);
    util::Result<void> beginReceive(uint32_t timeoutMs);
    util::Result<ProgressState> pollReceive();
    util::Result<std::span<const uint8_t>> receivedFrameView();
    void setReceiveCallback(ReceiveCallback callback, void* context);
    PhysicalLayerState getState() const;
    util::Result<void> setBusMonitorMode(Toggle mode);

private:
    void drainInboundOnce(uint32_t timeoutMs);

    platform::NetworkInterface* network_;

    IpAddress multicastGroup_;
    NetIpPort port_;
    IpAddress interfaceAddress_;

    uint8_t multicastTtl_;
    bool multicastLoopback_;

#if KNX_SECURE_ENABLED
    bool secureRoutingEnabled_{false};
    std::array<uint8_t, 16> secureRoutingGroupKey_{};
    std::array<uint8_t, 6> secureRoutingSerial_{};
    std::array<uint8_t, 2> secureRoutingTag_{};
    uint64_t secureRoutingInitialSeq_{1};
#endif

    std::atomic<size_t> maxRxQueueDepth_;
    std::atomic<uint64_t> rxDropped_;

    std::unique_ptr<netip::RoutingEndpoint> endpoint_;

#if KNX_SECURE_ENABLED
    std::unique_ptr<netip::NetIpSecurity> routingSecurity_;
#endif

    std::atomic<bool> initialized_;

    mutable std::mutex rxMutex_;
    std::deque<std::vector<uint8_t>> rxQueue_;
    std::vector<uint8_t> lastReceivedFrame_;
    std::vector<uint8_t> pendingTxFrame_;
    bool txActive_{false};
    uint32_t txSequence_{0};
    ProgressState txState_{ProgressState::Success};
    bool rxActive_{false};
    uint64_t rxDeadlineMs_{0};
    platform::TimingPlatform* timingPlatform_{nullptr};

    ReceiveCallback rxCb_;
    void* rxCtx_;

    std::atomic<PhysicalLayerState> state_;
};

} // namespace physical
} // namespace knx
