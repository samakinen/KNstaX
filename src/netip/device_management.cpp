// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/device_management.hpp"
#include "knx/netip/detail/polling.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/secure_udp_datagram_channel.hpp"
#include "knx/netip/udp_datagram_channel.hpp"

#include <algorithm>

namespace dm = knx::netip::device_management;

namespace knx {
namespace netip {

namespace {

constexpr NetIpServiceType kDeviceConfigurationRequestService{0x0310};
constexpr NetIpServiceType kDeviceConfigurationAckService{0x0311};

template <typename Confirmation, typename PollFn>
util::Result<Confirmation> waitForPropertyConfirmation(knx::platform::TimingPlatform* timingPlatform,
                                                       PollFn&& poll)
{
    while (true) {
        Confirmation confirmation{};
        auto progress = poll(confirmation);
        if (progress.isError()) return progress.error();

        switch (progress.value()) {
            case util::OperationProgressState::Success:
                return confirmation;
            case util::OperationProgressState::Pending:
            case util::OperationProgressState::Busy:
                detail::delayForNextPoll(timingPlatform);
                continue;
            case util::OperationProgressState::Timeout:
                return util::ErrorCode::Timeout;
            case util::OperationProgressState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
        }
    }
}

} // namespace

DeviceManagementClient::DeviceManagementClient() = default;

DeviceManagementClient::~DeviceManagementClient() { close(); }

void DeviceManagementClient::setConnection(ChannelId channelId, uint8_t nextSequenceCounter) noexcept
{
    cancelOperation();
    connection_.channelId = channelId;
    connection_.sequenceCounter = nextSequenceCounter;
}

void DeviceManagementClient::bindSession(DeviceManagementConnectionProvider provider) noexcept
{
    cancelOperation();
    sessionBinding_ = provider;
}

void DeviceManagementClient::unbindSession() noexcept
{
    cancelOperation();
    sessionBinding_ = {};
}

bool DeviceManagementClient::isSessionBound() const noexcept
{
    return sessionBinding_.isBound();
}

void DeviceManagementClient::clearConnection() noexcept
{
    cancelOperation();
    connection_ = {};
}

bool DeviceManagementClient::hasConnection() const noexcept
{
    return connection_.channelId.isValid();
}

dm::ConnectionHeader DeviceManagementClient::connection() const noexcept
{
    return connection_;
}

void DeviceManagementClient::cancelOperation() noexcept
{
    operation_.reset();
}

util::Result<dm::ConnectionHeader> DeviceManagementClient::makeRequestConnectionHeader() const
{
    if (isSessionBound()) {
        return sessionBinding_.acquireHeader();
    }
    if (!hasConnection()) return util::ErrorCode::NotInitialized;
    return connection_;
}

void DeviceManagementClient::advanceSequenceCounter() noexcept
{
    connection_.sequenceCounter = static_cast<uint8_t>(connection_.sequenceCounter + 1);
}

util::Result<dm::PropertyReadConfirmationView> DeviceManagementClient::readProperty(
    const dm::PropertyAccessTarget& target,
    std::span<uint8_t> responsePayload,
    int timeoutMs)
{
    auto beginResult = beginReadProperty(target, responsePayload, timeoutMs);
    if (beginResult.isError()) return beginResult.error();

    return waitForPropertyConfirmation<dm::PropertyReadConfirmationView>(
        timingPlatform_,
        [this](dm::PropertyReadConfirmationView& confirmation) {
            return pollReadProperty(confirmation);
        });
}

util::Result<dm::PropertyWriteConfirmation> DeviceManagementClient::writeProperty(
    const dm::PropertyAccessTarget& target,
    std::span<const uint8_t> data,
    std::span<uint8_t> responsePayload,
    int timeoutMs)
{
    auto beginResult = beginWriteProperty(target, data, responsePayload, timeoutMs);
    if (beginResult.isError()) return beginResult.error();

    return waitForPropertyConfirmation<dm::PropertyWriteConfirmation>(
        timingPlatform_,
        [this](dm::PropertyWriteConfirmation& confirmation) {
            return pollWriteProperty(confirmation);
        });
}

util::Result<void> DeviceManagementClient::beginReadProperty(
    const dm::PropertyAccessTarget& target,
    std::span<uint8_t> responsePayload,
    int timeoutMs)
{
    auto connectionHeader = makeRequestConnectionHeader();
    if (connectionHeader.isError()) return connectionHeader.error();

    const dm::PropertyReadRequest request{connectionHeader.value(), target};
    auto encodeResult = dm::encodePropertyReadRequest(request, requestBuffer().span());
    if (encodeResult.isError()) return encodeResult.error();

    return beginOperation(OperationType::PropertyRead,
                          std::span<const uint8_t>(requestBuffer().bytes).first(encodeResult.value()),
                          request.connection,
                          responsePayload,
                          timeoutMs);
}

util::Result<util::OperationProgressState> DeviceManagementClient::pollReadProperty(
    dm::PropertyReadConfirmationView& outConfirmation)
{
    if (operation_.kind != OperationType::PropertyRead || !operation_.active) {
        return util::ErrorCode::OperationNotReady;
    }

    size_t responseLength = 0;
    auto progress = pollOperation(responseLength);
    if (progress.isError()) return progress.error();
    if (progress.value() != util::OperationProgressState::Success) return progress.value();

    auto confirmation = dm::decodePropertyReadConfirmation(operation_.responsePayload.first(responseLength));
    if (confirmation.isError()) {
        operation_.reset();
        return confirmation.error();
    }
    if (confirmation.value().connection.channelId != operation_.requestConnection.channelId ||
        confirmation.value().connection.sequenceCounter != operation_.requestConnection.sequenceCounter) {
        operation_.reset();
        return util::ErrorCode::OperationFailed;
    }

    outConfirmation = confirmation.value();
    if (!isSessionBound()) {
        advanceSequenceCounter();
    }
    operation_.reset();
    return util::OperationProgressState::Success;
}

util::Result<void> DeviceManagementClient::beginWriteProperty(
    const dm::PropertyAccessTarget& target,
    std::span<const uint8_t> data,
    std::span<uint8_t> responsePayload,
    int timeoutMs)
{
    auto connectionHeader = makeRequestConnectionHeader();
    if (connectionHeader.isError()) return connectionHeader.error();

    const dm::PropertyWriteRequest request{connectionHeader.value(), target, data};
    auto encodeResult = dm::encodePropertyWriteRequest(request, requestBuffer().span());
    if (encodeResult.isError()) return encodeResult.error();

    return beginOperation(OperationType::PropertyWrite,
                          std::span<const uint8_t>(requestBuffer().bytes).first(encodeResult.value()),
                          request.connection,
                          responsePayload,
                          timeoutMs);
}

util::Result<util::OperationProgressState> DeviceManagementClient::pollWriteProperty(
    dm::PropertyWriteConfirmation& outConfirmation)
{
    if (operation_.kind != OperationType::PropertyWrite || !operation_.active) {
        return util::ErrorCode::OperationNotReady;
    }

    size_t responseLength = 0;
    auto progress = pollOperation(responseLength);
    if (progress.isError()) return progress.error();
    if (progress.value() != util::OperationProgressState::Success) return progress.value();

    auto confirmation = dm::decodePropertyWriteConfirmation(operation_.responsePayload.first(responseLength));
    if (confirmation.isError()) {
        operation_.reset();
        return confirmation.error();
    }
    if (confirmation.value().connection.channelId != operation_.requestConnection.channelId ||
        confirmation.value().connection.sequenceCounter != operation_.requestConnection.sequenceCounter) {
        operation_.reset();
        return util::ErrorCode::OperationFailed;
    }

    outConfirmation = confirmation.value();
    if (!isSessionBound()) {
        advanceSequenceCounter();
    }
    operation_.reset();
    return util::OperationProgressState::Success;
}

util::Result<void> DeviceManagementClient::beginConfigurationExchange(std::span<const uint8_t> requestPayload,
                                                                      std::span<uint8_t> responsePayload,
                                                                      int timeoutMs)
{
    auto connectionHeader = makeRequestConnectionHeader();
    if (connectionHeader.isError()) return connectionHeader.error();
    return beginOperation(OperationType::ConfigurationExchange,
                          requestPayload,
                          connectionHeader.value(),
                          responsePayload,
                          timeoutMs);
}

util::Result<util::OperationProgressState> DeviceManagementClient::pollConfigurationExchange(size_t& responseLength)
{
    if (operation_.kind != OperationType::ConfigurationExchange || !operation_.active) {
        return util::ErrorCode::OperationNotReady;
    }

    auto progress = pollOperation(responseLength);
    if (progress.isError()) return progress.error();
    if (progress.value() != util::OperationProgressState::Success) return progress.value();

    if (!isSessionBound()) {
        advanceSequenceCounter();
    }
    operation_.reset();
    return util::OperationProgressState::Success;
}

util::Result<size_t> DeviceManagementClient::configurationExchange(std::span<const uint8_t> requestPayload,
                                                                   std::span<uint8_t> responsePayload,
                                                                   int timeoutMs)
{
    auto beginResult = beginConfigurationExchange(requestPayload, responsePayload, timeoutMs);
    if (beginResult.isError()) return beginResult.error();

    while (true) {
        size_t responseLength = 0;
        auto progress = pollConfigurationExchange(responseLength);
        if (progress.isError()) return progress.error();

        switch (progress.value()) {
            case util::OperationProgressState::Success:
                return responseLength;
            case util::OperationProgressState::Pending:
            case util::OperationProgressState::Busy:
                detail::delayForNextPoll(timingPlatform_);
                continue;
            case util::OperationProgressState::Timeout:
                return util::ErrorCode::Timeout;
            case util::OperationProgressState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
        }
    }
}

util::Result<void> DeviceManagementClient::open(platform::NetworkInterface& network, IpAddress host, NetIpPort port) {
    close();

    sock_ = network.createUdpSocket();
    if (!sock_) return util::ErrorCode::ResourceUnavailable;

    auto openRes = sock_->open(0);
    if (openRes.isError()) {
        close();
        return openRes;
    }

    const IpAddress addr = host;
    if (addr.isZero()) {
        close();
        return util::ErrorCode::InvalidAddress;
    }

    remote_.addr = addr;
    remote_.port = port;
    return util::Result<void>::ok();
}

void DeviceManagementClient::close() {
    if (sock_) sock_->close();
    sock_.reset();
    remote_.addr = IpAddress(0);
    remote_.port = NetIpPort::invalid();
    unbindSession();
    clearConnection();
    cancelOperation();
}

bool DeviceManagementClient::isOpen() const noexcept { return sock_ && sock_->isOpen() && !remote_.addr.isZero() && remote_.port.isValid(); }

util::Result<void> DeviceManagementClient::beginOperation(OperationType kind,
                                                          std::span<const uint8_t> requestPayload,
                                                          dm::ConnectionHeader requestConnection,
                                                          std::span<uint8_t> responsePayload,
                                                          int timeoutMs)
{
    if (operation_.active) return util::ErrorCode::Busy;
    if (responsePayload.empty()) return util::ErrorCode::InvalidParameter;

    auto sendResult = sendConfigurationRequest(requestPayload);
    if (sendResult.isError()) return sendResult.error();

    operation_.active = true;
    operation_.kind = kind;
    operation_.startTimeMs = detail::nowMs(timingPlatform_);
    operation_.timeoutMs = timeoutMs;
    operation_.requestConnection = requestConnection;
    operation_.responsePayload = responsePayload;
    return util::Result<void>::ok();
}

util::Result<util::OperationProgressState> DeviceManagementClient::pollOperation(size_t& responseLength)
{
    if (!operation_.active) return util::ErrorCode::OperationNotReady;

    auto ready = isConfigurationResponseReady();
    if (ready.isError()) {
        operation_.reset();
        return ready.error();
    }
    if (!ready.value()) {
        if (detail::remainingTimeoutMs(timingPlatform_, operation_.startTimeMs, operation_.timeoutMs) <= 0) {
            operation_.reset();
            return util::OperationProgressState::Timeout;
        }
        return util::OperationProgressState::Pending;
    }

    auto receiveResult = receiveConfigurationResponse(operation_.responsePayload);
    if (receiveResult.isError()) {
        operation_.reset();
        return receiveResult.error();
    }

    responseLength = receiveResult.value();
    return util::OperationProgressState::Success;
}

util::Result<void> DeviceManagementClient::sendConfigurationRequest(std::span<const uint8_t> requestPayload)
{
    if (!isOpen()) return util::ErrorCode::NotInitialized;

    auto encodedHeader = KnxNetIpCodec::encodeHeader(kDeviceConfigurationRequestService,
                                                     requestPayload.size(),
                                                     std::span<uint8_t, KnxNetIpCodec::kHeaderLen>(frameBuffer().bytes.data(), KnxNetIpCodec::kHeaderLen));
    if (encodedHeader.isError()) return util::ErrorCode::InvalidFrameSize;

    std::copy(requestPayload.begin(), requestPayload.end(), frameBuffer().bytes.begin() + KnxNetIpCodec::kHeaderLen);
    const auto requestFrame = std::span<const uint8_t>(frameBuffer().bytes).first(encodedHeader.value());
    const int sent = socket()->send(remoteEndpoint().addr, remoteEndpoint().port.value(), requestFrame);
    if (sent != static_cast<int>(requestFrame.size())) return util::ErrorCode::TransmissionFailed;
    return util::Result<void>::ok();
}

util::Result<bool> DeviceManagementClient::isConfigurationResponseReady()
{
    if (!isOpen()) return util::ErrorCode::NotInitialized;
    return UdpDatagramChannel::waitReadable(*socket(), 0, timingPlatform_);
}

util::Result<size_t> DeviceManagementClient::receiveConfigurationResponse(std::span<uint8_t> ackPayload)
{
    if (!isOpen()) return util::ErrorCode::NotInitialized;

    IpAddress srcAddr(0);
    uint16_t srcPort = 0;
    const int received = socket()->receive(frameBuffer().span(), srcAddr, srcPort);
    if (received < 0) return util::ErrorCode::TransmissionFailed;
    if (srcAddr != remoteEndpoint().addr || srcPort != remoteEndpoint().port.value()) {
        return util::ErrorCode::OperationFailed;
    }

    auto decoded = KnxNetIpCodec::payloadSpan(frameBuffer().span().first(static_cast<size_t>(received)),
                                              kDeviceConfigurationAckService);
    if (decoded.isError()) return decoded.error();

    const auto response = decoded.value();
    if (response.size() > ackPayload.size()) return util::ErrorCode::BufferTooSmall;
    std::copy(response.begin(), response.end(), ackPayload.begin());
    return response.size();
}

util::Result<void> SecureDeviceManagementClient::sendConfigurationRequest(std::span<const uint8_t> requestPayload)
{
    if (!isOpen()) return util::ErrorCode::NotInitialized;
    if (!security_) return util::ErrorCode::NotInitialized;

    auto encodedHeader = KnxNetIpCodec::encodeHeader(kDeviceConfigurationRequestService,
                                                     requestPayload.size(),
                                                     std::span<uint8_t, KnxNetIpCodec::kHeaderLen>(frameBuffer().bytes.data(), KnxNetIpCodec::kHeaderLen));
    if (encodedHeader.isError()) return util::ErrorCode::InvalidFrameSize;

    std::copy(requestPayload.begin(), requestPayload.end(), frameBuffer().bytes.begin() + KnxNetIpCodec::kHeaderLen);

    SecureUdpDatagramChannel secureChannel(*socket(), *security_, secureBuffer_.span(), secureBuffer_.span());
    return secureChannel.send(remoteEndpoint(), std::span<const uint8_t>(frameBuffer().bytes).first(encodedHeader.value()));
}

util::Result<size_t> SecureDeviceManagementClient::receiveConfigurationResponse(std::span<uint8_t> ackPayload)
{
    if (!isOpen()) return util::ErrorCode::NotInitialized;
    if (!security_) return util::ErrorCode::NotInitialized;

    SecureUdpDatagramChannel secureChannel(*socket(), *security_, secureBuffer_.span(), secureBuffer_.span());
    IpAddress srcAddr(0);
    uint16_t srcPort = 0;
    auto receiveResult = secureChannel.receive(frameBuffer().span(), srcAddr, srcPort);
    if (receiveResult.isError()) return receiveResult.error();
    if (srcAddr != remoteEndpoint().addr || srcPort != remoteEndpoint().port.value()) {
        return util::ErrorCode::OperationFailed;
    }

    auto decoded = KnxNetIpCodec::payloadSpan(frameBuffer().span().first(receiveResult.value()),
                                              kDeviceConfigurationAckService);
    if (decoded.isError()) return decoded.error();

    const auto response = decoded.value();
    if (response.size() > ackPayload.size()) return util::ErrorCode::BufferTooSmall;
    std::copy(response.begin(), response.end(), ackPayload.begin());
    return response.size();
}

} // namespace netip
} // namespace knx
