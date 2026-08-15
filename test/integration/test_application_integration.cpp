// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_application_integration.cpp
 * @brief Integration tests for application layer stack
 */

#include "knx/application/application_layer.hpp"
#include "knx/transport/transport_layer.hpp"
#include "knx/application/device_descriptor.hpp"
#include "knx/application/address_space.hpp"
#include "knx/transport/transport_layer.hpp"
#include "knx/network/network_layer.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/platform/linux_platform.hpp"
#include "../mocks/mock_physical_layer.hpp"
#include "unity.h"
#include <memory>
#include <span>

using namespace knx;
using namespace knx::application;
using namespace knx::test;

static std::unique_ptr<MockPhysicalLayer> physLayer;
static std::unique_ptr<knx::platform::LinuxPlatform> platformInstance;
static std::unique_ptr<datalink::Tp1DataLinkLayer> dlLayer;
static std::unique_ptr<network::NetworkLayer> nwLayer;
static std::unique_ptr<transport::TransportLayer> tpLayer;
static std::unique_ptr<ApplicationLayer> appLayer;

static IndividualAddress ownAddress(1, 1, 1);

static std::vector<uint8_t> encodeTp1(const datalink::LDataFrame& frame) {
    uint8_t buffer[23];
    auto enc = datalink::FrameCodec::encodeFrame(frame, buffer);
    TEST_ASSERT_TRUE(enc.isOk());
    return std::vector<uint8_t>(buffer, buffer + enc.value());
}

static datalink::LDataFrame decodeTp1(std::span<const uint8_t> wire) {
    datalink::LDataFrame out;
    auto dec = datalink::FrameCodec::decodeFrame(wire, out);
    TEST_ASSERT_TRUE(dec.isOk());
    return out;
}

void setUp(void) {
    platformInstance = std::make_unique<knx::platform::LinuxPlatform>();
    physLayer = std::make_unique<MockPhysicalLayer>();
    datalink::Tp1DataLinkConfig config = datalink::Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;
    dlLayer = std::make_unique<datalink::Tp1DataLinkLayer>(*platformInstance, *physLayer, config);
    nwLayer = std::make_unique<network::NetworkLayer>(*dlLayer);
    tpLayer = std::make_unique<transport::TransportLayer>(*nwLayer);
    appLayer = std::make_unique<ApplicationLayer>(*tpLayer);
    
    TEST_ASSERT_TRUE(dlLayer->init(ownAddress).isOk());
    TEST_ASSERT_TRUE(nwLayer->init(ownAddress).isOk());
    TEST_ASSERT_TRUE(tpLayer->init(ownAddress).isOk());
    TEST_ASSERT_TRUE(appLayer->init(ownAddress).isOk());
}

void tearDown(void) {
    appLayer.reset();
    tpLayer.reset();
    nwLayer.reset();
    dlLayer.reset();
    physLayer.reset();
    platformInstance.reset();
}

void test_FullStack_Initialization() {
    TEST_ASSERT_NOT_NULL(physLayer.get());
    TEST_ASSERT_NOT_NULL(dlLayer.get());
    TEST_ASSERT_NOT_NULL(nwLayer.get());
    TEST_ASSERT_NOT_NULL(tpLayer.get());
    TEST_ASSERT_NOT_NULL(appLayer.get());

    TEST_ASSERT_TRUE(physLayer->isOpen());
    TEST_ASSERT_TRUE(dlLayer->isOpen());
}

void test_FullStack_DeviceDescriptor() {
    DeviceDescriptor desc = DeviceDescriptor::createDefault();
    desc.type0.maskLow = 0x42;
    appLayer->deviceDescriptorService().setDescriptor(desc);

    const auto& stored = appLayer->deviceDescriptorService().getDescriptor();
    TEST_ASSERT_EQUAL(0x07, stored.type0.maskHigh);
    TEST_ASSERT_EQUAL(0x42, stored.type0.maskLow);
}

void test_FullStack_MemoryService() {
    const uint8_t requestedCount = 5;
    const uint16_t requestedAddr = 0x1234;
    bool readCalled = false;

    // Allow reads in the requested address range
    application::MemoryRegion region{
        .startAddress = MemoryAddress(0x1200),
        .size = 0x0100,
        .accessMode = application::MemoryAccessMode::ReadWrite,
        .name = "test"
    };
    TEST_ASSERT_TRUE(appLayer->addressSpace().addRegion(region).isOk());

    appLayer->memoryService().setReadCallback(
        [&](knx::MemoryAddress address, uint8_t len, std::span<uint8_t> data) -> knx::util::Result<void> {
            readCalled = true;
            TEST_ASSERT_EQUAL(requestedAddr, address.raw);
            TEST_ASSERT_EQUAL(requestedCount, len);
            std::fill(data.begin(), data.end(), 0xAA);
            return knx::util::Result<void>::ok();
        }
    );

    // Inject an individual-addressed MemoryRead request to this device
    datalink::LDataFrame rx;
    rx.source = IndividualAddress(1, 1, 2);
    rx.destination = GroupAddress(ownAddress.raw);
    rx.destinationType = AddressType::Individual;
    rx.ackRequested = false;
        rx.setTpdu(knx::protocol::TPCI::UnnumberedData,
                   application::APCIField::create(APCIService::MemoryRead, requestedCount),
                   {static_cast<uint8_t>((requestedAddr >> 8) & 0xFF), static_cast<uint8_t>(requestedAddr & 0xFF)});

    physLayer->injectFrame(encodeTp1(rx));
    TEST_ASSERT_TRUE(readCalled);

    // Expect a MemoryResponse to be sent back to the requester
    TEST_ASSERT_TRUE(physLayer->sentFrameCount() >= 1);
    std::vector<uint8_t> wire;
    TEST_ASSERT_TRUE(physLayer->getSentFrame(wire));
    auto tx = decodeTp1(wire);

    TEST_ASSERT_TRUE(tx.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL(rx.source.raw, tx.destination.raw);
    TEST_ASSERT_EQUAL(ownAddress.raw, tx.source.raw);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::MemoryResponse), static_cast<uint16_t>(tx.apci().service()));
    TEST_ASSERT_EQUAL(requestedCount & 0x3F, tx.apci().data6());

    const auto payload = tx.payload();
    TEST_ASSERT_EQUAL(2 + requestedCount, static_cast<int>(payload.size()));
    TEST_ASSERT_EQUAL(static_cast<uint8_t>((requestedAddr >> 8) & 0xFF), payload[0]);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(requestedAddr & 0xFF), payload[1]);
    for (size_t i = 0; i < requestedCount; i++) {
        TEST_ASSERT_EQUAL(0xAA, payload[2 + i]);
    }
}

void test_FullStack_AuthorizationService() {
    AuthorizationKey key = {0x12, 0x34, 0x56, 0x78};
    appLayer->authorizationService().setKeys(key, key, key);

    // Inject AuthorizeRequest (payload is 4-byte key)
    datalink::LDataFrame rx;
    rx.source = IndividualAddress(1, 1, 2);
    rx.destination = GroupAddress(ownAddress.raw);
    rx.destinationType = AddressType::Individual;
    rx.ackRequested = false;
        rx.setTpdu(knx::protocol::TPCI::UnnumberedData,
                   application::APCIField::create(APCIService::AuthorizeRequest),
                   {0x12, 0x34, 0x56, 0x78});

    physLayer->injectFrame(encodeTp1(rx));

    // Authorization should be stored and response should be sent
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(AuthorizationLevel::Maximum),
                      static_cast<uint8_t>(appLayer->authorizationService().getCurrentLevel(rx.source)));

    TEST_ASSERT_TRUE(physLayer->sentFrameCount() >= 1);
    std::vector<uint8_t> wire;
    TEST_ASSERT_TRUE(physLayer->getSentFrame(wire));
    auto tx = decodeTp1(wire);

    TEST_ASSERT_TRUE(tx.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL(rx.source.raw, tx.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::AuthorizeResponse), static_cast<uint16_t>(tx.apci().service()));
    const auto payload = tx.payload();
    TEST_ASSERT_EQUAL(1, static_cast<int>(payload.size()));
    // Wire encoding is privilege-inverted (KNX access levels): Maximum = 0.
    TEST_ASSERT_EQUAL(0, payload[0]);
}

void test_FullStack_RestartService() {
    bool called = false;
    appLayer->restartService().setRestartCallback(
        [&](application::RestartType type) -> knx::util::Result<void> {
            called = true;
            TEST_ASSERT_EQUAL(static_cast<uint8_t>(application::RestartType::Basic), static_cast<uint8_t>(type));
            return knx::util::Result<void>::ok();
        }
    );

    datalink::LDataFrame rx;
    rx.source = IndividualAddress(1, 1, 2);
    rx.destination = GroupAddress(ownAddress.raw);
    rx.destinationType = AddressType::Individual;
    rx.ackRequested = false;
        rx.setTpdu(knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
             application::APCIField::create(APCIService::Restart),
             {static_cast<uint8_t>(application::RestartType::Basic)});
    physLayer->injectFrame(encodeTp1(rx));

    TEST_ASSERT_TRUE(called);
}

void test_FullStack_MultipleServices() {
    DeviceDescriptor desc = DeviceDescriptor::createDefault();
    appLayer->deviceDescriptorService().setDescriptor(desc);
    
    AuthorizationKey key = {0x11, 0x22, 0x33, 0x44};
    appLayer->authorizationService().setKeys(key, key, key);
    
    appLayer->restartService().setRestartCallback(
        [](application::RestartType) -> knx::util::Result<void> { return knx::util::Result<void>::ok(); }
    );
    
    appLayer->memoryService().setReadCallback(
        [](knx::MemoryAddress, uint8_t, std::span<uint8_t> data) -> knx::util::Result<void> {
            std::fill(data.begin(), data.end(), 0x00);
            return knx::util::Result<void>::ok();
        }
    );

    // Sanity: ensure configuration didn't break init
    TEST_ASSERT_TRUE(dlLayer->isOpen());
}

void test_FullStack_APCIServiceCodes() {
    TEST_ASSERT_EQUAL(0x000, static_cast<uint16_t>(APCIService::GroupValueRead));
    TEST_ASSERT_EQUAL(0x080, static_cast<uint16_t>(APCIService::GroupValueWrite));
    TEST_ASSERT_EQUAL(0x300, static_cast<uint16_t>(APCIService::DeviceDescriptorRead));
    TEST_ASSERT_EQUAL(0x200, static_cast<uint16_t>(APCIService::MemoryRead));
    TEST_ASSERT_EQUAL(0x3D1, static_cast<uint16_t>(APCIService::AuthorizeRequest));
    TEST_ASSERT_EQUAL(0x380, static_cast<uint16_t>(APCIService::Restart));
}

void test_FullStack_ServiceHandlers() {
    DeviceDescriptorService& dds = appLayer->deviceDescriptorService();
    PropertyServices& ps = appLayer->propertyServices();
    MemoryService& ms = appLayer->memoryService();
    AuthorizationService& as = appLayer->authorizationService();
    RestartService& rs = appLayer->restartService();
    
    (void)dds; (void)ps; (void)ms; (void)as; (void)rs;
    TEST_ASSERT_NOT_NULL(&dds);
    TEST_ASSERT_NOT_NULL(&ps);
    TEST_ASSERT_NOT_NULL(&ms);
    TEST_ASSERT_NOT_NULL(&as);
    TEST_ASSERT_NOT_NULL(&rs);
}

void test_FullStack_SendData() {
    GroupAddress destination(1, 2, 3);
    std::vector<uint8_t> data = {0xAB, 0xCD};
    
    auto result = appLayer->sendData(destination, APCIService::GroupValueWrite, data, knx::AddressType::Group);
    TEST_ASSERT_TRUE(result.isOk());
}

void test_FullStack_AddressTypes() {
    IndividualAddress addr(1, 2, 3);
    TEST_ASSERT_EQUAL(1, addr.area());
    TEST_ASSERT_EQUAL(2, addr.line());
    TEST_ASSERT_EQUAL(3, addr.device());
}

void test_FullStack_DescriptorEncodeDecode() {
    DeviceDescriptorType0 type0{0x07, 0x12};
    auto encoded = type0.encode();
    TEST_ASSERT_EQUAL(2, encoded.size());

    auto decoded = DeviceDescriptorType0::decode(encoded);
    TEST_ASSERT_EQUAL(0x07, decoded.maskHigh);
    TEST_ASSERT_EQUAL(0x12, decoded.maskLow);
}

void test_FullStack_AuthorizationLevels() {
    TEST_ASSERT_EQUAL(0x01, static_cast<uint8_t>(AuthorizationLevel::Management));
    TEST_ASSERT_EQUAL(0x02, static_cast<uint8_t>(AuthorizationLevel::Configuration));
    TEST_ASSERT_EQUAL(0x03, static_cast<uint8_t>(AuthorizationLevel::Maximum));
}

void test_FullStack_RestartTypes() {
    TEST_ASSERT_EQUAL(0x00, static_cast<uint8_t>(application::RestartType::Basic));
    TEST_ASSERT_EQUAL(0x01, static_cast<uint8_t>(application::RestartType::MasterReset));
}

void test_FullStack_CompleteCoverage() {
    // Device Descriptor
    DeviceDescriptor desc = DeviceDescriptor::createDefault();
    appLayer->deviceDescriptorService().setDescriptor(desc);
    
    // Property Services
    PropertyServices& ps = appLayer->propertyServices();
    (void)ps;
    
    // Memory Services
    appLayer->memoryService().setReadCallback(
        [](knx::MemoryAddress, uint8_t, std::span<uint8_t>) -> knx::util::Result<void> { return knx::util::Result<void>::ok(); }
    );
    
    // Authorization
    AuthorizationKey key = {0x12, 0x34, 0x56, 0x78};
    appLayer->authorizationService().setKeys(key, key, key);
    
    // Restart
    appLayer->restartService().setRestartCallback(
        [](application::RestartType) -> knx::util::Result<void> { return knx::util::Result<void>::ok(); }
    );
    
    // Dispatcher
    appLayer->setReceiveCallback(
        [](const IndividualAddress&, const GroupAddress&, APCIService, std::span<const uint8_t>, AddressType) {}
    );

    // Exercise end-to-end receive path for GroupValueWrite (short APDU)
    bool got = false;
    appLayer->setReceiveCallback(
        [&](const IndividualAddress& src, const GroupAddress& dst, APCIService svc, std::span<const uint8_t> data, AddressType destinationType) {
            got = true;
            TEST_ASSERT_TRUE(destinationType == AddressType::Group);
            TEST_ASSERT_EQUAL(IndividualAddress(2, 2, 2).raw, src.raw);
            TEST_ASSERT_EQUAL(GroupAddress(1, 2, 3).raw, dst.raw);
            TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(svc));
            TEST_ASSERT_EQUAL(1, static_cast<int>(data.size()));
            TEST_ASSERT_EQUAL(0x01, data[0]);
        }
    );

    dlLayer->setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);
    datalink::LDataFrame rx;
    rx.source = IndividualAddress(2, 2, 2);
    rx.destination = GroupAddress(1, 2, 3);
    rx.destinationType = AddressType::Group;
    rx.ackRequested = false;
    // Short APDU: value 0x01 encoded in APCI low 6 bits, no payload
    rx.setTpdu(knx::protocol::TPCI::UnnumberedData,
               application::APCIField::create(APCIService::GroupValueWrite, 0x01),
               {});
    physLayer->injectFrame(encodeTp1(rx));

    TEST_ASSERT_TRUE(got);
}

void test_FullStack_ApplicationReceiveQueueWithoutCallback() {
    appLayer->setReceiveCallback(nullptr);
    dlLayer->setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);

    datalink::LDataFrame rx;
    rx.source = IndividualAddress(2, 2, 2);
    rx.destination = GroupAddress(1, 2, 3);
    rx.destinationType = AddressType::Group;
    rx.ackRequested = false;
    rx.setTpdu(knx::protocol::TPCI::UnnumberedData,
               application::APCIField::create(APCIService::GroupValueWrite, 0x01),
               {});
    physLayer->injectFrame(encodeTp1(rx));

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedReceiveCount()));

    ADataFrame queued;
    TEST_ASSERT_TRUE(appLayer->popReceivedFrame(queued));
    TEST_ASSERT_EQUAL(IndividualAddress(2, 2, 2).raw, queued.source.raw);
    TEST_ASSERT_EQUAL(GroupAddress(1, 2, 3).raw, queued.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(queued.service));
    TEST_ASSERT_TRUE(queued.destinationType == AddressType::Group);
    TEST_ASSERT_EQUAL(1, static_cast<int>(queued.data.size()));
    TEST_ASSERT_EQUAL(0x01, queued.data[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(appLayer->queuedReceiveCount()));
}

void test_FullStack_InternalResponseSendFailureIsQueued() {
    const uint8_t requestedCount = 3;
    const uint16_t requestedAddr = 0x1234;

    application::MemoryRegion region{
        .startAddress = MemoryAddress(0x1200),
        .size = 0x0100,
        .accessMode = application::MemoryAccessMode::ReadWrite,
        .name = "test"
    };
    TEST_ASSERT_TRUE(appLayer->addressSpace().addRegion(region).isOk());

    appLayer->memoryService().setReadCallback(
        [&](knx::MemoryAddress address, uint8_t len, std::span<uint8_t> data) -> knx::util::Result<void> {
            TEST_ASSERT_EQUAL(requestedAddr, address.raw);
            TEST_ASSERT_EQUAL(requestedCount, len);
            std::fill(data.begin(), data.end(), 0x5A);
            return knx::util::Result<void>::ok();
        }
    );

    physLayer->close();

    datalink::LDataFrame rx;
    rx.source = IndividualAddress(1, 1, 2);
    rx.destination = GroupAddress(ownAddress.raw);
    rx.destinationType = AddressType::Individual;
    rx.ackRequested = false;
    rx.setTpdu(knx::protocol::TPCI::UnnumberedData,
               application::APCIField::create(APCIService::MemoryRead, requestedCount),
               {static_cast<uint8_t>((requestedAddr >> 8) & 0xFF), static_cast<uint8_t>(requestedAddr & 0xFF)});

    physLayer->injectFrame(encodeTp1(rx));

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));

    ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_TRUE(outcome.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL(rx.source.raw, outcome.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::MemoryResponse), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::TransmissionFailed), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(1u, outcome.attempts);
}

void test_FullStack_InternalResponseRetriesTransmissionFailureThenSucceeds() {
    const uint8_t requestedCount = 3;
    const uint16_t requestedAddr = 0x1234;

    application::MemoryRegion region{
        .startAddress = MemoryAddress(0x1200),
        .size = 0x0100,
        .accessMode = application::MemoryAccessMode::ReadWrite,
        .name = "test"
    };
    TEST_ASSERT_TRUE(appLayer->addressSpace().addRegion(region).isOk());

    appLayer->memoryService().setReadCallback(
        [&](knx::MemoryAddress address, uint8_t len, std::span<uint8_t> data) -> knx::util::Result<void> {
            TEST_ASSERT_EQUAL(requestedAddr, address.raw);
            TEST_ASSERT_EQUAL(requestedCount, len);
            std::fill(data.begin(), data.end(), 0x6B);
            return knx::util::Result<void>::ok();
        }
    );

    knx::application::SendOptions options;
    options.maxAttempts = 2u;
    options.retryOnTransmissionFailed = true;
    appLayer->setServiceResponseSendOptions(options);

    physLayer->queueSendResult(knx::util::ErrorCode::TransmissionFailed);
    physLayer->queueSendResult(knx::util::ErrorCode::Success);

    datalink::LDataFrame rx;
    rx.source = IndividualAddress(1, 1, 2);
    rx.destination = GroupAddress(ownAddress.raw);
    rx.destinationType = AddressType::Individual;
    rx.ackRequested = false;
    rx.setTpdu(knx::protocol::TPCI::UnnumberedData,
               application::APCIField::create(APCIService::MemoryRead, requestedCount),
               {static_cast<uint8_t>((requestedAddr >> 8) & 0xFF), static_cast<uint8_t>(requestedAddr & 0xFF)});

    physLayer->injectFrame(encodeTp1(rx));

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));

    ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_TRUE(outcome.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL(rx.source.raw, outcome.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::MemoryResponse), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_FullStack_SendDataProgressionSeam() {
    auto beginResult = appLayer->beginSendData(GroupAddress(1, 2, 3),
                                               APCIService::GroupValueWrite,
                                               std::vector<uint8_t>{0x2A},
                                               AddressType::Group);
    TEST_ASSERT_TRUE(beginResult.isOk());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));

    auto progress = appLayer->pollSendData();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(ApplicationLayer::SendProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_TRUE(physLayer->sentFrameCount() >= 1);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));

    ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(1u, outcome.attempts);
}

void test_FullStack_SendDataProgressionRetriesTransmissionFailureThenSucceeds() {
    physLayer->queueSendResult(knx::util::ErrorCode::TransmissionFailed);
    physLayer->queueSendResult(knx::util::ErrorCode::Success);

    knx::application::SendOptions options;
    options.maxAttempts = 2u;
    options.retryOnTransmissionFailed = true;

    auto beginResult = appLayer->beginSendData(GroupAddress(1, 2, 3),
                                               APCIService::GroupValueWrite,
                                               std::vector<uint8_t>{0x2B},
                                               AddressType::Group,
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
    ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_FullStack_GroupValueWriteUsesDefaultSendPolicyPreset() {
    physLayer->queueSendResult(knx::util::ErrorCode::Timeout);
    physLayer->queueSendResult(knx::util::ErrorCode::Success);

    appLayer->setDefaultSendOptions(
        ApplicationLayer::optionsForPreset(knx::application::SendPolicyPreset::RetryTransientOnce));

    auto result = appLayer->sendGroupValueWrite(GroupAddress(1, 2, 3), std::vector<uint8_t>{0x2C});
    TEST_ASSERT_TRUE(result.isOk());

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(appLayer->queuedSendOutcomeCount()));
    ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_FullStack_BeginSendGroupValueWriteUsesDefaultSendPolicyPreset() {
    physLayer->queueSendResult(knx::util::ErrorCode::Timeout);
    physLayer->queueSendResult(knx::util::ErrorCode::Success);

    appLayer->setDefaultSendOptions(
        ApplicationLayer::optionsForPreset(knx::application::SendPolicyPreset::RetryTransientOnce));

    auto beginResult = appLayer->beginSendGroupValueWrite(GroupAddress(1, 2, 3), std::vector<uint8_t>{0x2D});
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
    ASendOutcome outcome;
    TEST_ASSERT_TRUE(appLayer->popSendOutcome(outcome));
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::GroupValueWrite), static_cast<uint16_t>(outcome.service));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

int main() {
    UNITY_BEGIN();
    
    RUN_TEST(test_FullStack_Initialization);
    RUN_TEST(test_FullStack_DeviceDescriptor);
    RUN_TEST(test_FullStack_MemoryService);
    RUN_TEST(test_FullStack_AuthorizationService);
    RUN_TEST(test_FullStack_RestartService);
    RUN_TEST(test_FullStack_MultipleServices);
    RUN_TEST(test_FullStack_APCIServiceCodes);
    RUN_TEST(test_FullStack_ServiceHandlers);
    RUN_TEST(test_FullStack_SendData);
    RUN_TEST(test_FullStack_AddressTypes);
    RUN_TEST(test_FullStack_DescriptorEncodeDecode);
    RUN_TEST(test_FullStack_AuthorizationLevels);
    RUN_TEST(test_FullStack_RestartTypes);
    RUN_TEST(test_FullStack_CompleteCoverage);
    RUN_TEST(test_FullStack_ApplicationReceiveQueueWithoutCallback);
    RUN_TEST(test_FullStack_InternalResponseSendFailureIsQueued);
    RUN_TEST(test_FullStack_InternalResponseRetriesTransmissionFailureThenSucceeds);
    RUN_TEST(test_FullStack_SendDataProgressionSeam);
    RUN_TEST(test_FullStack_SendDataProgressionRetriesTransmissionFailureThenSucceeds);
    RUN_TEST(test_FullStack_GroupValueWriteUsesDefaultSendPolicyPreset);
    RUN_TEST(test_FullStack_BeginSendGroupValueWriteUsesDefaultSendPolicyPreset);
    
    return UNITY_END();
}
