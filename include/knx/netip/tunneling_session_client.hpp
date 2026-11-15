// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/config.hpp"
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
    /// TUNNELLING_REQUEST_TIMEOUT (03/08/04 §2.6.1).
    static constexpr int kTunnelingRequestTimeoutMs = 1000;
    /// CONNECTIONSTATE_REQUEST_TIMEOUT and the number of repetitions before
    /// the client gives up on the server (03/08/02 §5.4).
    static constexpr int kConnectionStateRequestTimeoutMs = 10000;
    static constexpr int kHeartbeatRepetitions = 3;

    using ReceiveCallback = util::InplaceFunction<void(std::span<const uint8_t>), ::knx::config::callbackBytes(64)>;

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

    /**
     * @brief One heartbeat (03/08/02 §5.4): a CONNECTIONSTATE_REQUEST, repeated
     * up to three times on timeout or an error status, then DISCONNECT_REQUEST
     * and close. The keepalive runs this every interval.
     */
    util::Result<void> heartbeat(int attemptTimeoutMs = kConnectionStateRequestTimeoutMs);
    void startKeepalive(uint32_t intervalMs = config::kTunnelingKeepaliveIntervalMs);
    void stopKeepalive();
    bool isKeepaliveActive() const;

    /**
     * @brief Send one cEMI frame in a TUNNELLING_REQUEST (03/08/04 §2.6.1).
     *
     * With @p waitAck, a request not acknowledged within @p timeoutMs, or
     * acknowledged with an error status, is repeated once with the same
     * sequence counter; if that fails too the connection is terminated with
     * a DISCONNECT_REQUEST. The sequence counter advances only once the
     * request is confirmed (or at once without @p waitAck).
     */
    util::Result<void> sendCemi(std::span<const uint8_t> cemi, bool waitAck = true,
                                int timeoutMs = kTunnelingRequestTimeoutMs);
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
    /// The Individual Address the server assigned to this connection (the
    /// CRD of the CONNECT_RESPONSE, 03/08/04 §5.4.4); 0 when not connected.
    uint16_t tunnelAddress() const noexcept { return tunnelAddress_; }

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
    util::Result<void> sendTunnelingRequest(uint8_t sequence, std::span<const uint8_t> cemi);
    void resetConnectionState() noexcept;
    util::Result<bool> receiveOnce(int timeoutMs);
    uint32_t getCurrentTimeMs() const noexcept;

    static constexpr size_t kMaxDatagramLen = config::kUdpBufferSize;

    std::unique_ptr<platform::UdpSocket> sock_;
    ChannelId channelId_{ChannelId::invalid()};
    TunnelingSequence seq_{TunnelingSequence(0)};
    ReceiveCallback rxCb_{};
    RemoteInfo remote_{};
    /// The server's data endpoint from the CONNECT_RESPONSE, where
    /// TUNNELLING_REQUEST and TUNNELLING_ACK go (03/08/02 §7.8.2).
    RemoteInfo dataEndpoint_{};
    /// Sequence counter expected on the next TUNNELLING_REQUEST from the server.
    TunnelingSequence rxSeq_{TunnelingSequence(0)};
    uint16_t tunnelAddress_{0};
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
