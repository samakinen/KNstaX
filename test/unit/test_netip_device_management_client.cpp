// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/device_management.hpp"
#include "knx/netip/device_management_codec.hpp"
#include "knx/netip/netip_config.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <span>
#include <thread>
#include <vector>

using namespace knx;
using namespace knx::netip;
namespace dm = knx::netip::device_management;

void setUp(void) {}
void tearDown(void) {}

namespace {

class PrefixSecurity final : public NetIpSecurity {
public:
    util::Result<size_t> protect(std::span<const uint8_t> in, std::span<uint8_t> out) override
    {
        if (out.size() < in.size() + 3) return util::ErrorCode::BufferTooSmall;
        out[0] = 'S';
        out[1] = 'E';
        out[2] = 'C';
        std::memcpy(out.data() + 3, in.data(), in.size());
        return in.size() + 3;
    }

    util::Result<size_t> unprotect(std::span<const uint8_t> in, std::span<uint8_t> out) override
    {
        if (in.size() < 3 || in[0] != 'S' || in[1] != 'E' || in[2] != 'C') return util::ErrorCode::DecodeFailed;
        if (out.size() < in.size() - 3) return util::ErrorCode::BufferTooSmall;
        std::memcpy(out.data(), in.data() + 3, in.size() - 3);
        return in.size() - 3;
    }
};

class TestUdpSocket final : public platform::UdpSocket {
public:
    util::Result<void> open(uint16_t port = 0) override
    {
        open_ = true;
        port_ = port;
        return util::Result<void>::ok();
    }

    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    util::Result<void> joinMulticast(IpAddress, IpAddress) override { return util::Result<void>::ok(); }
    void leaveMulticast(IpAddress, IpAddress) override {}
    util::Result<void> setMulticastInterface(IpAddress) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastLoopback(platform::MulticastLoopbackMode) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastTtl(uint8_t) override { return util::Result<void>::ok(); }

    int send(IpAddress destAddr, uint16_t destPort, std::span<const uint8_t> data) override
    {
        lastDestAddr = destAddr;
        lastDestPort = destPort;
        lastSent.assign(data.begin(), data.end());
        return static_cast<int>(data.size());
    }

    int receive(std::span<uint8_t> buffer) override
    {
        IpAddress addr(0);
        uint16_t port = 0;
        return receive(buffer, addr, port);
    }

    int receive(std::span<uint8_t> buffer, IpAddress& srcAddr, uint16_t& srcPort) override
    {
        if (rx_.empty()) return -1;
        const auto& next = rx_.front();
        srcAddr = next.addr;
        srcPort = next.port;
        const size_t n = std::min(buffer.size(), next.bytes.size());
        std::memcpy(buffer.data(), next.bytes.data(), n);
        rx_.pop_front();
        return static_cast<int>(n);
    }

    size_t available() const override { return rx_.empty() ? 0 : rx_.front().bytes.size(); }
    uint16_t localPort() const override { return port_; }

    void pushRx(IpAddress addr, uint16_t port, std::span<const uint8_t> bytes)
    {
        rx_.push_back(RxPacket{addr, port, std::vector<uint8_t>(bytes.begin(), bytes.end())});
    }

    IpAddress lastDestAddr{IpAddress(0)};
    uint16_t lastDestPort{0};
    std::vector<uint8_t> lastSent;

private:
    struct RxPacket {
        IpAddress addr;
        uint16_t port;
        std::vector<uint8_t> bytes;
    };

    bool open_{false};
    uint16_t port_{0};
    std::deque<RxPacket> rx_;
};

class TestNetwork final : public platform::NetworkInterface {
public:
    util::Result<void> init() override { return util::Result<void>::ok(); }
    bool isConnected() const override { return true; }
    IpAddress ipAddress() const override { return IpAddress::fromOctets(192, 168, 1, 2); }
    IpAddress subnetMask() const override { return IpAddress::fromOctets(255, 255, 255, 0); }
    IpAddress gateway() const override { return IpAddress::fromOctets(192, 168, 1, 1); }
    void macAddress(std::span<uint8_t> mac) const override
    {
        std::fill(mac.begin(), mac.end(), 0);
    }

    std::unique_ptr<platform::UdpSocket> createUdpSocket() override
    {
        auto socket = std::make_unique<TestUdpSocket>();
        sock = socket.get();
        return socket;
    }

    TestUdpSocket* sock{nullptr};
};

std::vector<uint8_t> buildFrame(NetIpServiceType serviceType, std::span<const uint8_t> payload)
{
    std::vector<uint8_t> frame(KnxNetIpCodec::kHeaderLen + payload.size());
    auto encoded = KnxNetIpCodec::encodeHeader(
        serviceType,
        payload.size(),
        std::span<uint8_t, KnxNetIpCodec::kHeaderLen>(frame.data(), KnxNetIpCodec::kHeaderLen));
    if (encoded.isError()) return {};
    std::copy(payload.begin(), payload.end(), frame.begin() + KnxNetIpCodec::kHeaderLen);
    return frame;
}

} // namespace

void test_device_management_client_exchanges_configuration_ack_payload(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(192, 168, 1, 10);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 32> responsePayload{};
    std::array<uint8_t, 16> responseBuffer{};

    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setConnection(ChannelId(0x15), 0x02);
    const std::array<uint8_t, 13> confirmationPayload = {
        0x04,
        0x15,
        0x02,
        0x00,
        dm::kPropertyReadConfirmationCode,
        0x00,
        0x0B,
        0x01,
        static_cast<uint8_t>(application::PropertyID::DeviceAddress),
        0x10,
        0x01,
        0xAA,
        0xBB,
    };
    auto responseFrame = buildFrame(NetIpServiceType(0x0311), confirmationPayload);
    TEST_ASSERT_FALSE(responseFrame.empty());
    network.sock->pushRx(host, port.value(), responseFrame);

    auto beginResult = client.beginReadProperty(target, responsePayload, 10);
    TEST_ASSERT_TRUE(beginResult.isOk());
    TEST_ASSERT_TRUE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::PropertyRead),
                            static_cast<uint8_t>(client.activeOperation()));

    dm::PropertyReadConfirmationView result{};
    auto progress = client.pollReadProperty(result);
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(util::OperationProgressState::Success),
                            static_cast<uint8_t>(progress.value()));
    TEST_ASSERT_EQUAL_UINT32(host.raw, network.sock->lastDestAddr.raw);
    TEST_ASSERT_EQUAL_UINT16(port.value(), network.sock->lastDestPort);
    TEST_ASSERT_EQUAL_UINT8(0x15, result.connection.channelId.value());
    TEST_ASSERT_EQUAL_UINT8(0x02, result.connection.sequenceCounter);
    TEST_ASSERT_EQUAL_UINT8(0x03, client.connection().sequenceCounter);
    TEST_ASSERT_EQUAL_UINT32(2, result.data.size());
    TEST_ASSERT_EQUAL_UINT8(0xAA, result.data[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, result.data[1]);
    TEST_ASSERT_FALSE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::None),
                            static_cast<uint8_t>(client.activeOperation()));
}

void test_device_management_client_rejects_unexpected_device_management_response(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(10, 0, 0, 50);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 16> responseBuffer{};

    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setConnection(ChannelId(0x21), 0x01);
    const std::array<uint8_t, 2> wrongResponsePayload = {0x21, 0x43};
    auto responseFrame = buildFrame(NetIpServiceType(0x0313), wrongResponsePayload);
    TEST_ASSERT_FALSE(responseFrame.empty());
    network.sock->pushRx(host, port.value(), responseFrame);

    auto result = client.readProperty(target, responseBuffer, 10);
    TEST_ASSERT_TRUE(result.isError());
}

void test_device_management_client_async_read_reports_pending_then_timeout(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(10, 20, 30, 40);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 16> responseBuffer{};

    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setConnection(ChannelId(0x15), 0x02);

    auto beginResult = client.beginReadProperty(target, responseBuffer, 20);
    TEST_ASSERT_TRUE(beginResult.isOk());
    TEST_ASSERT_TRUE(client.isOperationPending());

    dm::PropertyReadConfirmationView confirmation{};
    auto firstPoll = client.pollReadProperty(confirmation);
    TEST_ASSERT_TRUE(firstPoll.isOk());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(util::OperationProgressState::Pending),
                            static_cast<uint8_t>(firstPoll.value()));

    std::this_thread::sleep_for(std::chrono::milliseconds(25));

    auto timeoutPoll = client.pollReadProperty(confirmation);
    TEST_ASSERT_TRUE(timeoutPoll.isOk());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(util::OperationProgressState::Timeout),
                            static_cast<uint8_t>(timeoutPoll.value()));
    TEST_ASSERT_FALSE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(0x02, client.connection().sequenceCounter);
}

void test_device_management_client_async_read_rejects_overlapping_operations(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(192, 168, 10, 5);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 16> responseBuffer{};

    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setConnection(ChannelId(0x44), 0x07);

    auto beginResult = client.beginReadProperty(target, responseBuffer, 20);
    TEST_ASSERT_TRUE(beginResult.isOk());
    TEST_ASSERT_TRUE(client.isOperationPending());

    auto overlappingBegin = client.beginReadProperty(target, responseBuffer, 20);
    TEST_ASSERT_TRUE(overlappingBegin.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::Busy, overlappingBegin.error());

    dm::PropertyReadConfirmationView confirmation{};
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    auto timeoutPoll = client.pollReadProperty(confirmation);
    TEST_ASSERT_TRUE(timeoutPoll.isOk());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(util::OperationProgressState::Timeout),
                            static_cast<uint8_t>(timeoutPoll.value()));
}

void test_device_management_client_cancel_resets_pending_operation_state(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(172, 16, 0, 9);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 16> responseBuffer{};

    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setConnection(ChannelId(0x30), 0x09);
    TEST_ASSERT_TRUE(client.beginReadProperty(target, responseBuffer, 100).isOk());
    TEST_ASSERT_TRUE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::PropertyRead),
                            static_cast<uint8_t>(client.activeOperation()));

    client.cancelOperation();

    TEST_ASSERT_FALSE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::None),
                            static_cast<uint8_t>(client.activeOperation()));

    dm::PropertyReadConfirmationView confirmation{};
    auto pollResult = client.pollReadProperty(confirmation);
    TEST_ASSERT_TRUE(pollResult.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::OperationNotReady, pollResult.error());

    auto restartResult = client.beginReadProperty(target, responseBuffer, 10);
    TEST_ASSERT_TRUE(restartResult.isOk());
}

void test_device_management_client_binding_change_cancels_pending_operation(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(172, 16, 0, 10);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 16> responseBuffer{};

    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setConnection(ChannelId(0x31), 0x04);
    TEST_ASSERT_TRUE(client.beginWriteProperty(target, std::array<uint8_t, 2>{0x12, 0x34}, responseBuffer, 100).isOk());
    TEST_ASSERT_TRUE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::PropertyWrite),
                            static_cast<uint8_t>(client.activeOperation()));

    client.clearConnection();

    TEST_ASSERT_FALSE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::None),
                            static_cast<uint8_t>(client.activeOperation()));
    TEST_ASSERT_FALSE(client.hasConnection());
}

void test_secure_device_management_client_secures_configuration_exchange(void)
{
    TestNetwork network;
    PrefixSecurity security;
    SecureDeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(127, 0, 0, 1);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 32> responseBuffer{};
    std::array<uint8_t, 32> secureFrameBuffer{};

    const std::array<uint8_t, 2> propertyData = {0x55, 0x66};
    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setNetIpSecurity(&security);
    client.setConnection(ChannelId(0x32), 0x03);

    const std::array<uint8_t, 11> responsePayload = {
        0x04,
        0x32,
        0x03,
        0x00,
        dm::kPropertyWriteConfirmationCode,
        0x00,
        0x0B,
        0x01,
        static_cast<uint8_t>(application::PropertyID::DeviceAddress),
        0x10,
        0x01,
    };
    auto responseFrame = buildFrame(NetIpServiceType(0x0311), responsePayload);
    TEST_ASSERT_FALSE(responseFrame.empty());
    auto protectedLen = security.protect(responseFrame, secureFrameBuffer);
    TEST_ASSERT_TRUE(protectedLen.isOk());
    network.sock->pushRx(host,
                         port.value(),
                         std::span<const uint8_t>(secureFrameBuffer.data(), protectedLen.value()));

    auto beginResult = client.beginWriteProperty(target, propertyData, responseBuffer, 10);
    TEST_ASSERT_TRUE(beginResult.isOk());
    TEST_ASSERT_TRUE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::PropertyWrite),
                            static_cast<uint8_t>(client.activeOperation()));

    dm::PropertyWriteConfirmation result{};
    auto progress = client.pollWriteProperty(result);
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(util::OperationProgressState::Success),
                            static_cast<uint8_t>(progress.value()));
    TEST_ASSERT_EQUAL_UINT8('S', network.sock->lastSent[0]);
    TEST_ASSERT_EQUAL_UINT8('E', network.sock->lastSent[1]);
    TEST_ASSERT_EQUAL_UINT8('C', network.sock->lastSent[2]);
    TEST_ASSERT_EQUAL_UINT8(0x32, result.connection.channelId.value());
    TEST_ASSERT_EQUAL_UINT8(0x03, result.connection.sequenceCounter);
    TEST_ASSERT_EQUAL_UINT8(0x04, client.connection().sequenceCounter);
    TEST_ASSERT_FALSE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::None),
                            static_cast<uint8_t>(client.activeOperation()));
}

void test_device_management_client_exposes_raw_configuration_exchange(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(192, 168, 1, 10);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    const std::array<uint8_t, 5> requestPayload = {0x04, 0x15, 0x02, 0x00, 0x99};
    std::array<uint8_t, 16> responsePayload{};

    const std::array<uint8_t, 7> confirmationPayload = {0x04, 0x15, 0x02, 0x00, 0x55, 0xAA, 0x11};

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setConnection(ChannelId(0x15), 0x02);
    auto responseFrame = buildFrame(NetIpServiceType(0x0311), confirmationPayload);
    TEST_ASSERT_FALSE(responseFrame.empty());
    network.sock->pushRx(host, port.value(), responseFrame);

    auto beginResult = client.beginConfigurationExchange(requestPayload, responsePayload, 10);
    TEST_ASSERT_TRUE(beginResult.isOk());
    TEST_ASSERT_TRUE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::ConfigurationExchange),
                            static_cast<uint8_t>(client.activeOperation()));

    size_t responseLength = 0;
    auto progress = client.pollConfigurationExchange(responseLength);
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(util::OperationProgressState::Success),
                            static_cast<uint8_t>(progress.value()));
    TEST_ASSERT_EQUAL_UINT32(requestPayload.size() + KnxNetIpCodec::kHeaderLen, network.sock->lastSent.size());
    for (size_t i = 0; i < requestPayload.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(requestPayload[i], network.sock->lastSent[KnxNetIpCodec::kHeaderLen + i]);
    }
    TEST_ASSERT_EQUAL_UINT32(confirmationPayload.size(), responseLength);
    for (size_t i = 0; i < responseLength; ++i) {
        TEST_ASSERT_EQUAL_UINT8(confirmationPayload[i], responsePayload[i]);
    }
    TEST_ASSERT_FALSE(client.isOperationPending());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(DeviceManagementClient::OperationType::None),
                            static_cast<uint8_t>(client.activeOperation()));
    TEST_ASSERT_EQUAL_UINT8(0x03, client.connection().sequenceCounter);
}

void test_device_management_codec_encodes_and_decodes_property_read_frames(void)
{
    std::array<uint8_t, 16> payload{};
    dm::PropertyReadRequest request;
    request.connection.channelId = ChannelId(0x15);
    request.connection.sequenceCounter = 0x02;
    request.target.objectType = InterfaceObjectType::knxNetIpParameter();
    request.target.objectInstance = InterfaceObjectInstance(0x01);
    request.target.propertyId = application::PropertyID::DeviceAddress;
    request.target.elementCount = 1;
    request.target.startIndex = 1;

    auto encodeResult = dm::encodePropertyReadRequest(request, payload);
    TEST_ASSERT_TRUE(encodeResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(11, encodeResult.value());
    TEST_ASSERT_EQUAL_UINT8(0x04, payload[0]);
    TEST_ASSERT_EQUAL_UINT8(0x15, payload[1]);
    TEST_ASSERT_EQUAL_UINT8(0x02, payload[2]);
    TEST_ASSERT_EQUAL_UINT8(dm::kPropertyReadRequestCode, payload[4]);
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[5]);
    TEST_ASSERT_EQUAL_UINT8(0x0B, payload[6]);
    TEST_ASSERT_EQUAL_UINT8(0x01, payload[7]);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(application::PropertyID::DeviceAddress), payload[8]);
    TEST_ASSERT_EQUAL_UINT8(0x10, payload[9]);
    TEST_ASSERT_EQUAL_UINT8(0x01, payload[10]);

    const std::array<uint8_t, 13> responsePayload = {
        0x04,
        0x15,
        0x02,
        0x00,
        dm::kPropertyReadConfirmationCode,
        0x00,
        0x0B,
        0x01,
        static_cast<uint8_t>(application::PropertyID::DeviceAddress),
        0x10,
        0x01,
        0x11,
        0x22,
    };
    auto decodeResult = dm::decodePropertyReadConfirmation(responsePayload);
    TEST_ASSERT_TRUE(decodeResult.isOk());
    TEST_ASSERT_EQUAL_UINT8(0x15, decodeResult.value().connection.channelId.value());
    TEST_ASSERT_EQUAL_UINT8(0x02, decodeResult.value().connection.sequenceCounter);
    TEST_ASSERT_EQUAL_UINT16(InterfaceObjectType::knxNetIpParameter().value(), decodeResult.value().target.objectType.value());
    TEST_ASSERT_EQUAL_UINT8(0x01, decodeResult.value().target.objectInstance.value());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(application::PropertyID::DeviceAddress), static_cast<uint8_t>(decodeResult.value().target.propertyId));
    TEST_ASSERT_EQUAL_UINT8(1, decodeResult.value().target.elementCount);
    TEST_ASSERT_EQUAL_UINT16(1, decodeResult.value().target.startIndex);
    TEST_ASSERT_EQUAL_UINT32(2, decodeResult.value().data.size());
    TEST_ASSERT_EQUAL_UINT8(0x11, decodeResult.value().data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x22, decodeResult.value().data[1]);
}

void test_device_management_client_sends_semantic_property_read_request(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(192, 168, 1, 10);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 16> requestPayload{};
    std::array<uint8_t, 32> responseBuffer{};

    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;
    dm::PropertyReadRequest request;
    request.connection.channelId = ChannelId(0x15);
    request.connection.sequenceCounter = 0x00;
    request.target = target;
    auto encodeRequest = dm::encodePropertyReadRequest(request, requestPayload);
    TEST_ASSERT_TRUE(encodeRequest.isOk());

    const std::array<uint8_t, 13> confirmationPayload = {
        0x04,
        0x15,
        0x00,
        0x00,
        dm::kPropertyReadConfirmationCode,
        0x00,
        0x0B,
        0x01,
        static_cast<uint8_t>(application::PropertyID::DeviceAddress),
        0x10,
        0x01,
        0x12,
        0x34,
    };

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setConnection(ChannelId(0x15), 0x00);
    auto responseFrame = buildFrame(NetIpServiceType(0x0311), confirmationPayload);
    TEST_ASSERT_FALSE(responseFrame.empty());
    network.sock->pushRx(host, port.value(), responseFrame);

    auto exchangeResult = client.readProperty(target, responseBuffer, 10);
    TEST_ASSERT_TRUE(exchangeResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(encodeRequest.value() + KnxNetIpCodec::kHeaderLen, network.sock->lastSent.size());
    for (size_t i = 0; i < encodeRequest.value(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(requestPayload[i], network.sock->lastSent[KnxNetIpCodec::kHeaderLen + i]);
    }
    TEST_ASSERT_EQUAL_UINT32(2, exchangeResult.value().data.size());
    TEST_ASSERT_EQUAL_UINT8(0x12, exchangeResult.value().data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34, exchangeResult.value().data[1]);
}

void test_device_management_client_requires_bound_connection(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(192, 168, 1, 10);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 16> responseBuffer{};

    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    auto result = client.readProperty(target, responseBuffer, 10);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::NotInitialized, result.error());
}

void test_device_management_client_rejects_mismatched_response_sequence(void)
{
    TestNetwork network;
    DeviceManagementClient client;
    const IpAddress host = IpAddress::fromOctets(192, 168, 1, 10);
    const NetIpPort port(knx::netip::config::kDefaultPort);
    std::array<uint8_t, 16> responseBuffer{};

    dm::PropertyAccessTarget target;
    target.objectType = InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = InterfaceObjectInstance(0x01);
    target.propertyId = application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    TEST_ASSERT_TRUE(client.open(network, host, port).isOk());
    client.setConnection(ChannelId(0x15), 0x07);

    const std::array<uint8_t, 13> confirmationPayload = {
        0x04,
        0x15,
        0x08,
        0x00,
        dm::kPropertyReadConfirmationCode,
        0x00,
        0x0B,
        0x01,
        static_cast<uint8_t>(application::PropertyID::DeviceAddress),
        0x10,
        0x01,
        0x12,
        0x34,
    };

    auto responseFrame = buildFrame(NetIpServiceType(0x0311), confirmationPayload);
    TEST_ASSERT_FALSE(responseFrame.empty());
    network.sock->pushRx(host, port.value(), responseFrame);

    auto result = client.readProperty(target, responseBuffer, 10);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::OperationFailed, result.error());
    TEST_ASSERT_EQUAL_UINT8(0x07, client.connection().sequenceCounter);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_device_management_client_exchanges_configuration_ack_payload);
    RUN_TEST(test_device_management_client_rejects_unexpected_device_management_response);
    RUN_TEST(test_device_management_client_async_read_reports_pending_then_timeout);
    RUN_TEST(test_device_management_client_async_read_rejects_overlapping_operations);
    RUN_TEST(test_device_management_client_cancel_resets_pending_operation_state);
    RUN_TEST(test_device_management_client_binding_change_cancels_pending_operation);
    RUN_TEST(test_device_management_client_exposes_raw_configuration_exchange);
    RUN_TEST(test_secure_device_management_client_secures_configuration_exchange);
    RUN_TEST(test_device_management_codec_encodes_and_decodes_property_read_frames);
    RUN_TEST(test_device_management_client_sends_semantic_property_read_request);
    RUN_TEST(test_device_management_client_requires_bound_connection);
    RUN_TEST(test_device_management_client_rejects_mismatched_response_sequence);
    return UNITY_END();
}