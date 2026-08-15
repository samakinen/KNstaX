// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/device_management_procedures.hpp"
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
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

using knx::application::PropertyID;

void writeHeader(std::vector<uint8_t>& buffer, uint16_t service, uint16_t totalLen)
{
    std::array<uint8_t, knx::netip::KnxNetIpCodec::kHeaderLen> header{};
    auto result = knx::netip::KnxNetIpCodec::encodeHeader(
        knx::NetIpServiceType(service),
        totalLen - knx::netip::KnxNetIpCodec::kHeaderLen,
        header);
    if (result.isError()) return;
    buffer.insert(buffer.end(), header.begin(), header.end());
}

std::vector<uint8_t> buildDmAck(uint8_t channelId,
                                uint8_t sequenceCounter,
                                uint8_t messageCode,
                                PropertyID propertyId,
                                std::span<const uint8_t> data)
{
    std::vector<uint8_t> payload;
    payload.reserve(11 + data.size());
    payload.push_back(0x04);
    payload.push_back(channelId);
    payload.push_back(sequenceCounter);
    payload.push_back(0x00);
    payload.push_back(messageCode);
    payload.push_back(0x00);
    payload.push_back(0x0B);
    payload.push_back(0x01);
    payload.push_back(static_cast<uint8_t>(propertyId));
    payload.push_back(0x10);
    payload.push_back(0x01);
    payload.insert(payload.end(), data.begin(), data.end());

    std::vector<uint8_t> frame;
    writeHeader(frame,
                0x0311,
                static_cast<uint16_t>(knx::netip::KnxNetIpCodec::kHeaderLen + payload.size()));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

int runPlainProcedureTest()
{
    int serverSocket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (serverSocket < 0) return 1;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    if (::bind(serverSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(serverSocket);
        return 2;
    }

    socklen_t addressLength = sizeof(address);
    ::getsockname(serverSocket, reinterpret_cast<sockaddr*>(&address), &addressLength);
    const uint16_t port = ntohs(address.sin_port);

    std::atomic<bool> running{true};
    std::atomic<bool> disconnectReceived{false};
    uint8_t programmingMode = 0x00;
    uint8_t subnetByte = 0x11;
    uint8_t deviceByte = 0x0A;
    const std::array<uint8_t, 2> manufacturerId{0x12, 0x34};

    std::thread server([&]() {
        uint8_t buffer[1500];
        while (running.load()) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(serverSocket, &rfds);
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 200 * 1000;
            const int ready = ::select(serverSocket + 1, &rfds, nullptr, nullptr, &timeout);
            if (ready <= 0) continue;

            sockaddr_in src{};
            socklen_t srcLen = sizeof(src);
            const ssize_t received = ::recvfrom(serverSocket,
                                                buffer,
                                                sizeof(buffer),
                                                0,
                                                reinterpret_cast<sockaddr*>(&src),
                                                &srcLen);
            if (received < 6) continue;

            const uint16_t serviceType = static_cast<uint16_t>((static_cast<uint16_t>(buffer[2]) << 8) | buffer[3]);
            if (serviceType == 0x0205) {
                std::vector<uint8_t> response;
                writeHeader(response, 0x0206, 8);
                response.push_back(0x01);
                response.push_back(0x00);
                ::sendto(serverSocket, response.data(), response.size(), 0, reinterpret_cast<sockaddr*>(&src), srcLen);
            } else if (serviceType == 0x0209) {
                disconnectReceived.store(true);
                std::vector<uint8_t> response;
                writeHeader(response, 0x020A, 8);
                response.push_back(0x01);
                response.push_back(0x00);
                ::sendto(serverSocket, response.data(), response.size(), 0, reinterpret_cast<sockaddr*>(&src), srcLen);
            } else if (serviceType == 0x0310 && received >= 17) {
                const uint8_t channelId = buffer[7];
                const uint8_t sequenceCounter = buffer[8];
                const uint8_t messageCode = buffer[10];
                const auto propertyId = static_cast<PropertyID>(buffer[14]);
                std::vector<uint8_t> response;

                if (messageCode == knx::netip::device_management::kPropertyReadRequestCode) {
                    std::array<uint8_t, 2> twoByteData{};
                    std::array<uint8_t, 1> oneByteData{};
                    std::span<const uint8_t> payload{};
                    switch (propertyId) {
                        case PropertyID::ManufacturerId:
                            twoByteData = manufacturerId;
                            payload = twoByteData;
                            break;
                        case PropertyID::SubnetAddress:
                            oneByteData[0] = subnetByte;
                            payload = oneByteData;
                            break;
                        case PropertyID::DeviceAddress:
                            oneByteData[0] = deviceByte;
                            payload = oneByteData;
                            break;
                        case PropertyID::ProgMode:
                            oneByteData[0] = programmingMode;
                            payload = oneByteData;
                            break;
                        default:
                            break;
                    }
                    response = buildDmAck(channelId,
                                          sequenceCounter,
                                          knx::netip::device_management::kPropertyReadConfirmationCode,
                                          propertyId,
                                          payload);
                } else if (messageCode == knx::netip::device_management::kPropertyWriteRequestCode && received >= 18) {
                    const uint8_t value = buffer[17];
                    switch (propertyId) {
                        case PropertyID::ProgMode:
                            programmingMode = value;
                            break;
                        case PropertyID::SubnetAddress:
                            if (programmingMode != 0x00) subnetByte = value;
                            break;
                        case PropertyID::DeviceAddress:
                            if (programmingMode != 0x00) deviceByte = value;
                            break;
                        default:
                            break;
                    }
                    response = buildDmAck(channelId,
                                          sequenceCounter,
                                          knx::netip::device_management::kPropertyWriteConfirmationCode,
                                          propertyId,
                                          std::span<const uint8_t>());
                }

                if (!response.empty()) {
                    ::sendto(serverSocket, response.data(), response.size(), 0, reinterpret_cast<sockaddr*>(&src), srcLen);
                }
            }
        }
    });

    knx::platform::LinuxPlatform platform;
    auto* network = platform.network();
    if (!network) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 3;
    }
    auto initResult = network->init();
    if (initResult.isError()) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 4;
    }

    knx::netip::TunnelingDeviceManagementSession workflow;
    auto openResult = workflow.open(*network, knx::IpAddress::fromOctets(127, 0, 0, 1), knx::NetIpPort(port), 500);
    if (openResult.isError()) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 5;
    }

    auto procedures = knx::netip::makeDeviceManagementProcedures(workflow);
    auto identityResult = procedures.readDeviceIdentity(500);
    if (identityResult.isError()) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 6;
    }
    if (identityResult.value().manufacturerId.value() != 0x1234 ||
        identityResult.value().individualAddress.raw != 0x110A ||
        identityResult.value().programmingMode != knx::Toggle::Disable) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 7;
    }

    const auto readStatus = procedures.procedureStatus();
    if (readStatus.active ||
        readStatus.type != knx::netip::TunnelingDeviceManagementProcedures::ProcedureType::ReadDeviceIdentity ||
        readStatus.phase != knx::netip::TunnelingDeviceManagementSession::PlanPhase::Success ||
        readStatus.terminalError != knx::util::ErrorCode::Success) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 8;
    }

    auto programmingModeResult = procedures.readProgrammingMode(500);
    if (programmingModeResult.isError() || programmingModeResult.value() != knx::Toggle::Disable) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 9;
    }

    auto setProgrammingModeResult = procedures.setProgrammingMode(knx::Toggle::Enable, 500);
    if (setProgrammingModeResult.isError()) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 10;
    }
    if (setProgrammingModeResult.value().requestedMode != knx::Toggle::Enable ||
        setProgrammingModeResult.value().programmingModeBefore != knx::Toggle::Disable ||
        setProgrammingModeResult.value().programmingModeAfter != knx::Toggle::Enable ||
        !setProgrammingModeResult.value().changed ||
        programmingMode != 0x01) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 11;
    }

    const auto setProgrammingModeStatus = procedures.procedureStatus();
    if (setProgrammingModeStatus.active ||
        setProgrammingModeStatus.type != knx::netip::TunnelingDeviceManagementProcedures::ProcedureType::SetProgrammingMode ||
        setProgrammingModeStatus.phase != knx::netip::TunnelingDeviceManagementSession::PlanPhase::Success ||
        setProgrammingModeStatus.terminalError != knx::util::ErrorCode::Success) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 12;
    }

    auto assignResult = procedures.assignIndividualAddress(
        knx::IndividualAddress(0x1121),
        knx::netip::TunnelingDeviceManagementProcedures::AssignIndividualAddressOptions{
            knx::netip::TunnelingDeviceManagementProcedures::ProgrammingModeDisposition::Enable,
            500,
        });
    if (assignResult.isError()) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 13;
    }
    if (assignResult.value().assignedAddress.raw != 0x1121 ||
        assignResult.value().programmingModeBefore != knx::Toggle::Enable ||
        assignResult.value().programmingModeAfter != knx::Toggle::Enable) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 14;
    }
    if (subnetByte != 0x11 || deviceByte != 0x21 || programmingMode != 0x01) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 15;
    }

    const auto assignStatus = procedures.procedureStatus();
    if (assignStatus.active ||
        assignStatus.type != knx::netip::TunnelingDeviceManagementProcedures::ProcedureType::AssignIndividualAddress ||
        assignStatus.phase != knx::netip::TunnelingDeviceManagementSession::PlanPhase::Success ||
        assignStatus.terminalError != knx::util::ErrorCode::Success) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 16;
    }

    auto commissionResult = procedures.commissionIndividualAddress(
        knx::IndividualAddress(0x1133),
        knx::netip::TunnelingDeviceManagementProcedures::CommissionIndividualAddressOptions{
            knx::Toggle::Disable,
            true,
            500,
        });
    if (commissionResult.isError()) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 17;
    }
    if (commissionResult.value().identityBefore.individualAddress.raw != 0x1121 ||
        commissionResult.value().identityBefore.programmingMode != knx::Toggle::Enable ||
        commissionResult.value().identityAfter.individualAddress.raw != 0x1133 ||
        commissionResult.value().identityAfter.programmingMode != knx::Toggle::Disable ||
        !commissionResult.value().addressChanged ||
        !commissionResult.value().programmingModeChanged) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 18;
    }
    if (subnetByte != 0x11 || deviceByte != 0x33 || programmingMode != 0x00) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 19;
    }

    const auto commissionStatus = procedures.procedureStatus();
    if (commissionStatus.active ||
        commissionStatus.type != knx::netip::TunnelingDeviceManagementProcedures::ProcedureType::CommissionIndividualAddress ||
        commissionStatus.phase != knx::netip::TunnelingDeviceManagementSession::PlanPhase::Success ||
        commissionStatus.terminalError != knx::util::ErrorCode::Success) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 20;
    }

    auto updatedIdentity = procedures.readDeviceIdentity(500);
    if (updatedIdentity.isError() ||
        updatedIdentity.value().individualAddress.raw != 0x1133 ||
        updatedIdentity.value().programmingMode != knx::Toggle::Disable) {
        running.store(false);
        server.join();
        ::close(serverSocket);
        return 21;
    }

    workflow.close();
    running.store(false);
    server.join();
    ::close(serverSocket);
    return disconnectReceived.load() ? 0 : 22;
}

#if KNX_SECURE_ENABLED

using knx::netip::ip_secure::SecureSession;
using knx::netip::ip_secure::SecureWrapper;

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
    for (uint8_t byte : seq) value = (value << 8) | static_cast<uint64_t>(byte);
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

    DummyUdpSocket* udpSocket{nullptr};

private:
    std::shared_ptr<SharedTcpState> state_;
};

int runSecureProcedureTest()
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

    std::vector<uint8_t> clientPrivBytes;
    if (!knx_test::vec::getHex(reqKv, "client_priv", clientPrivBytes) || clientPrivBytes.size() != 32) return 38;

    std::vector<uint8_t> respFrame;
    if (!knx_test::vec::getHex(respKv, "frame", respFrame)) return 39;

    std::vector<uint8_t> userIdBytes;
    std::vector<uint8_t> passwordLatin1;
    if (!knx_test::vec::getHex(authPlainKv, "user_id", userIdBytes)) return 40;
    if (!knx_test::vec::getHex(authPlainKv, "password_latin1", passwordLatin1)) return 41;
    if (userIdBytes.size() != 1) return 42;

    std::vector<uint8_t> sessionKeyBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "session_key", sessionKeyBytes)) return 43;
    std::array<uint8_t, 16> sessionKey{};
    if (!toFixed16(sessionKeyBytes, sessionKey)) return 44;

    std::vector<uint8_t> sidBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "sid", sidBytes) || sidBytes.size() != 2) return 45;
    const knx::SessionId sid(static_cast<uint16_t>((static_cast<uint16_t>(sidBytes[0]) << 8) | sidBytes[1]));

    std::vector<uint8_t> seqBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "seq", seqBytes)) return 46;
    const uint64_t initialSeq = seq48FromBytes(seqBytes);
    if (initialSeq == 0) return 47;

    std::vector<uint8_t> serialBytes;
    if (!knx_test::vec::getHex(authWrappedKv, "serial", serialBytes)) return 48;
    std::array<uint8_t, 6> clientSerial{};
    if (!toFixed6(serialBytes, clientSerial)) return 49;

    std::array<uint8_t, SecureSession::kSessionStatusFrameLen> statusPlain{};
    if (SecureSession::encodeSessionStatus(0x00, statusPlain).isError()) return 50;

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
    if (statusWrap.isError()) return 51;
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
    if (connectWrap.isError()) return 52;
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
    auto openResult = workflow.open(network, options, 200, 200);
    if (openResult.isError()) return 53;
    if (!network.udpSocket) return 54;

    const auto manufacturerAck = buildDmAck(0x11,
                                            0x00,
                                            knx::netip::device_management::kPropertyReadConfirmationCode,
                                            PropertyID::ManufacturerId,
                                            std::array<uint8_t, 2>{0x12, 0x34});
    const auto subnetAck = buildDmAck(0x11,
                                      0x01,
                                      knx::netip::device_management::kPropertyReadConfirmationCode,
                                      PropertyID::SubnetAddress,
                                      std::array<uint8_t, 1>{0x11});
    const auto deviceAck = buildDmAck(0x11,
                                      0x02,
                                      knx::netip::device_management::kPropertyReadConfirmationCode,
                                      PropertyID::DeviceAddress,
                                      std::array<uint8_t, 1>{0x21});
    const auto progModeAck = buildDmAck(0x11,
                                        0x03,
                                        knx::netip::device_management::kPropertyReadConfirmationCode,
                                        PropertyID::ProgMode,
                                        std::array<uint8_t, 1>{0x01});

    auto pushWrappedAck = [&](const std::vector<uint8_t>& frame,
                              const std::array<uint8_t, 6>& sequence,
                              const std::array<uint8_t, 6>& serial) -> bool {
        std::vector<uint8_t> wrapped(SecureWrapper::kOverhead + frame.size());
        auto wrapResult = SecureWrapper::wrap(wrapperKey, sid, sequence, serial, tag, counterSuffix, frame, wrapped);
        if (wrapResult.isError()) return false;
        wrapped.resize(wrapResult.value());
        network.udpSocket->pushRx(options.host, options.port.value(), wrapped);
        return true;
    };

    if (!pushWrappedAck(manufacturerAck, {0x00, 0x00, 0x00, 0x00, 0x00, 0x03}, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x01})) return 55;
    if (!pushWrappedAck(subnetAck, {0x00, 0x00, 0x00, 0x00, 0x00, 0x04}, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x02})) return 56;
    if (!pushWrappedAck(deviceAck, {0x00, 0x00, 0x00, 0x00, 0x00, 0x05}, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x03})) return 57;
    if (!pushWrappedAck(progModeAck, {0x00, 0x00, 0x00, 0x00, 0x00, 0x06}, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x04})) return 58;

    auto procedures = knx::netip::makeDeviceManagementProcedures(workflow);
    auto identityResult = procedures.readDeviceIdentity(200);
    if (identityResult.isError()) return 59;
    if (identityResult.value().manufacturerId.value() != 0x1234 ||
        identityResult.value().individualAddress.raw != 0x1121 ||
        identityResult.value().programmingMode != knx::Toggle::Enable) {
        return 60;
    }

    const auto status = procedures.procedureStatus();
    if (status.active ||
        status.type != knx::netip::SecureTunnelingDeviceManagementProcedures::ProcedureType::ReadDeviceIdentity ||
        status.phase != knx::netip::SecureTunnelingDeviceManagementSession::PlanPhase::Success ||
        status.terminalError != knx::util::ErrorCode::Success) {
        return 61;
    }

    const auto progModeReadAck = buildDmAck(0x11,
                                            0x04,
                                            knx::netip::device_management::kPropertyReadConfirmationCode,
                                            PropertyID::ProgMode,
                                            std::array<uint8_t, 1>{0x01});
    const auto progModeWriteAck = buildDmAck(0x11,
                                             0x05,
                                             knx::netip::device_management::kPropertyWriteConfirmationCode,
                                             PropertyID::ProgMode,
                                             std::span<const uint8_t>());
    const auto progModeVerifyAck = buildDmAck(0x11,
                                              0x06,
                                              knx::netip::device_management::kPropertyReadConfirmationCode,
                                              PropertyID::ProgMode,
                                              std::array<uint8_t, 1>{0x00});

    if (!pushWrappedAck(progModeReadAck, {0x00, 0x00, 0x00, 0x00, 0x00, 0x07}, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x05})) return 62;
    if (!pushWrappedAck(progModeWriteAck, {0x00, 0x00, 0x00, 0x00, 0x00, 0x08}, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x06})) return 63;
    if (!pushWrappedAck(progModeVerifyAck, {0x00, 0x00, 0x00, 0x00, 0x00, 0x09}, {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0x07})) return 64;

    auto setProgrammingModeResult = procedures.setProgrammingMode(knx::Toggle::Disable, 200);
    if (setProgrammingModeResult.isError()) return 65;
    if (setProgrammingModeResult.value().requestedMode != knx::Toggle::Disable ||
        setProgrammingModeResult.value().programmingModeBefore != knx::Toggle::Enable ||
        setProgrammingModeResult.value().programmingModeAfter != knx::Toggle::Disable ||
        !setProgrammingModeResult.value().changed) {
        return 66;
    }

    auto verifiedProgrammingMode = procedures.readProgrammingMode(200);
    if (verifiedProgrammingMode.isError() || verifiedProgrammingMode.value() != knx::Toggle::Disable) {
        return 67;
    }

    const auto setStatus = procedures.procedureStatus();
    if (setStatus.active ||
        setStatus.type != knx::netip::SecureTunnelingDeviceManagementProcedures::ProcedureType::ReadProgrammingMode ||
        setStatus.phase != knx::netip::SecureTunnelingDeviceManagementSession::PlanPhase::Success ||
        setStatus.terminalError != knx::util::ErrorCode::Success) {
        return 68;
    }

    if (network.udpSocket->lastDestAddr != options.host || network.udpSocket->lastDestPort != options.port.value()) {
        return 69;
    }

    workflow.close();
    return 0;
}

#endif

} // namespace

int main()
{
    const int plainResult = runPlainProcedureTest();
    if (plainResult != 0) return plainResult;

#if KNX_SECURE_ENABLED
    const int secureResult = runSecureProcedureTest();
    if (secureResult != 0) return secureResult;
#endif

    return 0;
}