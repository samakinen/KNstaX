// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/types.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

namespace knx {
namespace netip {

/**
 * @brief The server side of KNXnet/IP Tunnelling over UDP (03/08/04).
 *
 * Each connection gets its own Individual Address from
 * Options::tunnelAddresses, returned in the CRD of the CONNECT_RESPONSE
 * (03/08/04 §2.2.2, §5.4.4); there are as many connections as addresses.
 */
class TunnelingServerEndpoint {
public:
    /// TUNNELLING_REQUEST_TIMEOUT (03/08/04 §2.6.1).
    static constexpr uint32_t kTunnelingRequestTimeoutMs = 1000;
    /// "If the KNXnet/IP Server does not receive a heartbeat request within
    /// 120 seconds of the last correctly received message frame, the server
    /// shall terminate the connection" (03/08/02 §5.4).
    static constexpr uint32_t kConnectionAliveTimeoutMs = 120000;
    /// Outbound frames queued per connection while one awaits its ACK.
    static constexpr size_t kMaxQueuedFrames = 16;

    struct Options {
        NetIpPort port{NetIpPort(config::kDefaultPort)};
        /// Upper bound on concurrent connections; the effective number is
        /// also limited by tunnelAddresses.
        size_t maxChannels{4};
        uint32_t channelIdleTimeoutMs{kConnectionAliveTimeoutMs};
        /// One unique Individual Address per tunnelling connection.
        std::vector<IndividualAddress> tunnelAddresses{};
        /// Address advertised in the data endpoint HPAI of CONNECT_RESPONSE;
        /// zero means the network interface's own address.
        IpAddress dataEndpointAddress{IpAddress(0)};
    };

    struct ChannelInfo {
        ChannelId channelId{ChannelId::invalid()};
        IpAddress remoteAddress{IpAddress(0)};
        NetIpPort remotePort{NetIpPort::invalid()};
        /// The client's control and data endpoints (Route Back resolved).
        control_packet::HpaiEndpoint controlEndpoint{};
        control_packet::HpaiEndpoint dataEndpoint{};
        IndividualAddress tunnelAddress{IndividualAddress(0)};
        uint8_t expectedRxSequence{0};
        uint8_t nextTxSequence{0};
        uint32_t lastActivityMs{0};
    };

    using ReceiveCallback = util::InplaceFunction<void(ChannelId channelId, std::span<const uint8_t> cemi), 64>;

    TunnelingServerEndpoint();
    ~TunnelingServerEndpoint();

    TunnelingServerEndpoint(const TunnelingServerEndpoint&) = delete;
    TunnelingServerEndpoint& operator=(const TunnelingServerEndpoint&) = delete;

    util::Result<void> open(platform::NetworkInterface& network, const Options& options);
    void close();
    bool isOpen() const;

    void setTimingPlatform(platform::TimingPlatform* timingPlatform) noexcept { timingPlatform_ = timingPlatform; }
    platform::TimingPlatform* timingPlatform() const noexcept { return timingPlatform_; }

    void setReceiveCallback(ReceiveCallback callback);

    /// Receive and handle one datagram, and run the timers (ACK repetition,
    /// heartbeat supervision).
    util::Result<bool> poll(int timeoutMs = 0);

    /// Send a cEMI frame to one client in a TUNNELLING_REQUEST. Frames are
    /// sent one at a time: the next goes once the previous is acknowledged
    /// (03/08/04 §2.6.1); until then they queue.
    util::Result<void> sendCemi(ChannelId channelId, std::span<const uint8_t> cemi);
    util::Result<size_t> sendCemiToAll(std::span<const uint8_t> cemi);

    size_t activeChannelCount() const;
    std::vector<ChannelInfo> activeChannels() const;

private:
    struct ChannelSlot {
        bool active{false};
        ChannelInfo info{};
        /// The TUNNELLING_REQUEST awaiting its ACK, if any.
        std::vector<uint8_t> inFlight{};
        uint32_t inFlightSentMs{0};
        bool inFlightRepeated{false};
        std::deque<std::vector<uint8_t>> queue{};
    };

    uint32_t nowMs() const;
    void markChannelActivity(ChannelSlot& slot);
    void runTimers();

    ChannelSlot* findChannelById(ChannelId channelId);
    const ChannelSlot* findChannelById(ChannelId channelId) const;
    ChannelSlot* allocateChannel(std::span<const IndividualAddress> candidates);

    static void resolveEndpoint(control_packet::HpaiEndpoint& endpoint, IpAddress sourceAddress, NetIpPort sourcePort);

    util::Result<void> sendTo(const control_packet::HpaiEndpoint& endpoint, std::span<const uint8_t> datagram);
    util::Result<void> sendChannelStatusResponse(NetIpServiceType serviceType,
                                                 const control_packet::HpaiEndpoint& destination,
                                                 uint8_t channelId,
                                                 uint8_t status);
    util::Result<void> sendConnectResponse(const control_packet::HpaiEndpoint& destination,
                                           uint8_t channelId,
                                           uint8_t status,
                                           IndividualAddress tunnelAddress);
    util::Result<void> transmitInFlight(ChannelSlot& slot);
    void startNextQueued(ChannelSlot& slot);
    void terminate(ChannelSlot& slot, const char* reason);

    util::Result<void> handleConnectionRequest(std::span<const uint8_t> frame,
                                               IpAddress sourceAddress,
                                               NetIpPort sourcePort);
    util::Result<void> handleConnectionStateRequest(std::span<const uint8_t> frame,
                                                    IpAddress sourceAddress,
                                                    NetIpPort sourcePort);
    util::Result<void> handleDisconnectRequest(std::span<const uint8_t> frame,
                                               IpAddress sourceAddress,
                                               NetIpPort sourcePort);
    util::Result<void> handleTunnelingRequest(std::span<const uint8_t> frame,
                                              IpAddress sourceAddress,
                                              NetIpPort sourcePort);
    util::Result<void> handleTunnelingAck(std::span<const uint8_t> frame);

    std::unique_ptr<platform::UdpSocket> socket_;
    Options options_{};
    IpAddress localAddress_{IpAddress(0)};
    platform::TimingPlatform* timingPlatform_{nullptr};
    ReceiveCallback receiveCallback_{};

    std::vector<ChannelSlot> channels_{};

    DatagramBuffer<config::kUdpBufferSize> rxBuffer_{};
    DatagramBuffer<config::kUdpBufferSize> txBuffer_{};
};

} // namespace netip
} // namespace knx
