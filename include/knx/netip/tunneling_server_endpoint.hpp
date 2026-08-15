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
#include <memory>
#include <span>
#include <vector>

namespace knx {
namespace netip {

class TunnelingServerEndpoint {
public:
    struct Options {
        NetIpPort port{NetIpPort(config::kDefaultPort)};
        size_t maxChannels{4};
        uint32_t channelIdleTimeoutMs{60000};
    };

    struct ChannelInfo {
        ChannelId channelId{ChannelId::invalid()};
        IpAddress remoteAddress{IpAddress(0)};
        NetIpPort remotePort{NetIpPort::invalid()};
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

    util::Result<bool> poll(int timeoutMs = 0);

    util::Result<void> sendCemi(ChannelId channelId, std::span<const uint8_t> cemi);
    util::Result<size_t> sendCemiToAll(std::span<const uint8_t> cemi);

    size_t activeChannelCount() const;
    std::vector<ChannelInfo> activeChannels() const;

private:
    struct ChannelSlot {
        bool active{false};
        ChannelInfo info{};
    };

    static constexpr uint8_t kStatusNoError = 0x00;
    static constexpr uint8_t kStatusNoMoreConnections = 0x24;
    static constexpr uint8_t kStatusInvalidConnection = 0x21;
    static constexpr uint8_t kStatusSequenceError = 0x04;

    uint32_t nowMs() const;
    void markChannelActivity(ChannelSlot& slot);
    void pruneIdleChannels();

    ChannelSlot* findChannelById(ChannelId channelId);
    const ChannelSlot* findChannelById(ChannelId channelId) const;
    ChannelSlot* findChannelByEndpoint(IpAddress address, NetIpPort port);
    ChannelSlot* allocateChannel(IpAddress address, NetIpPort port);

    util::Result<void> sendChannelStatusResponse(NetIpServiceType serviceType,
                                                 IpAddress destination,
                                                 NetIpPort destinationPort,
                                                 uint8_t channelId,
                                                 uint8_t status);

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

    std::unique_ptr<platform::UdpSocket> socket_;
    Options options_{};
    platform::TimingPlatform* timingPlatform_{nullptr};
    ReceiveCallback receiveCallback_{};

    std::vector<ChannelSlot> channels_{};

    DatagramBuffer<config::kUdpBufferSize> rxBuffer_{};
    DatagramBuffer<config::kUdpBufferSize> txBuffer_{};
};

} // namespace netip
} // namespace knx
