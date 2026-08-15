// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/netip/tunneling_session_client.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/util/operation_progress.hpp"
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace knx {
namespace physical {

// Adapter that implements TP1 Physical interface over KNXnet/IP Tunneling
class IpTunnelingPhysical {
public:
    using ProgressState = util::OperationProgressState;

    IpTunnelingPhysical();
    ~IpTunnelingPhysical();

    // Must be called before init(). Required for cross-platform builds.
    void setNetworkInterface(platform::NetworkInterface* network);
    void setTimingPlatform(platform::TimingPlatform* timingPlatform);

    // Configure KNXnet/IP gateway endpoint (host:port); call before init()
    void setGateway(IpAddress host, NetIpPort port);

    // Optional: inject KNXnet/IP datagram security wrapper (advanced use).
    // Must be called before init(); the caller owns the security instance.
    void setNetIpSecurity(netip::NetIpSecurity* security);

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

    platform::NetworkInterface* network_{nullptr};
    netip::TunnelingSessionClient client_;
    netip::NetIpSecurity* security_{nullptr};
    IpAddress host_;
    NetIpPort port_;
    bool initialized_{};
    PhysicalLayerState state_ { PhysicalLayerState::Idle };
    ReceiveCallback rxCb_{};
    void* rxCtx_{};
    std::deque<std::vector<uint8_t>> rxQueue_;
    std::vector<uint8_t> lastReceivedFrame_;
    std::vector<uint8_t> pendingTxCemi_;
    bool txActive_{false};
    uint32_t txSequence_{0};
    ProgressState txState_{ProgressState::Success};
    bool rxActive_{false};
    uint64_t rxDeadlineMs_{0};
    platform::TimingPlatform* timingPlatform_{nullptr};
    std::mutex rxMutex_;
};

} // namespace physical
} // namespace knx
