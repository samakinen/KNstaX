// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/device_management_codec.hpp"
#include "knx/netip/device_management_connection_source.hpp"
#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/netip/netip_security.hpp"
#include "knx/netip/udp_datagram_channel.hpp"
#include "knx/types.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace knx {
namespace netip {

class DeviceManagementClient {
public:
    enum class OperationType : uint8_t {
        None = 0,
        ConfigurationExchange,
        PropertyRead,
        PropertyWrite,
    };

    DeviceManagementClient();
    virtual ~DeviceManagementClient();

    DeviceManagementClient(const DeviceManagementClient&) = delete;
    DeviceManagementClient& operator=(const DeviceManagementClient&) = delete;

    util::Result<void> open(platform::NetworkInterface& network, IpAddress host, NetIpPort port);
    void close();
    bool isOpen() const noexcept;

    void bindSession(DeviceManagementConnectionProvider provider) noexcept;
    template <typename Session>
    void bindSession(Session& session) noexcept
    {
        bindSession(session.deviceManagementConnectionProvider());
    }
    void unbindSession() noexcept;
    bool isSessionBound() const noexcept;

    void setConnection(ChannelId channelId, uint8_t nextSequenceCounter = 0) noexcept;
    void clearConnection() noexcept;
    bool hasConnection() const noexcept;
    device_management::ConnectionHeader connection() const noexcept;

    util::Result<void> beginReadProperty(const device_management::PropertyAccessTarget& target,
                                         std::span<uint8_t> responsePayload,
                                         int timeoutMs = 1000);
    util::Result<util::OperationProgressState> pollReadProperty(
        device_management::PropertyReadConfirmationView& outConfirmation);

    util::Result<device_management::PropertyReadConfirmationView> readProperty(
        const device_management::PropertyAccessTarget& target,
        std::span<uint8_t> responsePayload,
        int timeoutMs = 1000);

    util::Result<void> beginWriteProperty(const device_management::PropertyAccessTarget& target,
                                          std::span<const uint8_t> data,
                                          std::span<uint8_t> responsePayload,
                                          int timeoutMs = 1000);
    util::Result<util::OperationProgressState> pollWriteProperty(
        device_management::PropertyWriteConfirmation& outConfirmation);

    util::Result<device_management::PropertyWriteConfirmation> writeProperty(
        const device_management::PropertyAccessTarget& target,
        std::span<const uint8_t> data,
        std::span<uint8_t> responsePayload,
        int timeoutMs = 1000);

    util::Result<void> beginConfigurationExchange(std::span<const uint8_t> requestPayload,
                                                  std::span<uint8_t> responsePayload,
                                                  int timeoutMs = 1000);
    util::Result<util::OperationProgressState> pollConfigurationExchange(size_t& responseLength);
    util::Result<size_t> configurationExchange(std::span<const uint8_t> requestPayload,
                                               std::span<uint8_t> responsePayload,
                                               int timeoutMs = 1000);

    bool isOperationPending() const noexcept { return operation_.active; }
    OperationType activeOperation() const noexcept { return operation_.kind; }
    void cancelOperation() noexcept;
    void setTimingPlatform(platform::TimingPlatform* timingPlatform) noexcept { timingPlatform_ = timingPlatform; }
    platform::TimingPlatform* timingPlatform() const noexcept { return timingPlatform_; }


protected:
    static constexpr size_t kMaxDatagramLen = config::kDeviceManagementBufferSize;

    virtual util::Result<void> sendConfigurationRequest(std::span<const uint8_t> requestPayload);
    virtual util::Result<bool> isConfigurationResponseReady();
    virtual util::Result<size_t> receiveConfigurationResponse(std::span<uint8_t> ackPayload);

    const UdpDatagramEndpoint& remoteEndpoint() const noexcept { return remote_; }
    platform::UdpSocket* socket() const noexcept { return sock_.get(); }
    DatagramBuffer<kMaxDatagramLen>& frameBuffer() noexcept { return frameBuffer_; }
    const DatagramBuffer<kMaxDatagramLen>& frameBuffer() const noexcept { return frameBuffer_; }
    DatagramBuffer<kMaxDatagramLen>& requestBuffer() noexcept { return requestBuffer_; }

private:
    struct Operation {
        bool active{false};
        OperationType kind{OperationType::None};
        uint32_t startTimeMs{0};
        int timeoutMs{0};
        device_management::ConnectionHeader requestConnection{};
        std::span<uint8_t> responsePayload{};

        constexpr void reset() noexcept
        {
            active = false;
            kind = OperationType::None;
            startTimeMs = 0;
            timeoutMs = 0;
            requestConnection = {};
            responsePayload = {};
        }
    };

    util::Result<device_management::ConnectionHeader> makeRequestConnectionHeader() const;
    void advanceSequenceCounter() noexcept;
    util::Result<void> beginOperation(OperationType kind,
                                      std::span<const uint8_t> requestPayload,
                                      device_management::ConnectionHeader requestConnection,
                                      std::span<uint8_t> responsePayload,
                                      int timeoutMs);
    util::Result<util::OperationProgressState> pollOperation(size_t& responseLength);

    std::unique_ptr<platform::UdpSocket> sock_;
    UdpDatagramEndpoint remote_;
    device_management::ConnectionHeader connection_{};
    DeviceManagementConnectionProvider sessionBinding_{};
    Operation operation_{};
    platform::TimingPlatform* timingPlatform_{nullptr};
    DatagramBuffer<kMaxDatagramLen> frameBuffer_{};
    DatagramBuffer<kMaxDatagramLen> requestBuffer_{};
};

class SecureDeviceManagementClient final : public DeviceManagementClient {
public:
    void setNetIpSecurity(NetIpSecurity* security) noexcept { security_ = security; }

protected:
    util::Result<void> sendConfigurationRequest(std::span<const uint8_t> requestPayload) override;
    util::Result<size_t> receiveConfigurationResponse(std::span<uint8_t> ackPayload) override;

private:
    NetIpSecurity* security_{nullptr};
    SecureDatagramBuffer<kMaxDatagramLen, ip_secure::SecureWrapper::kOverhead> secureBuffer_{};
};

} // namespace netip
} // namespace knx
