// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/application/application_layer.hpp"
#include "knx/application/device_descriptor.hpp"
#include "knx/application/authorization_service.hpp"
#include "knx/transport/transport_layer.hpp"
#include "knx/network/network_layer.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/platform/linux_platform.hpp"
#include "../mocks/mock_physical_layer.hpp"
#include "unity.h"
#include <memory>

using knx::IndividualAddress;
using knx::GroupAddress;
using knx::application::APCIService;
using knx::application::ApplicationLayer;
using knx::application::DeviceDescriptorType0;
using knx::MediumType;
using knx::application::AuthorizationKey;
using namespace knx::test;

using namespace knx::test;

// Test fixtures
static std::unique_ptr<MockPhysicalLayer> physicalLayer;
static std::unique_ptr<knx::platform::LinuxPlatform> platform;
static std::unique_ptr<knx::datalink::Tp1DataLinkLayer> dlLayer;
static std::unique_ptr<knx::network::NetworkLayer> nwLayer;
static std::unique_ptr<knx::transport::TransportLayer> tpLayer;
static std::unique_ptr<ApplicationLayer> appLayer;

void setUp(void) {
    platform = std::make_unique<knx::platform::LinuxPlatform>();
    physicalLayer = std::make_unique<MockPhysicalLayer>();
    dlLayer = std::make_unique<knx::datalink::Tp1DataLinkLayer>(*platform, *physicalLayer);
    nwLayer = std::make_unique<knx::network::NetworkLayer>(*dlLayer);
    tpLayer = std::make_unique<knx::transport::TransportLayer>(*nwLayer);
    appLayer = std::make_unique<ApplicationLayer>(*tpLayer);
    
    IndividualAddress ownAddr(1, 1, 1);
    TEST_ASSERT_TRUE(dlLayer->init(ownAddr).isOk());
    TEST_ASSERT_TRUE(nwLayer->init(ownAddr).isOk());
    TEST_ASSERT_TRUE(tpLayer->init(ownAddr).isOk());
    TEST_ASSERT_TRUE(appLayer->init(ownAddr).isOk());
}

void tearDown(void) {
    appLayer.reset();
    tpLayer.reset();
    nwLayer.reset();
    dlLayer.reset();
    physicalLayer.reset();
    platform.reset();
}

void test_ApplicationLayer_CompileCheck() {
    // After close(), ApplicationLayer must reject outbound sends.
    appLayer->close();
    const auto ok = appLayer->sendGroupValueRead(GroupAddress(1, 2, 3));
    TEST_ASSERT_TRUE(ok.isError());
}

void test_APCIServices() {
    TEST_ASSERT_EQUAL(0x000, static_cast<uint16_t>(APCIService::GroupValueRead));
    TEST_ASSERT_EQUAL(0x300, static_cast<uint16_t>(APCIService::DeviceDescriptorRead));
    TEST_ASSERT_EQUAL(0x380, static_cast<uint16_t>(APCIService::Restart));
}

void test_DeviceDescriptorType0() {
    DeviceDescriptorType0 desc{0x07, 0xB0}; // mask version 0x07B0
    TEST_ASSERT_EQUAL(0x07, desc.maskHigh);
    TEST_ASSERT_EQUAL(0xB0, desc.maskLow);
}

void test_AuthorizationKey() {
    AuthorizationKey key = {0x12, 0x34, 0x56, 0x78};
    TEST_ASSERT_EQUAL(4, key.size());
}

void test_AddressTypes() {
    IndividualAddress addr(1, 2, 3);
    TEST_ASSERT_EQUAL(1, addr.area());
    TEST_ASSERT_EQUAL(2, addr.line());
}

static std::vector<uint8_t> extractTpduFromTp1Frame(std::span<const uint8_t> frame) {
    // FrameCodec layout: ctrl(1) src(2) dst(2) len(1) tpdu(len+1) checksum(1)
    TEST_ASSERT_TRUE(frame.size() >= 8);
    const uint8_t lengthField = frame[5];
    const uint8_t dataLength = lengthField & 0x0F;
    const size_t tpduLength = static_cast<size_t>(dataLength) + 1u;
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(8U + dataLength), static_cast<uint32_t>(frame.size()));
    TEST_ASSERT_TRUE(tpduLength >= 2);
    return std::vector<uint8_t>(frame.begin() + 6, frame.begin() + 6 + tpduLength);
}

void test_GroupValueWrite() {
    GroupAddress groupAddr(1, 2, 3);
    std::vector<uint8_t> value = {0x01};
    
    auto result = appLayer->sendGroupValueWrite(groupAddr, value);
    TEST_ASSERT_TRUE(result.isOk());
    
    // Verify frame was sent (check physical layer has frame)
    std::vector<uint8_t> sentFrame;
    bool haFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(haFrame);

    // Golden TP1 bytes (end-to-end ApplicationLayer -> Transport -> Network -> DL -> FrameCodec)
    // ctrl=0xBC: standard frame, not repeated, bit4 set (mandatory for all
    // L_DATA frames per KNX 03_02_02), Low priority, no DL ack-request bit.
    // src=1.1.1 (0x1101), dst=1/2/3 group (0x0A03), len=0xE1 (group + hop=6 +
    // length field = TPDU octets − 1 = 1), tpdu=00 81, checksum=0x3A.
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x0A, 0x03, 0xE1, 0x00, 0x81, 0x3A};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(2, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0x81, tpdu[1]); // GroupValueWrite short APDU, value=1 in APCI data6
}

void test_GroupValueRead() {
    GroupAddress groupAddr(1, 2, 3);
    
    auto result = appLayer->sendGroupValueRead(groupAddr);
    TEST_ASSERT_TRUE(result.isOk());
    
    // Verify frame was sent
    std::vector<uint8_t> sentFrame;
    bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes (end-to-end ApplicationLayer -> Transport -> Network -> DL -> FrameCodec)
    // ctrl=0xBC (standard, not repeated, bit4 mandatory, Low priority), src=1.1.1 (0x1101), dst=1/2/3 group (0x0A03), len=0xE2 (group + hop=6 + tpduLen=2), tpdu=00 00, checksum=0x8A
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x0A, 0x03, 0xE1, 0x00, 0x00, 0xBB};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(2, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[1]); // GroupValueRead, no payload
}

void test_GroupValueResponse() {
    GroupAddress groupAddr(1, 2, 3);
    std::vector<uint8_t> value = {0x42};
    
    auto result = appLayer->sendGroupValueResponse(groupAddr, value);
    TEST_ASSERT_TRUE(result.isOk());
    
    // Verify frame was sent
    std::vector<uint8_t> sentFrame;
    bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes (end-to-end ApplicationLayer -> Transport -> Network -> DL -> FrameCodec)
    // ctrl=0xBC (standard, not repeated, bit4 mandatory, Low priority), src=1.1.1 (0x1101), dst=1/2/3 group (0x0A03), len=0xE3 (group + hop=6 + tpduLen=3), tpdu=00 40 42, checksum=0x89
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x0A, 0x03, 0xE2, 0x00, 0x40, 0x42, 0xBA};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(3, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0x40, tpdu[1]); // GroupValueResponse long APDU, payload carries value
    TEST_ASSERT_EQUAL_HEX8(0x42, tpdu[2]);
}

void test_GroupValueReceiveCallback() {
    bool callbackInvoked = false;
    
    appLayer->setReceiveCallback([&](const IndividualAddress& src,
                                     const GroupAddress& dest,
                                     APCIService svc,
                                     std::span<const uint8_t> data,
                                     knx::AddressType destinationType) {
        callbackInvoked = true;
    });
    
    // Verify callback was set (can't easily test invocation without mocking transport layer)
    // The fact that setReceiveCallback() doesn't crash means it works
    TEST_ASSERT_FALSE(callbackInvoked); // Not invoked yet, but callback is set
}

void test_GroupValueReceiveQueueWithoutCallback() {
    appLayer->setReceiveCallback(nullptr);

    knx::network::NDataFrame frame;
    frame.dlFrame.source = IndividualAddress(2, 2, 2);
    frame.dlFrame.destination = GroupAddress(1, 2, 3);
    frame.dlFrame.destinationType = knx::AddressType::Group;
    frame.dlFrame.hopCount = 6;
    frame.dlFrame.setTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        knx::application::APCIField::create(APCIService::GroupValueWrite, 0x01),
        std::span<const uint8_t>{});

    tpLayer->handleNetworkRx(frame);

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedReceiveCount()));

    knx::application::ADataFrame queued;
    TEST_ASSERT_TRUE(appLayer->popReceivedFrame(queued));
    TEST_ASSERT_EQUAL_UINT16(IndividualAddress(2, 2, 2).raw, queued.source.raw);
    TEST_ASSERT_EQUAL_UINT16(GroupAddress(1, 2, 3).raw, queued.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(queued.service));
    TEST_ASSERT_TRUE(queued.destinationType == knx::AddressType::Group);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(queued.data.size()));
    TEST_ASSERT_EQUAL_UINT8(0x01u, queued.data[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(appLayer->queuedReceiveCount()));
}

void test_GroupValueReceiveQueueDropsNewestWhenFull() {
    appLayer->setReceiveCallback(nullptr);

    for (uint8_t value = 0u; value < 9u; ++value) {
        knx::network::NDataFrame frame;
        frame.dlFrame.source = IndividualAddress(2, 2, static_cast<uint8_t>(10u + value));
        frame.dlFrame.destination = GroupAddress(1, 2, 3);
        frame.dlFrame.destinationType = knx::AddressType::Group;
        frame.dlFrame.hopCount = 6;
        frame.dlFrame.setTpdu(
            knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
            knx::application::APCIField::create(APCIService::GroupValueWrite, value),
            std::span<const uint8_t>{});

        tpLayer->handleNetworkRx(frame);
    }

    TEST_ASSERT_EQUAL_UINT32(8u, static_cast<uint32_t>(appLayer->queuedReceiveCount()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->droppedReceiveFrameCount()));

    knx::application::ADataFrame queued;
    TEST_ASSERT_TRUE(appLayer->popReceivedFrame(queued));
    TEST_ASSERT_EQUAL_UINT16(IndividualAddress(2, 2, 10).raw, queued.source.raw);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(queued.data.size()));
    TEST_ASSERT_EQUAL_UINT8(0x00u, queued.data[0]);
}

void test_IndividualAddressReadResponseBroadcast() {
    appLayer->setProgrammingModeEnabled(true);

    knx::network::NDataFrame requestFrame;
    requestFrame.dlFrame.source = knx::initialIndividualAddress();
    requestFrame.dlFrame.destination = GroupAddress(0);
    requestFrame.dlFrame.destinationType = knx::AddressType::Group;
    requestFrame.dlFrame.hopCount = 6;
    requestFrame.dlFrame.setTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        knx::application::APCIField::create(APCIService::IndividualAddressRead),
        std::span<const uint8_t>{});

    tpLayer->handleNetworkRx(requestFrame);

    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sentFrame));

    // Request should yield a broadcast response: destination=0x0000 group, TPDU=A_IndividualAddress_Response.
    // Control byte 0xB0: standard frame (0x80) | not-repeated (0x20) | group/broadcast (0x10) | system priority (0x00).
    TEST_ASSERT_EQUAL_HEX8(0xB0, sentFrame[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0000, static_cast<uint16_t>((sentFrame[3] << 8) | sentFrame[4]));
    TEST_ASSERT_TRUE((sentFrame[5] & 0x80) != 0);

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(tpdu.size()));
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0x40, tpdu[1]);
    // The device answers with its own address, which setUp() sets to 1.1.1 —
    // not the uncommissioned 15.15.255 this assertion originally assumed.
    TEST_ASSERT_EQUAL_HEX8(0x11, tpdu[2]);
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[3]);
}

// 03/05/02 §2.17.1.4 NM_Read_SerialNumber_By_ProgrammingMode — ETS's scan for
// devices in programming mode.  The request bytes below are the ones ETS 6
// actually puts on the wire.
namespace {

knx::network::NDataFrame makeProgrammingModeScanFrame() {
    // parameter_type: object_type=0x0000 (Device Object), PID=11 (12 bits,
    // left-aligned) + 4 reserved bits, then test_info operand 0x01.
    static const std::array<uint8_t, 5> payload = {0x00, 0x00, 0x00, 0xB0, 0x01};

    knx::network::NDataFrame requestFrame;
    requestFrame.dlFrame.source = IndividualAddress(1, 1, 254);
    requestFrame.dlFrame.destination = GroupAddress(0);
    requestFrame.dlFrame.destinationType = knx::AddressType::Group;
    requestFrame.dlFrame.hopCount = 6;
    requestFrame.dlFrame.setTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        knx::application::APCIField::create(APCIService::SystemNetworkParameterRead),
        std::span<const uint8_t>(payload));
    return requestFrame;
}

const knx::application::KnxSerialNumber kTestSerial = {0xE4, 0xB3, 0x23, 0x87, 0x9B, 0x84};

}  // namespace

void test_SystemNetworkParameterReadAnswersWithSerialNumberInProgrammingMode() {
    appLayer->setProgrammingModeEnabled(true);
    appLayer->networkParameterService().setSerialNumber(kTestSerial);

    auto requestFrame = makeProgrammingModeScanFrame();
    tpLayer->handleNetworkRx(requestFrame);

    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sentFrame));

    // Broadcast response, as for A_IndividualAddress_Response.
    TEST_ASSERT_EQUAL_HEX8(0xB0, sentFrame[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0000, static_cast<uint16_t>((sentFrame[3] << 8) | sentFrame[4]));

    // APCI = A_SystemNetworkParameter_Response (0x1C9), then the echoed
    // parameter_type + test_info, then test_result = our serial number.
    // 2 octets APCI + 4 parameter_type + 1 test_info + 6 test_result.
    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT32(13u, static_cast<uint32_t>(tpdu.size()));
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0xC9, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[4]);
    TEST_ASSERT_EQUAL_HEX8(0xB0, tpdu[5]);
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[6]);
    for (size_t i = 0; i < kTestSerial.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(kTestSerial[i], tpdu[7 + i]);
    }
}

void test_SystemNetworkParameterReadStaysSilentWithoutProgrammingMode() {
    appLayer->setProgrammingModeEnabled(false);
    appLayer->networkParameterService().setSerialNumber(kTestSerial);

    auto requestFrame = makeProgrammingModeScanFrame();
    tpLayer->handleNetworkRx(requestFrame);

    // Only devices in programming mode may answer this scan.
    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_FALSE(physicalLayer->getSentFrame(sentFrame));
}

void test_SystemNetworkParameterReadIgnoresUnsupportedParameterType() {
    appLayer->setProgrammingModeEnabled(true);
    appLayer->networkParameterService().setSerialNumber(kTestSerial);

    // PID 12 (manufacturer id) instead of 11: not a procedure this device
    // implements, so it must stay silent rather than answer FFFFh/FFh.
    const std::array<uint8_t, 5> payload = {0x00, 0x00, 0x00, 0xC0, 0x01};
    knx::network::NDataFrame requestFrame;
    requestFrame.dlFrame.source = IndividualAddress(1, 1, 254);
    requestFrame.dlFrame.destination = GroupAddress(0);
    requestFrame.dlFrame.destinationType = knx::AddressType::Group;
    requestFrame.dlFrame.hopCount = 6;
    requestFrame.dlFrame.setTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        knx::application::APCIField::create(APCIService::SystemNetworkParameterRead),
        std::span<const uint8_t>(payload));

    tpLayer->handleNetworkRx(requestFrame);

    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_FALSE(physicalLayer->getSentFrame(sentFrame));
}

// A_Restart master reset (03/03/07 §3.4.2.2). restart_type is bit 0 of the APCI
// — 0x381, not a payload octet — and the erase code + channel follow. The reply
// is mandatory: without it ETS only sees a timeout ("The device does not
// respond"), and with a non-zero error code it reports "ConfirmedRestart failed".
namespace {

knx::network::NDataFrame makeMasterResetFrame(uint8_t eraseCode) {
    const std::array<uint8_t, 2> payload = {eraseCode, 0x00};
    knx::network::NDataFrame requestFrame;
    requestFrame.dlFrame.source = IndividualAddress(1, 1, 254);
    requestFrame.dlFrame.destination = GroupAddress(IndividualAddress(1, 1, 1).raw);
    requestFrame.dlFrame.destinationType = knx::AddressType::Individual;
    requestFrame.dlFrame.hopCount = 6;
    requestFrame.dlFrame.setTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        knx::application::APCIField::create(APCIService::Restart, 0x01),
        std::span<const uint8_t>(payload));
    return requestFrame;
}

}  // namespace

void test_RestartMasterResetAnswersAndInvokesHandler() {
    bool restarted = false;
    knx::application::RestartType observedType{};
    appLayer->restartService().setRestartCallback(
        [&](knx::application::RestartType type) -> knx::util::Result<void> {
            restarted = true;
            observedType = type;
            return knx::util::Result<void>::ok();
        });

    auto requestFrame = makeMasterResetFrame(0x01);  // ConfirmedRestart
    tpLayer->handleNetworkRx(requestFrame);

    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sentFrame));

    // A_Restart_Response: APCI 0x3A1, error code, then a 2-octet process time.
    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT32(5u, static_cast<uint32_t>(tpdu.size()));
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA1, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[2]);  // error code = success, not 0xFF
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[4]);

    TEST_ASSERT_TRUE(restarted);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(knx::application::RestartType::MasterReset),
                            static_cast<uint8_t>(observedType));
}

void test_RestartMasterResetReportsErrorWhenNoHandlerRegistered() {
    // No restart callback: the device must still answer, with a non-zero error
    // code rather than silence.
    auto requestFrame = makeMasterResetFrame(0x01);
    tpLayer->handleNetworkRx(requestFrame);

    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sentFrame));

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_HEX8(0xA1, tpdu[1]);
    TEST_ASSERT_NOT_EQUAL(0x00, tpdu[2]);
}

void test_IndividualAddressWriteUpdatesOwnAddressWithoutSendingImmediateResponse() {
    appLayer->setProgrammingModeEnabled(true);

    const IndividualAddress newAddress(1, 1, 42);
    bool callbackCalled = false;
    IndividualAddress updatedAddress;
    appLayer->setIndividualAddressUpdateCallback([&](const IndividualAddress& address) {
        callbackCalled = true;
        updatedAddress = address;
    });

    knx::network::NDataFrame requestFrame;
    requestFrame.dlFrame.source = knx::initialIndividualAddress();
    requestFrame.dlFrame.destination = GroupAddress(0);
    requestFrame.dlFrame.destinationType = knx::AddressType::Group;
    requestFrame.dlFrame.hopCount = 6;
    const std::array<uint8_t, 2> newAddressBytes = {
        static_cast<uint8_t>((newAddress.raw >> 8) & 0xFF),
        static_cast<uint8_t>(newAddress.raw & 0xFF)
    };
    requestFrame.dlFrame.setTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        knx::application::APCIField::create(APCIService::IndividualAddressWrite),
        std::span<const uint8_t>(newAddressBytes));

    tpLayer->handleNetworkRx(requestFrame);

    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_FALSE(physicalLayer->getSentFrame(sentFrame));
    TEST_ASSERT_TRUE(callbackCalled);
    TEST_ASSERT_EQUAL_UINT16(newAddress.raw, updatedAddress.raw);
}

void test_SendOutcomeQueueRecordsDirectSendSuccess() {
    TEST_ASSERT_TRUE(appLayer->sendGroupValueWrite(GroupAddress(1, 2, 3), std::vector<uint8_t>{0x11}).isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));

    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL_UINT16(GroupAddress(1, 2, 3).raw, outcome.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_TRUE(outcome.destinationType == knx::AddressType::Group);
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(1u, outcome.attempts);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));
}

void test_SendOutcomeQueueRecordsLowerLayerFailure() {
    physicalLayer->close();

    auto result = appLayer->sendGroupValueWrite(GroupAddress(1, 2, 3), std::vector<uint8_t>{0x22});
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::TransmissionFailed), static_cast<int>(result.error()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));

    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL_UINT16(GroupAddress(1, 2, 3).raw, outcome.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::TransmissionFailed), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(1u, outcome.attempts);
}

// Send outcomes are an optional diagnostic feed — during an ETS download
// nothing calls popSendOutcome(). Overflow therefore drops the *oldest*
// outcome: an observer that comes back later wants the most recent
// transmissions, and the previous drop-newest behaviour additionally logged a
// warning per frame for a queue nobody was reading.
void test_SendOutcomeQueueOverwritesOldestWhenFull() {
    for (uint16_t sub = 1u; sub <= 9u; ++sub) {
        TEST_ASSERT_TRUE(appLayer->sendGroupValueWrite(GroupAddress(1, 2, sub), std::vector<uint8_t>{0x01}).isOk());
    }

    TEST_ASSERT_EQUAL_UINT32(8u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->droppedSendOutcomeCount()));

    // The first outcome was overwritten, so the queue starts at the second
    // send and still ends with the newest.
    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL_UINT16(GroupAddress(1, 2, 2).raw, outcome.destination.raw);

    for (uint16_t sub = 3u; sub <= 8u; ++sub) {
        TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
        TEST_ASSERT_EQUAL_UINT16(GroupAddress(1, 2, sub).raw, outcome.destination.raw);
    }
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL_UINT16(GroupAddress(1, 2, 9).raw, outcome.destination.raw);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));
}

void test_SendDataProgressionSeamQueuesSuccessOutcome() {
    auto beginResult = appLayer->beginSendData(GroupAddress(1, 2, 3),
                                               APCIService::GroupValueWrite,
                                               std::vector<uint8_t>{0x12},
                                               knx::AddressType::Group);
    TEST_ASSERT_TRUE(beginResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));

    auto progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));
    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(1u, outcome.attempts);
}

void test_SendDataProgressionSeamQueuesFailureOutcome() {
    physicalLayer->close();

    auto beginResult = appLayer->beginSendData(GroupAddress(1, 2, 3),
                                               APCIService::GroupValueWrite,
                                               std::vector<uint8_t>{0x13},
                                               knx::AddressType::Group);
    TEST_ASSERT_TRUE(beginResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));

    auto progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::TransmissionFailed),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));
    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::TransmissionFailed), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(1u, outcome.attempts);
}

void test_SendDataProgressionSeamRetriesTransmissionFailureThenQueuesSuccessOutcome() {
    physicalLayer->queueSendResult(knx::util::ErrorCode::TransmissionFailed);
    physicalLayer->queueSendResult(knx::util::ErrorCode::Success);

    knx::application::SendOptions options;
    options.maxAttempts = 2u;
    options.retryOnTransmissionFailed = true;

    auto beginResult = appLayer->beginSendData(GroupAddress(1, 2, 3),
                                               APCIService::GroupValueWrite,
                                               std::vector<uint8_t>{0x14},
                                               knx::AddressType::Group,
                                               options);
    TEST_ASSERT_TRUE(beginResult.isOk());

    auto progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Pending),
                      static_cast<int>(progress.value()));

    progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));
    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_SendDataBlockingRetriesTransmissionFailureThenQueuesSuccessOutcome() {
    physicalLayer->queueSendResult(knx::util::ErrorCode::TransmissionFailed);
    physicalLayer->queueSendResult(knx::util::ErrorCode::Success);

    knx::application::SendOptions options;
    options.maxAttempts = 2u;
    options.retryOnTransmissionFailed = true;

    auto result = appLayer->sendData(GroupAddress(1, 2, 3),
                                     APCIService::GroupValueWrite,
                                     std::vector<uint8_t>{0x15},
                                     knx::AddressType::Group,
                                     options);
    TEST_ASSERT_TRUE(result.isOk());

    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_SendDataProgressionSeamTimeoutRetryExhaustionQueuesFailureOutcome() {
    physicalLayer->queueSendResult(knx::util::ErrorCode::Timeout);
    physicalLayer->queueSendResult(knx::util::ErrorCode::Timeout);

    knx::application::SendOptions options;
    options.maxAttempts = 2u;
    options.retryOnTimeout = true;

    auto beginResult = appLayer->beginSendData(GroupAddress(1, 2, 3),
                                               APCIService::GroupValueWrite,
                                               std::vector<uint8_t>{0x16},
                                               knx::AddressType::Group,
                                               options);
    TEST_ASSERT_TRUE(beginResult.isOk());

    auto progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Pending),
                      static_cast<int>(progress.value()));

    progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Timeout),
                      static_cast<int>(progress.value()));

    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Timeout), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_SendPolicyPresetHelpersProduceExpectedRetryOptions() {
    const auto single = ApplicationLayer::optionsForPreset(knx::application::SendPolicyPreset::SingleAttempt);
    TEST_ASSERT_EQUAL_UINT8(1u, single.maxAttempts);
    TEST_ASSERT_TRUE(single.retryOnBusy);
    TEST_ASSERT_TRUE(single.retryOnTimeout);
    TEST_ASSERT_FALSE(single.retryOnTransmissionFailed);

    const auto retryOnce = ApplicationLayer::optionsForPreset(knx::application::SendPolicyPreset::RetryTransientOnce);
    TEST_ASSERT_EQUAL_UINT8(2u, retryOnce.maxAttempts);
    TEST_ASSERT_TRUE(retryOnce.retryOnBusy);
    TEST_ASSERT_TRUE(retryOnce.retryOnTimeout);
    TEST_ASSERT_FALSE(retryOnce.retryOnTransmissionFailed);

    const auto retryTwice = ApplicationLayer::optionsForPreset(knx::application::SendPolicyPreset::RetryTransientTwice);
    TEST_ASSERT_EQUAL_UINT8(3u, retryTwice.maxAttempts);
    TEST_ASSERT_TRUE(retryTwice.retryOnBusy);
    TEST_ASSERT_TRUE(retryTwice.retryOnTimeout);
    TEST_ASSERT_FALSE(retryTwice.retryOnTransmissionFailed);
}

void test_GroupValueWriteUsesConfiguredDefaultSendOptions() {
    physicalLayer->queueSendResult(knx::util::ErrorCode::Timeout);
    physicalLayer->queueSendResult(knx::util::ErrorCode::Success);

    appLayer->setDefaultSendOptions(
        ApplicationLayer::optionsForPreset(knx::application::SendPolicyPreset::RetryTransientOnce));

    auto result = appLayer->sendGroupValueWrite(GroupAddress(1, 2, 3), std::vector<uint8_t>{0x17});
    TEST_ASSERT_TRUE(result.isOk());

    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_BeginSendGroupValueWriteUsesConfiguredDefaultSendOptions() {
    physicalLayer->queueSendResult(knx::util::ErrorCode::Timeout);
    physicalLayer->queueSendResult(knx::util::ErrorCode::Success);

    appLayer->setDefaultSendOptions(
        ApplicationLayer::optionsForPreset(knx::application::SendPolicyPreset::RetryTransientOnce));

    auto beginResult = appLayer->beginSendGroupValueWrite(GroupAddress(1, 2, 3), std::vector<uint8_t>{0x18});
    TEST_ASSERT_TRUE(beginResult.isOk());

    auto progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Pending),
                      static_cast<int>(progress.value()));

    progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Success),
                      static_cast<int>(progress.value()));

    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_BeginSendGroupValueReadQueuesSuccessOutcome() {
    auto beginResult = appLayer->beginSendGroupValueRead(GroupAddress(1, 2, 3));
    TEST_ASSERT_TRUE(beginResult.isOk());

    auto progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Success),
                      static_cast<int>(progress.value()));

    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueRead), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(1u, outcome.attempts);
}

void test_BeginSendGroupValueResponseQueuesSuccessOutcome() {
    auto beginResult = appLayer->beginSendGroupValueResponse(GroupAddress(1, 2, 3), std::vector<uint8_t>{0x19});
    TEST_ASSERT_TRUE(beginResult.isOk());

    auto progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Success),
                      static_cast<int>(progress.value()));

    knx::application::ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueResponse), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(1u, outcome.attempts);
}

void test_MemoryRead_GoldenTp1Frame(void) {
    // Send A_Memory_Read to an individual destination.
    // Internal data format for MemoryRead is: [count, addr_hi, addr_lo]
    const knx::IndividualAddress dest(1, 1, 2);
    const std::vector<uint8_t> data = {10, 0x12, 0x34};

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::MemoryRead, data, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes (end-to-end ApplicationLayer -> Transport -> Network -> DL -> FrameCodec)
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x64 (hop=6, tpduLen=4), tpdu=02 0A 12 34, checksum=0x38
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x63, 0x02, 0x0A, 0x12, 0x34, 0x0D};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(4, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x02, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0A, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x12, tpdu[2]);
    TEST_ASSERT_EQUAL_HEX8(0x34, tpdu[3]);
}

void test_DeviceDescriptorRead_GoldenTp1Frame(void) {
    // Send A_DeviceDescriptor_Read to an individual destination.
    // Descriptor type is encoded in APCI data6 (no payload).
    const knx::IndividualAddress dest(1, 1, 2);
    const std::vector<uint8_t> data = {2};

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::DeviceDescriptorRead, data, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes (end-to-end ApplicationLayer -> Transport -> Network -> DL -> FrameCodec)
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x62 (hop=6, tpduLen=2), tpdu=03 02, checksum=0x11
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x61, 0x03, 0x02, 0x20};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(2, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, tpdu[1]);
}

void test_DeviceDescriptorResponse_GoldenTp1Frame(void) {
    // Send A_DeviceDescriptor_Response to an individual destination.
    // Descriptor type is encoded in APCI data6; payload contains descriptor bytes.
    const knx::IndividualAddress dest(1, 1, 2);
    const std::vector<uint8_t> data = {2, 0x11, 0x22};

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::DeviceDescriptorResponse, data, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes (end-to-end ApplicationLayer -> Transport -> Network -> DL -> FrameCodec)
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x64 (hop=6, tpduLen=4), tpdu=03 42 11 22, checksum=0x64
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x63, 0x03, 0x42, 0x11, 0x22, 0x51};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(4, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0x42, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x11, tpdu[2]);
    TEST_ASSERT_EQUAL_HEX8(0x22, tpdu[3]);
}

void test_PropertyValueRead_GoldenTp1Frame(void) {
    // Send A_PropertyValue_Read (extended APCI via Escape) to an individual destination.
    const knx::IndividualAddress dest(1, 1, 2);
    // Payload layout used by ApplicationLayer dispatch:
    // [objectIndex, propertyId, (elementCount<<4)|(startIndex>>8 & 0x0F), startIndex_lo]
    const std::vector<uint8_t> payload = {0x01, 0x02, 0x30, 0x12};

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::PropertyValueRead, payload, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes (end-to-end ApplicationLayer -> Transport -> Network -> DL -> FrameCodec)
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x66 (hop=6, tpduLen=6), tpdu=03 D4 01 02 30 12, checksum=0xE2
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x65, 0x03, 0xD5, 0x01, 0x02, 0x30, 0x12, 0xD2};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(6, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD5, tpdu[1]); // A_PropertyValue_Read = 0x3D5
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[2]);
    TEST_ASSERT_EQUAL_HEX8(0x02, tpdu[3]);
    TEST_ASSERT_EQUAL_HEX8(0x30, tpdu[4]);
    TEST_ASSERT_EQUAL_HEX8(0x12, tpdu[5]);
}

void test_PropertyValueResponse_GoldenTp1Frame(void) {
    // Send A_PropertyValue_Response (extended APCI via Escape) to an individual destination.
    const knx::IndividualAddress dest(1, 1, 2);
    // Payload layout used by ApplicationLayer response callback encoding:
    // [objectIndex, propertyId, (elementCount<<4)|(startIndex>>8 & 0x0F), startIndex_lo, data...]
    const std::vector<uint8_t> payload = {0x01, 0x02, 0x30, 0x12, 0xAA, 0xBB, 0xCC};

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::PropertyValueResponse, payload, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes (end-to-end ApplicationLayer -> Transport -> Network -> DL -> FrameCodec)
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x69 (hop=6, tpduLen=9), tpdu=03 D5 01 02 30 12 AA BB CC, checksum=0x31
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x68, 0x03, 0xD6, 0x01, 0x02, 0x30, 0x12, 0xAA, 0xBB, 0xCC, 0x01};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(9, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD6, tpdu[1]); // A_PropertyValue_Response = 0x3D6
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[2]);
    TEST_ASSERT_EQUAL_HEX8(0x02, tpdu[3]);
    TEST_ASSERT_EQUAL_HEX8(0x30, tpdu[4]);
    TEST_ASSERT_EQUAL_HEX8(0x12, tpdu[5]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, tpdu[6]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, tpdu[7]);
    TEST_ASSERT_EQUAL_HEX8(0xCC, tpdu[8]);
}

void test_PropertyDescriptionRead_GoldenTp1Frame(void) {
    // Send A_PropertyDescription_Read (extended APCI via Escape) to an individual destination.
    const knx::IndividualAddress dest(1, 1, 2);
    // Payload layout used by ApplicationLayer dispatch:
    // [objectIndex, propertyId, propertyIndex]
    const std::vector<uint8_t> payload = {0x01, 0x02, 0x03};

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::PropertyDescriptionRead, payload, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x65 (hop=6, tpduLen=5), tpdu=03 D8 01 02 03, checksum=0xCC
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x64, 0x03, 0xD8, 0x01, 0x02, 0x03, 0xFF};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(5, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD8, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[2]);
    TEST_ASSERT_EQUAL_HEX8(0x02, tpdu[3]);
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[4]);
}

void test_PropertyDescriptionResponse_GoldenTp1Frame(void) {
    // Send A_PropertyDescription_Response (extended APCI via Escape) to an individual destination.
    const knx::IndividualAddress dest(1, 1, 2);
    // Payload layout used by ApplicationLayer description response callback encoding:
    // [objectIndex, propertyId, propertyIndex, (type|writeEnabled), maxElements_hi, maxElements_lo, (readLevel<<4)|writeLevel]
    const std::vector<uint8_t> payload = {
        0x01, // objectIndex
        0x02, // propertyId
        0x03, // propertyIndex
        0x84, // type=0x04 with writeEnabled=1
        0x00, // maxElements hi
        0x10, // maxElements lo
        0x21  // readLevel=2, writeLevel=1
    };

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::PropertyDescriptionResponse, payload, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x69 (hop=6, tpduLen=9), tpdu=03 D9 01 02 03 84 00 10 21, checksum=0x74
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x68, 0x03, 0xD9, 0x01, 0x02, 0x03, 0x84, 0x00, 0x10, 0x21, 0x47};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(9, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD9, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[2]);
    TEST_ASSERT_EQUAL_HEX8(0x02, tpdu[3]);
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[4]);
    TEST_ASSERT_EQUAL_HEX8(0x84, tpdu[5]);
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[6]);
    TEST_ASSERT_EQUAL_HEX8(0x10, tpdu[7]);
    TEST_ASSERT_EQUAL_HEX8(0x21, tpdu[8]);
}

void test_AuthorizeRequest_GoldenTp1Frame(void) {
    // Send A_Authorize_Request (extended APCI via Escape) to an individual destination.
    const knx::IndividualAddress dest(1, 1, 2);
    // Payload layout used by ApplicationLayer dispatch:
    // [level, key(4 bytes)]
    const std::vector<uint8_t> payload = {0x02, 0x12, 0x34, 0x56, 0x78};

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::AuthorizeRequest, payload, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x67 (hop=6, tpduLen=7), tpdu=03 D1 02 12 34 56 78, checksum=0xCD
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x66, 0x03, 0xD1, 0x02, 0x12, 0x34, 0x56, 0x78, 0xFE};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(7, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD1, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02, tpdu[2]);
    TEST_ASSERT_EQUAL_HEX8(0x12, tpdu[3]);
    TEST_ASSERT_EQUAL_HEX8(0x34, tpdu[4]);
    TEST_ASSERT_EQUAL_HEX8(0x56, tpdu[5]);
    TEST_ASSERT_EQUAL_HEX8(0x78, tpdu[6]);
}

void test_AuthorizeResponse_GoldenTp1Frame(void) {
    // Send A_Authorize_Response (extended APCI via Escape) to an individual destination.
    const knx::IndividualAddress dest(1, 1, 2);
    // Payload layout used by ApplicationLayer response callback encoding:
    // [level]
    const std::vector<uint8_t> payload = {0x02};

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::AuthorizeResponse, payload, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x63 (hop=6, tpduLen=3), tpdu=03 D2 02, checksum=0xC2
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x62, 0x03, 0xD2, 0x02, 0xF1};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(3, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD2, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02, tpdu[2]);
}

void test_Restart_GoldenTp1Frame(void) {
    // Send A_Restart (standard APCI) to an individual destination.
    const knx::IndividualAddress dest(1, 1, 2);
    const std::vector<uint8_t> payload = {0x01};

        const auto result = appLayer->sendData(GroupAddress(dest.raw), APCIService::Restart, payload, knx::AddressType::Individual);
        TEST_ASSERT_TRUE(result.isOk());

    std::vector<uint8_t> sentFrame;
    const bool hasFrame = physicalLayer->getSentFrame(sentFrame);
    TEST_ASSERT_TRUE(hasFrame);

    // Golden TP1 bytes
    // ctrl=0x8E, src=1.1.1 (0x1101), dst=1.1.2 (0x1102 individual), len=0x63 (hop=6, tpduLen=3), tpdu=03 80 01, checksum=0x93
    const uint8_t expected[] = {0xBC, 0x11, 0x01, 0x11, 0x02, 0x62, 0x03, 0x80, 0x01, 0xA0};
    TEST_ASSERT_EQUAL_UINT32(sizeof(expected), static_cast<uint32_t>(sentFrame.size()));
    for (size_t i = 0; i < sizeof(expected); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sentFrame[i]);
    }

    const auto tpdu = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_UINT8(3, tpdu.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[2]);
}

// ETS's NM_IndividualAddress_Write final check (03/05/02 §2.3 step 4) reads the
// device descriptor *connectionless* while its management connection is still
// open. Answering that on the connection leaves the check unanswered — ETS then
// reports "the final check of this procedure failed".
namespace {

const knx::IndividualAddress kManagementClient(1, 1, 254);

knx::network::NDataFrame makeInboundFrame(const knx::protocol::TPCIField& tpci,
                                          const knx::application::APCIField& apci) {
    knx::network::NDataFrame frame;
    frame.dlFrame.source = kManagementClient;
    frame.dlFrame.destination = GroupAddress(IndividualAddress(1, 1, 1).raw);
    frame.dlFrame.destinationType = knx::AddressType::Individual;
    frame.dlFrame.hopCount = 6;
    frame.dlFrame.priority = knx::Priority::System;
    frame.dlFrame.setTpdu(tpci, apci, std::span<const uint8_t>{});
    return frame;
}

void openManagementConnection() {
    knx::network::NDataFrame connect;
    connect.dlFrame.source = kManagementClient;
    connect.dlFrame.destination = GroupAddress(IndividualAddress(1, 1, 1).raw);
    connect.dlFrame.destinationType = knx::AddressType::Individual;
    connect.dlFrame.hopCount = 6;
    connect.dlFrame.priority = knx::Priority::System;
    connect.dlFrame.setTpdu(knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect),
                            knx::application::APCIField(0),
                            std::span<const uint8_t>{});
    tpLayer->handleNetworkRx(connect);
}

/// The peer acknowledging our connection-oriented response; without it the
/// device's SeqNoSend never advances.
void injectAck(uint8_t seqNum) {
    knx::network::NDataFrame ack;
    ack.dlFrame.source = kManagementClient;
    ack.dlFrame.destination = GroupAddress(IndividualAddress(1, 1, 1).raw);
    ack.dlFrame.destinationType = knx::AddressType::Individual;
    ack.dlFrame.hopCount = 6;
    ack.dlFrame.priority = knx::Priority::System;
    ack.dlFrame.setTpdu(knx::protocol::TPCIField::ack(seqNum),
                        knx::application::APCIField(0),
                        std::span<const uint8_t>{});
    tpLayer->handleNetworkRx(ack);
}

/// Drains the T_ACK the transport layer sends for numbered data and returns the
/// TPDU of the application response that follows it.
std::vector<uint8_t> takeResponseTpduAfterAck() {
    // Control TPDUs are a single octet, so read it out of the frame directly:
    // ctrl(1) src(2) dst(2) len(1) tpdu(1) checksum(1).
    std::vector<uint8_t> ackFrame;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(ackFrame));
    TEST_ASSERT_EQUAL_UINT32(8u, static_cast<uint32_t>(ackFrame.size()));
    TEST_ASSERT_TRUE((ackFrame[6] & 0xC3) == 0xC2);  // T_ACK

    std::vector<uint8_t> responseFrame;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(responseFrame));
    return extractTpduFromTp1Frame(responseFrame);
}

}  // namespace

void test_ConnectionlessRequestIsAnsweredConnectionlessWhileConnected(void) {
    openManagementConnection();

    // First: a connection-oriented A_DeviceDescriptor_Read (seq 0) must be
    // answered on the connection, seq 0.
    tpLayer->handleNetworkRx(makeInboundFrame(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, 0),
        knx::application::APCIField::create(APCIService::DeviceDescriptorRead, 0)));

    const auto connectedResponse = takeResponseTpduAfterAck();
    TEST_ASSERT_EQUAL_HEX8(0x43, connectedResponse[0]);  // T_Data_Connected seq 0 + APCI high
    TEST_ASSERT_EQUAL_HEX8(0x40, connectedResponse[1]);  // A_DeviceDescriptor_Response, type 0
    injectAck(0);

    // Then the same read arriving connectionless, with the connection still
    // open: the answer must be T_Data_Individual, not T_Data_Connected.
    tpLayer->handleNetworkRx(makeInboundFrame(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        knx::application::APCIField::create(APCIService::DeviceDescriptorRead, 0)));

    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sentFrame));
    const auto connectionlessResponse = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_HEX8(0x03, connectionlessResponse[0]);  // T_Data_Individual + APCI high
    TEST_ASSERT_EQUAL_HEX8(0x40, connectionlessResponse[1]);

    // ...and it must not have consumed a sequence number: the next connected
    // request is still answered with seq 1.
    tpLayer->handleNetworkRx(makeInboundFrame(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, 1),
        knx::application::APCIField::create(APCIService::DeviceDescriptorRead, 0)));

    const auto nextConnectedResponse = takeResponseTpduAfterAck();
    TEST_ASSERT_EQUAL_HEX8(0x47, nextConnectedResponse[0]);  // seq 1
    TEST_ASSERT_EQUAL_HEX8(0x40, nextConnectedResponse[1]);
}

namespace {

/// A_ADC_Read as it arrives from a management client: channel_nr in the APCI's
/// 6-bit data field, read_count as the single payload octet (03/03/07 Fig. 72).
knx::network::NDataFrame makeAdcReadFrame(uint8_t channel, uint8_t readCount) {
    knx::network::NDataFrame frame;
    frame.dlFrame.source = kManagementClient;
    frame.dlFrame.destination = GroupAddress(IndividualAddress(1, 1, 1).raw);
    frame.dlFrame.destinationType = knx::AddressType::Individual;
    frame.dlFrame.hopCount = 6;
    frame.dlFrame.priority = knx::Priority::System;
    const uint8_t payload[] = {readCount};
    frame.dlFrame.setTpdu(knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
                          knx::application::APCIField::create(APCIService::ADCRead, channel),
                          std::span<const uint8_t>(payload, sizeof(payload)));
    return frame;
}

} // namespace

// A_ADC_Read went unanswered before: the service was decoded and then fell
// through to the user callback, which nothing handles. 03/03/07 §3.5.2 requires
// an A_ADC_Response either way — a wrong channel number is reported by
// answering with read_count = 0, not by silence. Silence costs the client its
// full connection timeout (observed: ETS stalling ~10 s mid-commissioning).
void test_AdcReadWithoutProviderIsAnsweredWithReadCountZero(void) {
    tpLayer->handleNetworkRx(makeAdcReadFrame(0x01, 4));

    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sentFrame));
    const auto tpdu = extractTpduFromTp1Frame(sentFrame);

    TEST_ASSERT_EQUAL_UINT32(5u, static_cast<uint32_t>(tpdu.size()));
    TEST_ASSERT_EQUAL_HEX8(0x01, tpdu[0]);  // T_Data_Individual + APCI high bits
    TEST_ASSERT_EQUAL_HEX8(0xC1, tpdu[1]);  // A_ADC_Response, channel_nr = 1
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[2]);  // read_count = 0: channel not available
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, tpdu[4]);
}

// With a converter wired up, the answer echoes channel and read_count and
// carries the summed value (Figure 73).
void test_AdcReadAnswersInstalledProviderValue(void) {
    uint8_t seenChannel = 0xFF;
    uint8_t seenCount = 0;
    appLayer->setAdcReadProvider([&](uint8_t channel, uint8_t readCount) -> std::optional<uint16_t> {
        seenChannel = channel;
        seenCount = readCount;
        return channel == 0x05 ? std::optional<uint16_t>(0x1234) : std::nullopt;
    });

    tpLayer->handleNetworkRx(makeAdcReadFrame(0x05, 8));

    std::vector<uint8_t> sentFrame;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sentFrame));
    const auto tpdu = extractTpduFromTp1Frame(sentFrame);

    TEST_ASSERT_EQUAL_UINT8(0x05, seenChannel);
    TEST_ASSERT_EQUAL_UINT8(8, seenCount);
    TEST_ASSERT_EQUAL_UINT32(5u, static_cast<uint32_t>(tpdu.size()));
    TEST_ASSERT_EQUAL_HEX8(0xC5, tpdu[1]);  // A_ADC_Response, channel_nr = 5
    TEST_ASSERT_EQUAL_HEX8(0x08, tpdu[2]);  // read_count echoed
    TEST_ASSERT_EQUAL_HEX8(0x12, tpdu[3]);  // sum high
    TEST_ASSERT_EQUAL_HEX8(0x34, tpdu[4]);  // sum low

    // A channel the provider does not serve is still answered, with count 0.
    tpLayer->handleNetworkRx(makeAdcReadFrame(0x06, 8));
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sentFrame));
    const auto refused = extractTpduFromTp1Frame(sentFrame);
    TEST_ASSERT_EQUAL_HEX8(0xC6, refused[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, refused[2]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ApplicationLayer_CompileCheck);
    RUN_TEST(test_APCIServices);
    RUN_TEST(test_DeviceDescriptorType0);
    RUN_TEST(test_AuthorizationKey);
    RUN_TEST(test_AddressTypes);
    RUN_TEST(test_GroupValueWrite);
    RUN_TEST(test_GroupValueRead);
    RUN_TEST(test_GroupValueResponse);
    RUN_TEST(test_GroupValueReceiveCallback);
    RUN_TEST(test_GroupValueReceiveQueueWithoutCallback);
    RUN_TEST(test_GroupValueReceiveQueueDropsNewestWhenFull);
    RUN_TEST(test_SendOutcomeQueueRecordsDirectSendSuccess);
    RUN_TEST(test_SendOutcomeQueueRecordsLowerLayerFailure);
    RUN_TEST(test_SendOutcomeQueueOverwritesOldestWhenFull);
    RUN_TEST(test_SendDataProgressionSeamQueuesSuccessOutcome);
    RUN_TEST(test_SendDataProgressionSeamQueuesFailureOutcome);
    RUN_TEST(test_SendDataProgressionSeamRetriesTransmissionFailureThenQueuesSuccessOutcome);
    RUN_TEST(test_SendDataBlockingRetriesTransmissionFailureThenQueuesSuccessOutcome);
    RUN_TEST(test_SendDataProgressionSeamTimeoutRetryExhaustionQueuesFailureOutcome);
    RUN_TEST(test_SendPolicyPresetHelpersProduceExpectedRetryOptions);
    RUN_TEST(test_GroupValueWriteUsesConfiguredDefaultSendOptions);
    RUN_TEST(test_BeginSendGroupValueWriteUsesConfiguredDefaultSendOptions);
    RUN_TEST(test_BeginSendGroupValueReadQueuesSuccessOutcome);
    RUN_TEST(test_BeginSendGroupValueResponseQueuesSuccessOutcome);
    RUN_TEST(test_MemoryRead_GoldenTp1Frame);
    RUN_TEST(test_DeviceDescriptorRead_GoldenTp1Frame);
    RUN_TEST(test_DeviceDescriptorResponse_GoldenTp1Frame);
    RUN_TEST(test_PropertyValueRead_GoldenTp1Frame);
    RUN_TEST(test_PropertyValueResponse_GoldenTp1Frame);
    RUN_TEST(test_PropertyDescriptionRead_GoldenTp1Frame);
    RUN_TEST(test_PropertyDescriptionResponse_GoldenTp1Frame);
    RUN_TEST(test_AuthorizeRequest_GoldenTp1Frame);
    RUN_TEST(test_AuthorizeResponse_GoldenTp1Frame);
    RUN_TEST(test_Restart_GoldenTp1Frame);
    // These two were defined but never registered, so the programming-mode
    // commissioning path was silently untested.
    RUN_TEST(test_IndividualAddressReadResponseBroadcast);
    RUN_TEST(test_IndividualAddressWriteUpdatesOwnAddressWithoutSendingImmediateResponse);
    RUN_TEST(test_SystemNetworkParameterReadAnswersWithSerialNumberInProgrammingMode);
    RUN_TEST(test_SystemNetworkParameterReadStaysSilentWithoutProgrammingMode);
    RUN_TEST(test_SystemNetworkParameterReadIgnoresUnsupportedParameterType);
    RUN_TEST(test_RestartMasterResetAnswersAndInvokesHandler);
    RUN_TEST(test_RestartMasterResetReportsErrorWhenNoHandlerRegistered);
    RUN_TEST(test_ConnectionlessRequestIsAnsweredConnectionlessWhileConnected);
    RUN_TEST(test_AdcReadWithoutProviderIsAnsweredWithReadCountZero);
    RUN_TEST(test_AdcReadAnswersInstalledProviderValue);
    return UNITY_END();
}
