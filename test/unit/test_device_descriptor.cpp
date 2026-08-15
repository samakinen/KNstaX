// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_device_descriptor.cpp
 * @brief Unit tests for device descriptor service
 */

#include "knx/application/device_descriptor.hpp"
#include "knx/application/device_descriptor_service.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/protocol/tpdu_codec.hpp"

#include <unity.h>

#include <array>
#include <vector>

using namespace knx;
using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// Device Descriptor Tests
// ============================================================================

void test_DescriptorType0_Encoding(void) {
    // Descriptor type 0 IS the mask version (KNX 03_05_01), e.g. 0x07B0.
    DeviceDescriptorType0 desc;
    desc.maskHigh = 0x07;
    desc.maskLow = 0xB0;

    auto encoded = desc.encode();
    TEST_ASSERT_EQUAL(2, encoded.size());
    TEST_ASSERT_EQUAL(0x07, encoded[0]);
    TEST_ASSERT_EQUAL(0xB0, encoded[1]);
}

void test_DescriptorType0_Decoding(void) {
    std::array<uint8_t, 2> data = {0x07, 0xB0}; // mask version 0x07B0

    auto desc = DeviceDescriptorType0::decode(data);
    TEST_ASSERT_EQUAL(0x07, desc.maskHigh);
    TEST_ASSERT_EQUAL(0xB0, desc.maskLow);
}

void test_DescriptorType2_Encoding(void) {
    DeviceDescriptorType2 desc;
    desc.mediumType = knx::MediumType::IP;
    desc.firmwareVersion = 1;
    desc.deviceType = knx::DeviceType(0x1234);
    desc.applicationVersion = 0x5678;
    desc.linkMgmtProcedures = 0xABCD;
    desc.reserved.fill(0xFF);

    auto encoded = desc.encode();
    TEST_ASSERT_EQUAL(14, encoded.size());
    TEST_ASSERT_EQUAL(0x20, encoded[0]); // IP = 0x20
    TEST_ASSERT_EQUAL(1, encoded[1]);
    TEST_ASSERT_EQUAL(0x12, encoded[2]);
    TEST_ASSERT_EQUAL(0x34, encoded[3]);
    TEST_ASSERT_EQUAL(0x56, encoded[4]);
    TEST_ASSERT_EQUAL(0x78, encoded[5]);
    TEST_ASSERT_EQUAL(0xAB, encoded[6]);
    TEST_ASSERT_EQUAL(0xCD, encoded[7]);
    TEST_ASSERT_EQUAL(0xFF, encoded[8]);
}

void test_DescriptorType2_Decoding(void) {
    std::array<uint8_t, 14> data = {
        0x02, 10,           // TP1=0x02, fw=10
        0x11, 0x22,         // device type
        0x33, 0x44,         // app version
        0x55, 0x66,         // link mgmt
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    auto desc = DeviceDescriptorType2::decode(data);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(knx::MediumType::TP1), static_cast<uint8_t>(desc.mediumType));
    TEST_ASSERT_EQUAL(10, desc.firmwareVersion);
    TEST_ASSERT_EQUAL(0x1122, desc.deviceType.value());
    TEST_ASSERT_EQUAL(0x3344, desc.applicationVersion);
    TEST_ASSERT_EQUAL(0x5566, desc.linkMgmtProcedures);
}

void test_DescriptorDefault(void) {
    auto desc = DeviceDescriptor::createDefault();

    TEST_ASSERT_EQUAL(0x07, desc.type0.maskHigh); // System B, TP1 = 0x07B0
    TEST_ASSERT_EQUAL(0xB0, desc.type0.maskLow);
    TEST_ASSERT_EQUAL(knx::MediumType::TP1, desc.type2.mediumType);
    TEST_ASSERT_EQUAL(0x0000, desc.type2.deviceType.value());
    TEST_ASSERT_EQUAL(0x0001, desc.type2.applicationVersion);
}

// ============================================================================
// Device Descriptor Service Tests
// ============================================================================

void test_DeviceDescriptorService_ReadType0(void) {
    auto desc = DeviceDescriptor::createDefault();
    desc.type0.maskHigh = 0x27; // e.g. RF-mask 0x27xx family
    desc.type0.maskLow = 0x20;

    DeviceDescriptorService service(desc);

    bool responseSent = false;
    std::vector<uint8_t> responseData;

    service.setResponseCallback([&](const knx::IndividualAddress&, uint8_t type, std::span<const uint8_t> data) {
        responseSent = true;
        TEST_ASSERT_EQUAL(0, type);
        TEST_ASSERT_EQUAL(2, data.size());
        responseData.assign(data.begin(), data.end());
    });

    knx::IndividualAddress source(1, 2, 3);
    TEST_ASSERT_TRUE(service.handleReadRequest(source, 0).isOk());
    TEST_ASSERT_TRUE(responseSent);
    TEST_ASSERT_EQUAL(0x27, responseData[0]);
    TEST_ASSERT_EQUAL(0x20, responseData[1]);
}

void test_DeviceDescriptorService_ReadType2(void) {
    auto desc = DeviceDescriptor::createDefault();
    desc.type2.deviceType = knx::DeviceType(0xCAFE);
    desc.type2.applicationVersion = 0xBEEF;

    DeviceDescriptorService service(desc);

    bool responseSent = false;
    std::vector<uint8_t> responseData;

    service.setResponseCallback([&](const knx::IndividualAddress&, uint8_t type, std::span<const uint8_t> data) {
        responseSent = true;
        TEST_ASSERT_EQUAL(2, type);
        TEST_ASSERT_EQUAL(14, data.size());
        responseData.assign(data.begin(), data.end());
    });

    knx::IndividualAddress source(1, 2, 3);
    TEST_ASSERT_TRUE(service.handleReadRequest(source, 2).isOk());
    TEST_ASSERT_TRUE(responseSent);

    uint16_t deviceType = (responseData[2] << 8) | responseData[3];
    TEST_ASSERT_EQUAL(0xCAFE, deviceType);
    uint16_t appVersion = (responseData[4] << 8) | responseData[5];
    TEST_ASSERT_EQUAL(0xBEEF, appVersion);
}

void test_DeviceDescriptorService_InvalidType(void) {
    auto desc = DeviceDescriptor::createDefault();
    DeviceDescriptorService service(desc);

    service.setResponseCallback([](const knx::IndividualAddress&, uint8_t, std::span<const uint8_t>) {
        TEST_FAIL_MESSAGE("Should not send response for invalid type");
    });

    knx::IndividualAddress source(1, 2, 3);
    TEST_ASSERT_FALSE(service.handleReadRequest(source, 99).isOk());
}

void test_DeviceDescriptorService_SupportMatrix(void) {
    TEST_ASSERT_TRUE(DeviceDescriptorService::isDescriptorTypeSupported(0));
    TEST_ASSERT_FALSE(DeviceDescriptorService::isDescriptorTypeSupported(1));
    TEST_ASSERT_TRUE(DeviceDescriptorService::isDescriptorTypeSupported(2));
    TEST_ASSERT_FALSE(DeviceDescriptorService::isDescriptorTypeSupported(3));
    TEST_ASSERT_FALSE(DeviceDescriptorService::isDescriptorTypeSupported(99));

    TEST_ASSERT_EQUAL_UINT32(2u, DeviceDescriptorService::encodedDescriptorSize(0));
    TEST_ASSERT_EQUAL_UINT32(0u, DeviceDescriptorService::encodedDescriptorSize(1));
    TEST_ASSERT_EQUAL_UINT32(14u, DeviceDescriptorService::encodedDescriptorSize(2));
    TEST_ASSERT_EQUAL_UINT32(0u, DeviceDescriptorService::encodedDescriptorSize(3));
    TEST_ASSERT_EQUAL_UINT32(0u, DeviceDescriptorService::encodedDescriptorSize(99));
}

void test_DeviceDescriptorService_EncodeReadRequest(void) {
    std::array<uint8_t, 2> data{};
    auto result = DeviceDescriptorService::encodeReadRequest(2, data);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(2, result.value());

    const auto hdr = knx::protocol::unpackTpduHeader(data[0], data[1]);
    TEST_ASSERT_EQUAL(APCIService::DeviceDescriptorRead, hdr.apci.service());
    TEST_ASSERT_EQUAL(2, hdr.apci.data6());
}

void test_DeviceDescriptorService_DecodeReadRequest(void) {
    std::vector<uint8_t> data = {0x03, 0x02}; // APCI + type 2

    uint8_t descType = DeviceDescriptorService::decodeReadRequest(data);
    TEST_ASSERT_EQUAL(2, descType);
}

void test_DeviceDescriptorService_EncodeResponse(void) {
    std::array<uint8_t, 4> descData = {0x00, 0x05, 0xAA, 0xBB};
    std::array<uint8_t, 6> data{};

    auto result = DeviceDescriptorService::encodeResponse(2, descData, data);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(6, result.value());

    const auto hdr = knx::protocol::unpackTpduHeader(data[0], data[1]);
    TEST_ASSERT_EQUAL(APCIService::DeviceDescriptorResponse, hdr.apci.service());
    TEST_ASSERT_EQUAL(2, hdr.apci.data6());

    // Payload is descriptor data
    TEST_ASSERT_EQUAL(0x00, data[2]);
    TEST_ASSERT_EQUAL(0x05, data[3]);
    TEST_ASSERT_EQUAL(0xAA, data[4]);
    TEST_ASSERT_EQUAL(0xBB, data[5]);
}

void test_DeviceDescriptorService_DecodeResponse(void) {
    std::vector<uint8_t> data = {0x03, 0x42, 0x11, 0x22, 0x33}; // APCI + type 2 + data

    uint8_t descType = 0;
    std::array<uint8_t, 3> descData{};
    auto result = DeviceDescriptorService::decodeResponse(data, descType, descData);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(3, result.value());
    TEST_ASSERT_EQUAL(2, descType);
    TEST_ASSERT_EQUAL(0x11, descData[0]);
    TEST_ASSERT_EQUAL(0x22, descData[1]);
    TEST_ASSERT_EQUAL(0x33, descData[2]);
}

// ============================================================================
// APCI Service Tests
// ============================================================================

void test_APCI_ExtractService(void) {
    application::APCIField apci(0x0302); // DeviceDescriptorRead + type
    APCIService service = apci.service();
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::DeviceDescriptorRead), static_cast<uint16_t>(service));
}

void test_APCI_CreateAPCI(void) {
    auto apci = APCIField::create(APCIService::MemoryRead, 0x2A);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(APCIService::MemoryRead) | 0x2A, apci.raw);
}

void test_APCI_IsExtended(void) {
    TEST_ASSERT_FALSE(application::APCIField(0x0000).isExtended()); // GroupValueRead
    TEST_ASSERT_FALSE(application::APCIField(0x0300).isExtended()); // DeviceDescriptorRead
    TEST_ASSERT_TRUE(application::APCIField(0x03C0).isExtended());  // Escape
}

// ============================================================================
// Test Runner
// ============================================================================

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_DescriptorType0_Encoding);
    RUN_TEST(test_DescriptorType0_Decoding);
    RUN_TEST(test_DescriptorType2_Encoding);
    RUN_TEST(test_DescriptorType2_Decoding);
    RUN_TEST(test_DescriptorDefault);

    RUN_TEST(test_DeviceDescriptorService_ReadType0);
    RUN_TEST(test_DeviceDescriptorService_ReadType2);
    RUN_TEST(test_DeviceDescriptorService_InvalidType);
    RUN_TEST(test_DeviceDescriptorService_SupportMatrix);
    RUN_TEST(test_DeviceDescriptorService_EncodeReadRequest);
    RUN_TEST(test_DeviceDescriptorService_DecodeReadRequest);
    RUN_TEST(test_DeviceDescriptorService_EncodeResponse);
    RUN_TEST(test_DeviceDescriptorService_DecodeResponse);

    RUN_TEST(test_APCI_ExtractService);
    RUN_TEST(test_APCI_CreateAPCI);
    RUN_TEST(test_APCI_IsExtended);

    return UNITY_END();
}
