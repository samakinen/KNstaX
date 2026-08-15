// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/device_management_session.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/platform/linux_platform.hpp"

#if KNX_SECURE_ENABLED
#include "knx/netip/ip_secure/secure_session.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/netip_config.hpp"

#include "../common/vec_file.hpp"
#endif

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

void writeHeader(std::vector<uint8_t>& buf, uint16_t service, uint16_t totalLen)
{
    std::array<uint8_t, knx::netip::KnxNetIpCodec::kHeaderLen> header{};
    auto result = knx::netip::KnxNetIpCodec::encodeHeader(
        knx::NetIpServiceType(service),
        totalLen - knx::netip::KnxNetIpCodec::kHeaderLen,
        header);
    if (result.isError()) return;
    buf.insert(buf.end(), header.begin(), header.end());
}

std::vector<uint8_t> buildFrame(uint16_t service, std::span<const uint8_t> payload)
{
    std::vector<uint8_t> frame;
    frame.reserve(knx::netip::KnxNetIpCodec::kHeaderLen + payload.size());
    writeHeader(frame,
                service,
                static_cast<uint16_t>(knx::netip::KnxNetIpCodec::kHeaderLen + payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

int runPlainWorkflowTest()
{
    int srv = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (srv < 0) return 1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(0);
    if (::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(srv);
        return 2;
    }
    socklen_t alen = sizeof(addr);
    ::getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &alen);
    const uint16_t port = ntohs(addr.sin_port);

    std::atomic<bool> running{true};
    std::atomic<bool> disconnectReceived{false};

    std::thread server([&]() {
        uint8_t buf[1500];
        while (running.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(srv, &rfds);
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = 200 * 1000;
            const int ready = ::select(srv + 1, &rfds, nullptr, nullptr, &tv);
            if (ready <= 0) continue;

            sockaddr_in src{};
            socklen_t slen = sizeof(src);
            const ssize_t n = ::recvfrom(srv, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&src), &slen);
            if (n < 6) continue;

            const uint16_t serviceType = static_cast<uint16_t>((static_cast<uint16_t>(buf[2]) << 8) | buf[3]);
            if (serviceType == 0x0205) {
                std::vector<uint8_t> resp;
                writeHeader(resp, 0x0206, 8);
                resp.push_back(0x01);
                resp.push_back(0x00);
                ::sendto(srv, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&src), slen);
            } else if (serviceType == 0x0209) {
                disconnectReceived.store(true);
                std::vector<uint8_t> resp;
                writeHeader(resp, 0x020A, 8);
                resp.push_back(0x01);
                resp.push_back(0x00);
                ::sendto(srv, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&src), slen);
            } else if (serviceType == 0x0310 && n >= 11) {
                const uint8_t requestCode = buf[10];
                std::vector<uint8_t> resp;
                if (n >= 17 && requestCode == knx::netip::device_management::kPropertyReadRequestCode) {
                    writeHeader(resp, 0x0311, 19);
                } else if (n >= 17) {
                    writeHeader(resp, 0x0311, 17);
                } else {
                    writeHeader(resp, 0x0311, 13);
                }
                if (n >= 17) {
                    resp.push_back(0x04);
                    resp.push_back(buf[7]);
                    resp.push_back(buf[8]);
                    resp.push_back(0x00);
                    resp.push_back(requestCode == knx::netip::device_management::kPropertyReadRequestCode
                                       ? knx::netip::device_management::kPropertyReadConfirmationCode
                                       : knx::netip::device_management::kPropertyWriteConfirmationCode);
                    resp.push_back(buf[11]);
                    resp.push_back(buf[12]);
                    resp.push_back(buf[13]);
                    resp.push_back(buf[14]);
                    resp.push_back(buf[15]);
                    resp.push_back(buf[16]);
                    if (requestCode == knx::netip::device_management::kPropertyReadRequestCode) {
                        resp.push_back(buf[14]);
                        resp.push_back(static_cast<uint8_t>(buf[14] + 1));
                    }
                } else {
                    resp.push_back(buf[6]);
                    resp.push_back(buf[7]);
                    resp.push_back(buf[8]);
                    resp.push_back(buf[9]);
                    resp.push_back(0xF5);
                    resp.push_back(0xAA);
                    resp.push_back(0x11);
                }
                ::sendto(srv, resp.data(), resp.size(), 0, reinterpret_cast<sockaddr*>(&src), slen);
            }
        }
    });

    knx::platform::LinuxPlatform platform;
    auto* network = platform.network();
    if (!network) {
        running.store(false);
        server.join();
        ::close(srv);
        return 3;
    }
    if (!network->init()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 4;
    }

    knx::netip::TunnelingDeviceManagementSession workflow;
    auto beginOpen = workflow.beginOpen(*network, knx::IpAddress::fromOctets(127, 0, 0, 1), knx::NetIpPort(port), 500);
    if (beginOpen.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 5;
    }
    if (workflow.activeSessionOperation() != knx::netip::TunnelingSessionClient::SessionOperationType::Open) {
        running.store(false);
        server.join();
        ::close(srv);
        return 6;
    }

    bool sawPending = false;
    bool opened = false;
    for (int attempt = 0; attempt < 200 && !opened; ++attempt) {
        auto progress = workflow.pollOpen();
        if (progress.isError()) {
            running.store(false);
            server.join();
            ::close(srv);
            return 7;
        }
        if (progress.value() == knx::util::OperationProgressState::Pending) sawPending = true;
        if (progress.value() == knx::util::OperationProgressState::Success) opened = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!opened ||
        !sawPending ||
        !workflow.isOpen() ||
        workflow.openState() != knx::netip::TunnelingDeviceManagementSession::OpenState::Ready) {
        running.store(false);
        server.join();
        ::close(srv);
        return 8;
    }
    if (!workflow.deviceManagement().isSessionBound()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 9;
    }

    knx::netip::device_management::PropertyAccessTarget target;
    target.objectType = knx::InterfaceObjectType::knxNetIpParameter();
    target.objectInstance = knx::InterfaceObjectInstance(0x01);
    target.propertyId = knx::application::PropertyID::DeviceAddress;
    target.elementCount = 1;
    target.startIndex = 1;

    std::array<uint8_t, 32> responseBuffer{};
    auto beginRead = workflow.beginReadProperty(target, responseBuffer, 500);
    if (beginRead.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 10;
    }
    if (workflow.activeOperation() != knx::netip::DeviceManagementClient::OperationType::PropertyRead) {
        running.store(false);
        server.join();
        ::close(srv);
        return 11;
    }

    knx::netip::device_management::PropertyReadConfirmationView confirmation{};
    bool readCompleted = false;
    for (int attempt = 0; attempt < 200 && !readCompleted; ++attempt) {
        auto progress = workflow.pollReadProperty(confirmation);
        if (progress.isError()) {
            running.store(false);
            server.join();
            ::close(srv);
            return 12;
        }
        if (progress.value() == knx::util::OperationProgressState::Success) readCompleted = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!readCompleted) {
        running.store(false);
        server.join();
        ::close(srv);
        return 13;
    }
    if (workflow.activeOperation() != knx::netip::DeviceManagementClient::OperationType::None) {
        running.store(false);
        server.join();
        ::close(srv);
        return 14;
    }
    if (confirmation.connection.channelId.value() != 0x01 || confirmation.data.size() != 2 ||
        confirmation.data[0] != static_cast<uint8_t>(knx::application::PropertyID::DeviceAddress) ||
        confirmation.data[1] != static_cast<uint8_t>(static_cast<uint8_t>(knx::application::PropertyID::DeviceAddress) + 1)) {
        running.store(false);
        server.join();
        ::close(srv);
        return 15;
    }

    const std::array<uint8_t, 5> rawRequestPayload = {0x04, 0x01, 0x01, 0x00, 0x99};
    std::array<uint8_t, 16> rawResponseBuffer{};
    auto beginRawExchange = workflow.beginConfigurationExchange(rawRequestPayload, rawResponseBuffer, 500);
    if (beginRawExchange.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 16;
    }
    if (workflow.activeOperation() != knx::netip::DeviceManagementClient::OperationType::ConfigurationExchange) {
        running.store(false);
        server.join();
        ::close(srv);
        return 17;
    }

    size_t rawResponseLength = 0;
    bool rawExchangeCompleted = false;
    for (int attempt = 0; attempt < 200 && !rawExchangeCompleted; ++attempt) {
        auto progress = workflow.pollConfigurationExchange(rawResponseLength);
        if (progress.isError()) {
            running.store(false);
            server.join();
            ::close(srv);
            return 18;
        }
        if (progress.value() == knx::util::OperationProgressState::Success) rawExchangeCompleted = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!rawExchangeCompleted || rawResponseLength != 7 ||
        rawResponseBuffer[0] != 0x04 || rawResponseBuffer[1] != 0x01 || rawResponseBuffer[4] != 0xF5) {
        running.store(false);
        server.join();
        ::close(srv);
        return 19;
    }
    if (workflow.activeOperation() != knx::netip::DeviceManagementClient::OperationType::None) {
        running.store(false);
        server.join();
        ::close(srv);
        return 20;
    }

    knx::netip::device_management::PropertyAccessTarget writeTarget = target;
    writeTarget.propertyId = knx::application::PropertyID::ProgMode;

    knx::netip::device_management::PropertyAccessTarget readPlanTarget = target;
    readPlanTarget.propertyId = knx::application::PropertyID::ManufacturerId;

    std::array<uint8_t, 2> writeData{0x55, 0xAA};
    size_t plannedRawResponseLength = 0;
    std::array<uint8_t, 16> plannedRawResponseBuffer{};
    std::array<uint8_t, 16> writeResponseBuffer{};
    std::array<uint8_t, 32> readPlanResponseBuffer{};
    knx::netip::device_management::PropertyWriteConfirmation writeConfirmation{};
    knx::netip::device_management::PropertyReadConfirmationView readPlanConfirmation{};
    std::array<knx::netip::TunnelingDeviceManagementSession::PlanStep, 3> planSteps = {
        knx::netip::TunnelingDeviceManagementSession::PlanStep{
            knx::netip::TunnelingDeviceManagementSession::ConfigurationExchangePlanStep{
                rawRequestPayload,
                plannedRawResponseBuffer,
                &plannedRawResponseLength,
                500,
            }},
        knx::netip::TunnelingDeviceManagementSession::PlanStep{
            knx::netip::TunnelingDeviceManagementSession::PropertyWritePlanStep{
                writeTarget,
                writeData,
                writeResponseBuffer,
                &writeConfirmation,
                500,
            }},
        knx::netip::TunnelingDeviceManagementSession::PlanStep{
            knx::netip::TunnelingDeviceManagementSession::PropertyReadPlanStep{
                readPlanTarget,
                readPlanResponseBuffer,
                &readPlanConfirmation,
                500,
            }},
    };

    auto beginPlan = workflow.beginPlan(planSteps);
    if (beginPlan.isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 100 + static_cast<int>(beginPlan.error());
    }
    const auto initialPlanStatus = workflow.planStatus();
    if (!initialPlanStatus.active ||
        initialPlanStatus.phase != knx::netip::TunnelingDeviceManagementSession::PlanPhase::Running ||
        initialPlanStatus.totalSteps != planSteps.size() ||
        initialPlanStatus.activeStepIndex != 0 || initialPlanStatus.completedSteps != 0 ||
        initialPlanStatus.activeOperation != knx::netip::DeviceManagementClient::OperationType::ConfigurationExchange ||
        initialPlanStatus.terminalError != knx::util::ErrorCode::Success) {
        running.store(false);
        server.join();
        ::close(srv);
        return 22;
    }

    bool planCompleted = false;
    bool sawPropertyWriteStep = false;
    bool sawPropertyReadStep = false;
    for (int attempt = 0; attempt < 200 && !planCompleted; ++attempt) {
        auto progress = workflow.pollPlan();
        if (progress.isError()) {
            running.store(false);
            server.join();
            ::close(srv);
            return 23;
        }
        const auto status = workflow.planStatus();
        if (status.active && status.activeStepIndex == 1 &&
            status.activeOperation == knx::netip::DeviceManagementClient::OperationType::PropertyWrite) {
            sawPropertyWriteStep = true;
        }
        if (status.active && status.activeStepIndex == 2 &&
            status.activeOperation == knx::netip::DeviceManagementClient::OperationType::PropertyRead) {
            sawPropertyReadStep = true;
        }
        if (progress.value() == knx::util::OperationProgressState::Success) {
            planCompleted = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!planCompleted || !sawPropertyWriteStep || !sawPropertyReadStep || workflow.isPlanPending()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 24;
    }
    const auto completedPlanStatus = workflow.planStatus();
    if (completedPlanStatus.active ||
        completedPlanStatus.phase != knx::netip::TunnelingDeviceManagementSession::PlanPhase::Success ||
        completedPlanStatus.totalSteps != planSteps.size() ||
        completedPlanStatus.completedSteps != planSteps.size() ||
        completedPlanStatus.activeStepIndex != planSteps.size() ||
        completedPlanStatus.activeOperation != knx::netip::DeviceManagementClient::OperationType::None ||
        completedPlanStatus.terminalError != knx::util::ErrorCode::Success) {
        running.store(false);
        server.join();
        ::close(srv);
        return 25;
    }
    if (plannedRawResponseLength != 7 || plannedRawResponseBuffer[0] != 0x04 ||
        plannedRawResponseBuffer[1] != 0x01 || plannedRawResponseBuffer[4] != 0xF5) {
        running.store(false);
        server.join();
        ::close(srv);
        return 21;
    }
    if (writeConfirmation.connection.sequenceCounter != 0x03 ||
        readPlanConfirmation.connection.sequenceCounter != 0x04) {
        running.store(false);
        server.join();
        ::close(srv);
        return 21;
    }
    if (readPlanConfirmation.data.size() != 2 ||
        readPlanConfirmation.data[0] != static_cast<uint8_t>(knx::application::PropertyID::ManufacturerId) ||
        readPlanConfirmation.data[1] != static_cast<uint8_t>(static_cast<uint8_t>(knx::application::PropertyID::ManufacturerId) + 1)) {
        running.store(false);
        server.join();
        ::close(srv);
        return 22;
    }

    std::array<uint8_t, 32> cancelledReadBuffer{};
    knx::netip::device_management::PropertyReadConfirmationView cancelledConfirmation{};
    std::array<knx::netip::TunnelingDeviceManagementSession::PlanStep, 1> cancelledPlanSteps = {
        knx::netip::TunnelingDeviceManagementSession::PlanStep{
            knx::netip::TunnelingDeviceManagementSession::PropertyReadPlanStep{
                target,
                cancelledReadBuffer,
                &cancelledConfirmation,
                500,
            }},
    };
    if (workflow.beginPlan(cancelledPlanSteps).isError()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 23;
    }
    workflow.cancelPlan();
    if (workflow.isPlanPending() || workflow.isOperationPending()) {
        running.store(false);
        server.join();
        ::close(srv);
        return 24;
    }
    const auto cancelledStatus = workflow.planStatus();
    if (cancelledStatus.active ||
        cancelledStatus.phase != knx::netip::TunnelingDeviceManagementSession::PlanPhase::Cancelled ||
        cancelledStatus.totalSteps != cancelledPlanSteps.size() ||
        cancelledStatus.completedSteps != 0 ||
        cancelledStatus.activeOperation != knx::netip::DeviceManagementClient::OperationType::None ||
        cancelledStatus.terminalError != knx::util::ErrorCode::Success) {
        running.store(false);
        server.join();
        ::close(srv);
        return 25;
    }
    auto cancelledProgress = workflow.pollPlan();
    if (!cancelledProgress.isError() || cancelledProgress.error() != knx::util::ErrorCode::OperationNotReady) {
        running.store(false);
        server.join();
        ::close(srv);
        return 26;
    }

    workflow.close();
    running.store(false);
    server.join();
    ::close(srv);
    return disconnectReceived.load() ? 0 : 27;
}

#if KNX_SECURE_ENABLED

using knx::netip::ip_secure::SecureSession;
using knx::netip::ip_secure::SecureWrapper;

bool splitFrames(std::span<const uint8_t> stream, std::vector<std::vector<uint8_t>>& out)
{
    out.clear();
    size_t offset = 0;
    while (offset + 6 <= stream.size()) {
        const uint16_t totalLen = static_cast<uint16_t>((static_cast<uint16_t>(stream[offset + 4]) << 8) | stream[offset + 5]);
        if (totalLen < 6 || offset + totalLen > stream.size()) return false;
        out.emplace_back(stream.begin() + static_cast<std::ptrdiff_t>(offset),
                         stream.begin() + static_cast<std::ptrdiff_t>(offset + totalLen));
        offset += totalLen;
    }
    return offset == stream.size();
}

bool toFixed16(std::span<const uint8_t> in, std::array<uint8_t, 16>& out)
{
    if (in.size() != out.size()) return false;
    for (size_t index = 0; index < out.size(); ++index) out[index] = in[index];
    return true;
}

bool toFixed6(std::span<const uint8_t> in, std::array<uint8_t, 6>& out)
{
    if (in.size() != out.size()) return false;
    for (size_t index = 0; index < out.size(); ++index) out[index] = in[index];
    return true;
}

uint64_t seq48FromBytes(std::span<const uint8_t> seq)
{
    if (seq.size() != 6) return 0;
    uint64_t value = 0;
    for (uint8_t byte : seq) {
        value = (value << 8) | static_cast<uint64_t>(byte);
    }
    return value & 0xFFFFFFFFFFFFULL;
}

struct SharedTcpState {
    std::vector<uint8_t> inbound;
    std::vector<uint8_t> outbound;
};

class DummyUdpSocket final : public knx::platform::UdpSocket {
public:
    knx::util::Result<void> open(uint16_t port = 0) override
    {
        open_ = true;
        port_ = port;
        return knx::util::Result<void>::ok();
    }

    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    knx::util::Result<void> joinMulticast(knx::IpAddress, knx::IpAddress) override { return knx::util::Result<void>::ok(); }
    void leaveMulticast(knx::IpAddress, knx::IpAddress) override {}
    knx::util::Result<void> setMulticastInterface(knx::IpAddress) override { return knx::util::Result<void>::ok(); }
    knx::util::Result<void> setMulticastLoopback(knx::platform::MulticastLoopbackMode) override { return knx::util::Result<void>::ok(); }
    knx::util::Result<void> setMulticastTtl(uint8_t) override { return knx::util::Result<void>::ok(); }
    int send(knx::IpAddress destAddr, uint16_t destPort, std::span<const uint8_t> data) override
    {
        lastDestAddr = destAddr;
        lastDestPort = destPort;
        lastSent.assign(data.begin(), data.end());
        return static_cast<int>(data.size());
    }

    int receive(std::span<uint8_t> buffer) override
    {
        knx::IpAddress addr(0);
        uint16_t port = 0;
        return receive(buffer, addr, port);
    }

    int receive(std::span<uint8_t> buffer, knx::IpAddress& srcAddr, uint16_t& srcPort) override
    {
        if (rx_.empty()) return -1;
        const auto& next = rx_.front();
        srcAddr = next.addr;
        srcPort = next.port;
        const size_t count = buffer.size() < next.bytes.size() ? buffer.size() : next.bytes.size();
        for (size_t index = 0; index < count; ++index) buffer[index] = next.bytes[index];
        rx_.erase(rx_.begin());
        return static_cast<int>(count);
    }

    size_t available() const override { return rx_.empty() ? 0 : rx_.front().bytes.size(); }
    uint16_t localPort() const override { return port_; }

    void pushRx(knx::IpAddress addr, uint16_t port, std::span<const uint8_t> bytes)
    {
        rx_.push_back(RxPacket{addr, port, std::vector<uint8_t>(bytes.begin(), bytes.end())});
    }

    knx::IpAddress lastDestAddr{knx::IpAddress(0)};
    uint16_t lastDestPort{0};
    std::vector<uint8_t> lastSent;

private:
    struct RxPacket {
        knx::IpAddress addr;
        uint16_t port;
        std::vector<uint8_t> bytes;
    };

    bool open_{false};
    uint16_t port_{0};
    std::vector<RxPacket> rx_;
};

class ScriptedTcpSocket final : public knx::platform::TcpSocket {
public:
    explicit ScriptedTcpSocket(std::shared_ptr<SharedTcpState> state) : state_(std::move(state)) {}

    knx::util::Result<void> connect(knx::IpAddress, uint16_t) override
    {
        open_ = true;
        return knx::util::Result<void>::ok();
    }

    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    int send(std::span<const uint8_t> data) override
    {
        if (!open_) return -1;
        state_->outbound.insert(state_->outbound.end(), data.begin(), data.end());
        return static_cast<int>(data.size());
    }

    int receive(std::span<uint8_t> buffer) override
    {
        if (!open_) return -1;
        if (state_->inbound.empty()) return 0;

        const size_t count = buffer.size() < state_->inbound.size() ? buffer.size() : state_->inbound.size();
        for (size_t index = 0; index < count; ++index) buffer[index] = state_->inbound[index];
        state_->inbound.erase(state_->inbound.begin(), state_->inbound.begin() + static_cast<std::ptrdiff_t>(count));
        return static_cast<int>(count);
    }

    size_t available() const override { return state_->inbound.size(); }
    uint16_t localPort() const override { return 12345; }
    knx::IpAddress localAddress() const override { return knx::IpAddress::fromString("127.0.0.1"); }

private:
    std::shared_ptr<SharedTcpState> state_;
    bool open_{false};
};

class MockNetworkInterface final : public knx::platform::NetworkInterface {
public:
    explicit MockNetworkInterface(std::shared_ptr<SharedTcpState> state) : state_(std::move(state)) {}

    knx::util::Result<void> init() override { return knx::util::Result<void>::ok(); }
    bool isConnected() const override { return true; }
    knx::IpAddress ipAddress() const override { return knx::IpAddress(0); }
    knx::IpAddress subnetMask() const override { return knx::IpAddress(0); }
    knx::IpAddress gateway() const override { return knx::IpAddress(0); }
    void macAddress(std::span<uint8_t> mac) const override
    {
        for (uint8_t& byte : mac) byte = 0;
    }

    std::unique_ptr<knx::platform::UdpSocket> createUdpSocket() override
    {
        auto socket = std::make_unique<DummyUdpSocket>();
        udpSocket = socket.get();
        return socket;
    }

    std::unique_ptr<knx::platform::TcpSocket> createTcpSocket() override
    {
        return std::make_unique<ScriptedTcpSocket>(state_);
    }

private:
    std::shared_ptr<SharedTcpState> state_;

public:
    DummyUdpSocket* udpSocket{nullptr};
};

int runSecureWorkflowTest()
{
    std::string reqText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0951_request.vec", reqText)) return 30;
    std::map<std::string, std::string> reqKv;
    if (!knx_test::vec::parseVec(reqText, reqKv)) return 31;

    std::string respText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0952_response.vec", respText)) return 32;
    std::map<std::string, std::string> respKv;
    if (!knx_test::vec::parseVec(respText, respKv)) return 33;

    std::string authPlainText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_plain.vec", authPlainText)) return 34;
    std::map<std::string, std::string> authPlainKv;
    if (!knx_test::vec::parseVec(authPlainText, authPlainKv)) return 35;

    std::string authWrappedText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_wrapped.vec", authWrappedText)) return 36;
    std::map<std::string, std::string> authWrappedKv;
    if (!knx_test::vec::parseVec(authWrappedText, authWrappedKv)) return 37;

    std::vector<uint8_t> reqFrameExpected;
    if (!knx_test::vec::getHex(reqKv, "frame", reqFrameExpected)) return 38;

    std::vector<uint8_t> clientPrivBytes;
    if (!knx_test::vec::getHex(reqKv, "client_priv", clientPrivBytes) || clientPrivBytes.size() != 32) return 39;

    std::vector<uint8_t> respFrame;
    if (!knx_test::vec::getHex(respKv, "frame", respFrame)) return 40;

    std::vector<uint8_t> userIdBytes;
    std::vector<uint8_t> passwordLatin1;
    if (!knx_test::vec::getHex(authPlainKv, "user_id", userIdBytes)) return 41;
    if (!knx_test::vec::getHex(authPlainKv, "password_latin1", passwordLatin1)) return 42;
    if (userIdBytes.size() != 1) return 43;

    std::vector<uint8_t> authWrappedExpected;
    if (!knx_test::vec::getHex(authWrappedKv, "frame", authWrappedExpected)) return 44;

    std::vector<uint8_t> sessionKeyBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "session_key", sessionKeyBytes)) return 45;
    std::array<uint8_t, 16> sessionKey{};
    if (!toFixed16(sessionKeyBytes, sessionKey)) return 46;

    std::vector<uint8_t> sidBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "sid", sidBytes) || sidBytes.size() != 2) return 47;
    const knx::SessionId sid(static_cast<uint16_t>((static_cast<uint16_t>(sidBytes[0]) << 8) | sidBytes[1]));

    std::vector<uint8_t> seqBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "seq", seqBytes)) return 48;
    const uint64_t initialSeq = seq48FromBytes(seqBytes);
    if (initialSeq == 0) return 49;

    std::vector<uint8_t> serialBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "serial", serialBytes)) return 50;
    std::array<uint8_t, 6> clientSerial{};
    if (!toFixed6(serialBytes, clientSerial)) return 51;

    std::array<uint8_t, SecureSession::kSessionStatusFrameLen> statusPlain{};
    if (SecureSession::encodeSessionStatus(0x00, statusPlain).isError()) return 52;

    SecureWrapper::Key wrapperKey{};
    for (size_t index = 0; index < wrapperKey.size(); ++index) wrapperKey[index] = sessionKey[index];

    const std::array<uint8_t, SecureWrapper::kTagLen> tag{0x00, 0x00};
    const std::array<uint8_t, 4> counterSuffix{0x00, 0x00, 0xFF, 0x00};

    std::vector<uint8_t> statusWrapped(SecureWrapper::kOverhead + statusPlain.size());
    auto statusWrap = SecureWrapper::wrap(wrapperKey,
                                          sid,
                                          std::array<uint8_t, 6>{0x00, 0x00, 0x00, 0x00, 0x00, 0x02},
                                          std::array<uint8_t, 6>{0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f},
                                          tag,
                                          counterSuffix,
                                          statusPlain,
                                          statusWrapped);
    if (statusWrap.isError()) return 53;
    statusWrapped.resize(statusWrap.value());

    std::vector<uint8_t> connectRespPlain = {
        0x06, 0x10, 0x02, 0x06, 0x00, 0x10, 0x11, 0x00,
        0x08, 0x02, 127, 0, 0, 1, 0x1F, 0x90,
    };

    std::vector<uint8_t> connectRespWrapped(SecureWrapper::kOverhead + connectRespPlain.size());
    auto connectWrap = SecureWrapper::wrap(wrapperKey,
                                           sid,
                                           std::array<uint8_t, 6>{0x00, 0x00, 0x00, 0x00, 0x00, 0x02},
                                           std::array<uint8_t, 6>{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
                                           tag,
                                           counterSuffix,
                                           connectRespPlain,
                                           connectRespWrapped);
    if (connectWrap.isError()) return 54;
    connectRespWrapped.resize(connectWrap.value());

    auto shared = std::make_shared<SharedTcpState>();
    shared->inbound.insert(shared->inbound.end(), respFrame.begin(), respFrame.end());
    shared->inbound.insert(shared->inbound.end(), statusWrapped.begin(), statusWrapped.end());
    shared->inbound.insert(shared->inbound.end(), connectRespWrapped.begin(), connectRespWrapped.end());

    MockNetworkInterface network(shared);

    knx::netip::ip_secure::SecureTunnelingClient::Options options;
    options.host = knx::IpAddress::fromOctets(127, 0, 0, 1);
    options.port = knx::NetIpPort(knx::netip::config::kDefaultPort);
    options.userId = knx::UserId(userIdBytes[0]);
    options.passwordLatin1 = passwordLatin1;
    for (size_t index = 0; index < 32; ++index) options.clientPrivateKey[index] = clientPrivBytes[index];
    options.clientSerial = clientSerial;
    options.initialSeq = initialSeq;

    knx::netip::SecureTunnelingDeviceManagementSession workflow;
    auto beginOpen = workflow.beginOpen(network, options, 200, 200);
    if (beginOpen.isError()) return 55;
    if (workflow.activeSessionOperation() !=
        knx::netip::ip_secure::SecureTunnelingClient::SessionOperationType::OpenSecureSession) {
        return 56;
    }

    bool sawPending = false;
    bool opened = false;
    for (int attempt = 0; attempt < 80 && !opened; ++attempt) {
        auto progress = workflow.pollOpen();
        if (progress.isError()) return 57;
        if (progress.value() == knx::util::OperationProgressState::Pending) sawPending = true;
        if (progress.value() == knx::util::OperationProgressState::Success) opened = true;
    }

    if (!opened || !sawPending || !workflow.isOpen() || !workflow.deviceManagement().isSessionBound()) return 58;
    if (!workflow.session().isTunnelingConnected()) return 59;
    if (workflow.activeSessionOperation() !=
        knx::netip::ip_secure::SecureTunnelingClient::SessionOperationType::None) {
        return 60;
    }
    if (!network.udpSocket || !network.udpSocket->isOpen()) return 61;

    std::vector<std::vector<uint8_t>> frames;
    if (!splitFrames(shared->outbound, frames)) return 62;
    if (frames.size() != 3) return 63;
    if (frames[0] != reqFrameExpected) return 64;
    if (frames[1] != authWrappedExpected) return 65;

    knx::netip::device_management::PropertyAccessTarget writeTarget;
    writeTarget.objectType = knx::InterfaceObjectType::knxNetIpParameter();
    writeTarget.objectInstance = knx::InterfaceObjectInstance(0x01);
    writeTarget.propertyId = knx::application::PropertyID::ProgMode;
    writeTarget.elementCount = 1;
    writeTarget.startIndex = 1;

    knx::netip::device_management::PropertyAccessTarget readTarget = writeTarget;
    readTarget.propertyId = knx::application::PropertyID::DeviceAddress;

    const std::array<uint8_t, 11> writeResponsePayload = {
        0x04,
        0x11,
        0x00,
        0x00,
        knx::netip::device_management::kPropertyWriteConfirmationCode,
        0x00,
        0x0B,
        0x01,
        static_cast<uint8_t>(knx::application::PropertyID::ProgMode),
        0x10,
        0x01,
    };
    const std::array<uint8_t, 13> readResponsePayload = {
        0x04,
        0x11,
        0x01,
        0x00,
        knx::netip::device_management::kPropertyReadConfirmationCode,
        0x00,
        0x0B,
        0x01,
        static_cast<uint8_t>(knx::application::PropertyID::DeviceAddress),
        0x10,
        0x01,
        0x21,
        0x22,
    };
    const auto writeFrame = buildFrame(0x0311, writeResponsePayload);
    const auto readFrame = buildFrame(0x0311, readResponsePayload);

    std::vector<uint8_t> writeWrapped(SecureWrapper::kOverhead + writeFrame.size());
    auto wrappedWriteResult = SecureWrapper::wrap(wrapperKey,
                                                  sid,
                                                  std::array<uint8_t, 6>{0x00, 0x00, 0x00, 0x00, 0x00, 0x03},
                                                  std::array<uint8_t, 6>{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01},
                                                  tag,
                                                  counterSuffix,
                                                  writeFrame,
                                                  writeWrapped);
    if (wrappedWriteResult.isError()) return 66;
    writeWrapped.resize(wrappedWriteResult.value());

    std::vector<uint8_t> readWrapped(SecureWrapper::kOverhead + readFrame.size());
    auto wrappedReadResult = SecureWrapper::wrap(wrapperKey,
                                                 sid,
                                                 std::array<uint8_t, 6>{0x00, 0x00, 0x00, 0x00, 0x00, 0x04},
                                                 std::array<uint8_t, 6>{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x02},
                                                 tag,
                                                 counterSuffix,
                                                 readFrame,
                                                 readWrapped);
    if (wrappedReadResult.isError()) return 67;
    readWrapped.resize(wrappedReadResult.value());

    network.udpSocket->pushRx(options.host, options.port.value(), writeWrapped);
    network.udpSocket->pushRx(options.host, options.port.value(), readWrapped);

    std::array<uint8_t, 1> writeData{0x01};
    std::array<uint8_t, 16> writeResponseBuffer{};
    std::array<uint8_t, 32> readResponseBuffer{};
    knx::netip::device_management::PropertyWriteConfirmation writeConfirmation{};
    knx::netip::device_management::PropertyReadConfirmationView readConfirmation{};
    std::array<knx::netip::SecureTunnelingDeviceManagementSession::PlanStep, 2> planSteps = {
        knx::netip::SecureTunnelingDeviceManagementSession::PlanStep{
            knx::netip::SecureTunnelingDeviceManagementSession::PropertyWritePlanStep{
                writeTarget,
                writeData,
                writeResponseBuffer,
                &writeConfirmation,
                200,
            }},
        knx::netip::SecureTunnelingDeviceManagementSession::PlanStep{
            knx::netip::SecureTunnelingDeviceManagementSession::PropertyReadPlanStep{
                readTarget,
                readResponseBuffer,
                &readConfirmation,
                200,
            }},
    };
    auto executePlanResult = workflow.executePlan(planSteps);
    if (executePlanResult.isError()) return 68;
    const auto securePlanStatus = workflow.planStatus();
    if (securePlanStatus.active ||
        securePlanStatus.phase != knx::netip::SecureTunnelingDeviceManagementSession::PlanPhase::Success ||
        securePlanStatus.totalSteps != planSteps.size() ||
        securePlanStatus.completedSteps != planSteps.size() ||
        securePlanStatus.activeStepIndex != planSteps.size() ||
        securePlanStatus.activeOperation != knx::netip::DeviceManagementClient::OperationType::None ||
        securePlanStatus.terminalError != knx::util::ErrorCode::Success) {
        return 69;
    }
    if (writeConfirmation.connection.sequenceCounter != 0x00 ||
        readConfirmation.connection.sequenceCounter != 0x01) {
        return 70;
    }
    if (readConfirmation.data.size() != 2 || readConfirmation.data[0] != 0x21 || readConfirmation.data[1] != 0x22) {
        return 71;
    }
    if (network.udpSocket->lastDestAddr != options.host || network.udpSocket->lastDestPort != options.port.value() ||
        network.udpSocket->lastSent.empty()) {
        return 72;
    }

    workflow.close();
    return 0;
}

#endif

} // namespace

int main()
{
    const int plainResult = runPlainWorkflowTest();
    if (plainResult != 0) return plainResult;

#if KNX_SECURE_ENABLED
    const int secureResult = runSecureWorkflowTest();
    if (secureResult != 0) return secureResult;
#endif

    return 0;
}