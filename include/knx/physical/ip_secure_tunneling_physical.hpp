// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#if KNX_SECURE_ENABLED

#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/netip/ip_secure/secure_tunneling_client.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace knx {
namespace physical {

// Adapter that implements TP1 Physical interface over KNXnet/IP Secure Tunneling (TCP)
class IpSecureTunnelingPhysical {
public:
    using ProgressState = util::OperationProgressState;

    IpSecureTunnelingPhysical();
    ~IpSecureTunnelingPhysical();

    IpSecureTunnelingPhysical(const IpSecureTunnelingPhysical&) = delete;
    IpSecureTunnelingPhysical& operator=(const IpSecureTunnelingPhysical&) = delete;

    // Must be called before init(). Required for cross-platform builds.
    void setNetworkInterface(platform::NetworkInterface* network);
    void setTimingPlatform(platform::TimingPlatform* timingPlatform);

    // Configure KNXnet/IP gateway endpoint (host:port); call before init().
    void setGateway(IpAddress host, NetIpPort port);

    // Configure secure tunnelling authentication/identity; call before init().
    void setCredentials(UserId userId,
                        std::span<const uint8_t> passwordLatin1,
                        const std::array<uint8_t, 32>& clientPrivateKey,
                        const std::array<uint8_t, 6>& clientSerial,
                        uint64_t initialSeq = 1);

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

    netip::ip_secure::SecureTunnelingClient client_;

    IpAddress host_;
    NetIpPort port_;

    netip::ip_secure::SecureTunnelingClient::Options options_;

    bool initialized_{};
    PhysicalLayerState state_{PhysicalLayerState::Idle};

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

#endif // KNX_SECURE_ENABLED
