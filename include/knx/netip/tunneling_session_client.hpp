// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/device_management_codec.hpp"
#include "knx/netip/device_management_connection_source.hpp"
#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/netip_security.hpp"
#include "knx/netip/session_keepalive_runner.hpp"
#include "knx/netip/tunneling_orchestration.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/types.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

namespace knx {
namespace netip {

class TunnelingSessionClient {
public:
    using ReceiveCallback = util::InplaceFunction<void(std::span<const uint8_t>), 64>;

    enum class SessionOperationType : uint8_t {
        None = 0,
        Open,
    };

    TunnelingSessionClient();
    ~TunnelingSessionClient();

    TunnelingSessionClient(const TunnelingSessionClient&) = delete;
    TunnelingSessionClient& operator=(const TunnelingSessionClient&) = delete;

    util::Result<void> beginOpen(platform::NetworkInterface& network,
                                 IpAddress host,
                                 NetIpPort port,
                                 int timeoutMs = 1000);
    util::Result<util::OperationProgressState> pollOpen();
    util::Result<void> open(platform::NetworkInterface& network, IpAddress host, NetIpPort port, int timeoutMs = 1000);
    void close();

    util::Result<void> sendConnectionStateRequest(int timeoutMs = 1000);
    void startKeepalive(uint32_t intervalMs = config::kTunnelingKeepaliveIntervalMs);
    void stopKeepalive();
    bool isKeepaliveActive() const;

    util::Result<void> sendCemi(std::span<const uint8_t> cemi, bool waitAck = true, int timeoutMs = 500);
    void setReceiveCallback(ReceiveCallback cb);
    void setNetIpSecurity(NetIpSecurity* security) { security_ = security; }
    void setTimingPlatform(platform::TimingPlatform* timingPlatform) noexcept { timingPlatform_ = timingPlatform; }
    platform::TimingPlatform* timingPlatform() const noexcept { return timingPlatform_; }
    util::Result<bool> poll(int timeoutMs = 0);
    util::Result<device_management::ConnectionHeader> acquireDeviceManagementConnectionHeader();
    DeviceManagementConnectionProvider deviceManagementConnectionProvider() noexcept
    {
        return DeviceManagementConnectionProvider::from<TunnelingSessionClient,
                                                       &TunnelingSessionClient::acquireDeviceManagementConnectionHeader>(*this);
    }

    bool isOpen() const noexcept;
    SessionOperationType activeSessionOperation() const noexcept;
    ChannelId channelId() const noexcept;
    TunnelingSequence sequence() const noexcept;
    uint32_t getTimeSinceLastActivity() const;

private:
    struct OpenOperation {
        bool active{false};
        uint32_t startTimeMs{0};
        int timeoutMs{0};

        constexpr void reset() noexcept
        {
            active = false;
            startTimeMs = 0;
            timeoutMs = 0;
        }
    };

    struct RemoteInfo {
        IpAddress addr{IpAddress(0)};
        NetIpPort port{NetIpPort::invalid()};
    };

    util::Result<void> sendConnectionRequest(IpAddress host, NetIpPort port);
    util::Result<bool> receiveConnectionResponseOnce();
    util::Result<void> sendDisconnect();
    util::Result<void> sendTunnelingRequest(std::span<const uint8_t> cemi);
    util::Result<bool> receiveOnce(int timeoutMs);
    uint32_t getCurrentTimeMs() const noexcept;

    static constexpr size_t kMaxDatagramLen = config::kUdpBufferSize;

    std::unique_ptr<platform::UdpSocket> sock_;
    ChannelId channelId_{ChannelId::invalid()};
    TunnelingSequence seq_{TunnelingSequence(0)};
    ReceiveCallback rxCb_{};
    RemoteInfo remote_{};
    IpAddress localAddr_{IpAddress(0)};

    uint32_t lastActivityTimeMs_{0};
    SessionKeepaliveRunner keepalive_;

    mutable std::mutex ioMutex_;
    TunnelingAckTracker ackTracker_{};
    OpenOperation openOperation_{};

    NetIpSecurity* security_{nullptr};
    platform::TimingPlatform* timingPlatform_{nullptr};
    DatagramBuffer<kMaxDatagramLen> frameBuffer_{};
    DatagramBuffer<kMaxDatagramLen> scratchBuffer_{};
    SecureDatagramBuffer<kMaxDatagramLen, ip_secure::SecureWrapper::kOverhead> secureBuffer_{};
};

} // namespace netip
} // namespace knx