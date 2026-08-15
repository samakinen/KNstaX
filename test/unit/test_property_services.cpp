// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_property_services.cpp
 * @brief Unit tests for property services
 */

#include "knx/application/property.hpp"
#include "knx/application/property_store.hpp"
#include "knx/application/property_services.hpp"
#include <unity.h>
#include <vector>

using namespace knx::application;
using namespace knx;

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// Property Tests
// ============================================================================

void test_PropertyValue_CreateFromUInt8(void) {
    auto prop = PropertyValue::fromUInt8(PropertyID::ProgMode, 1);
    
    TEST_ASSERT_EQUAL(PropertyID::ProgMode, prop.id);
    TEST_ASSERT_EQUAL(PropertyDataType::UnsignedChar, prop.type);
    TEST_ASSERT_EQUAL(1, prop.data.size());
    TEST_ASSERT_EQUAL(1, prop.data[0]);
    TEST_ASSERT_EQUAL(1, prop.asUInt8());
}

void test_PropertyValue_CreateFromUInt16(void) {
    auto prop = PropertyValue::fromUInt16(PropertyID::ManufacturerId, 0x1234);
    
    TEST_ASSERT_EQUAL(PropertyDataType::UnsignedInt, prop.type);
    TEST_ASSERT_EQUAL(2, prop.data.size());
    TEST_ASSERT_EQUAL(0x12, prop.data[0]);
    TEST_ASSERT_EQUAL(0x34, prop.data[1]);
    TEST_ASSERT_EQUAL(0x1234, prop.asUInt16());
}

void test_PropertyValue_CreateFromUInt32(void) {
    auto prop = PropertyValue::fromUInt32(PropertyID::SerialNumber, 0x12345678);
    
    TEST_ASSERT_EQUAL(PropertyDataType::UnsignedLong, prop.type);
    TEST_ASSERT_EQUAL(4, prop.data.size());
    TEST_ASSERT_EQUAL(0x12345678, prop.asUInt32());
}

void test_PropertyDescriptor_GetElementSize(void) {
    PropertyDescriptor desc;
    
    desc.type = PropertyDataType::UnsignedChar;
    TEST_ASSERT_EQUAL(1, desc.getElementSize());
    
    desc.type = PropertyDataType::UnsignedInt;
    TEST_ASSERT_EQUAL(2, desc.getElementSize());
    
    desc.type = PropertyDataType::UnsignedLong;
    TEST_ASSERT_EQUAL(4, desc.getElementSize());
    
    desc.type = PropertyDataType::Double;
    TEST_ASSERT_EQUAL(8, desc.getElementSize());
}

// ============================================================================
// PropertyStore Tests
// ============================================================================

void test_PropertyStore_RegisterAndRead(void) {
    PropertyStore store(InterfaceObjectType::device(), InterfaceObjectIndex(0)); // Device object, index 0
    
    PropertyDescriptor desc;
    desc.id = PropertyID::ManufacturerId;
    desc.type = PropertyDataType::UnsignedInt;
    desc.access = PropertyAccess::ReadOnly;
    desc.maxElements = 1;
    desc.readLevel = 0;
    desc.writeLevel = 15;
    
    auto value = PropertyValue::fromUInt16(PropertyID::ManufacturerId, 0xABCD);
    
    TEST_ASSERT_TRUE(store.registerProperty(desc, value).isOk());
    
    // Read property
    std::vector<uint8_t> data(2);
    auto readRes = store.readProperty(PropertyID::ManufacturerId, 1, 1, data);
    TEST_ASSERT_TRUE(readRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(2, readRes.value());

    uint16_t readValue = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    TEST_ASSERT_EQUAL(0xABCD, readValue);
}

void test_PropertyStore_WriteProperty(void) {
    PropertyStore store(InterfaceObjectType::device(), InterfaceObjectIndex(0));
    
    PropertyDescriptor desc;
    desc.id = PropertyID::ProgMode;
    desc.type = PropertyDataType::UnsignedChar;
    desc.access = PropertyAccess::ReadWrite;
    desc.maxElements = 1;
    desc.readLevel = 0;
    desc.writeLevel = 0;
    
    auto value = PropertyValue::fromUInt8(PropertyID::ProgMode, 0);
    
    TEST_ASSERT_TRUE(store.registerProperty(desc, value).isOk());
    
    // Write property
    std::vector<uint8_t> newData = {1};
    TEST_ASSERT_TRUE(store.writeProperty(PropertyID::ProgMode, 1, 1, newData).isOk());
    
    // Read back
    std::vector<uint8_t> readData(1);
    auto readRes = store.readProperty(PropertyID::ProgMode, 1, 1, readData);
    TEST_ASSERT_TRUE(readRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(1, readRes.value());
    TEST_ASSERT_EQUAL(1, readData[0]);
}

void test_PropertyStore_ReadOnlyProtection(void) {
    PropertyStore store(InterfaceObjectType::device(), InterfaceObjectIndex(0));
    
    PropertyDescriptor desc;
    desc.id = PropertyID::SerialNumber;
    desc.type = PropertyDataType::UnsignedLong;
    desc.access = PropertyAccess::ReadOnly;
    desc.maxElements = 1;
    
    auto value = PropertyValue::fromUInt32(PropertyID::SerialNumber, 0x12345678);
    TEST_ASSERT_TRUE(store.registerProperty(desc, value).isOk());
    
    // Try to write - should fail
    std::vector<uint8_t> newData = {0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_TRUE(store.writeProperty(PropertyID::SerialNumber, 1, 1, newData).isError());
    
    // Verify original value unchanged
    std::vector<uint8_t> readData(4);
    auto readRes = store.readProperty(PropertyID::SerialNumber, 1, 1, readData);
    TEST_ASSERT_TRUE(readRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(4, readRes.value());
    TEST_ASSERT_EQUAL(0x12345678,
        (static_cast<uint32_t>(readData[0]) << 24) |
        (static_cast<uint32_t>(readData[1]) << 16) |
        (static_cast<uint32_t>(readData[2]) << 8) |
        readData[3]);
}

void test_PropertyStore_MultiElement(void) {
    PropertyStore store(InterfaceObjectType::device(), InterfaceObjectIndex(0));
    
    PropertyDescriptor desc;
    desc.id = PropertyID::Table;
    desc.type = PropertyDataType::UnsignedChar;
    desc.access = PropertyAccess::ReadWrite;
    desc.maxElements = 10;
    
    // Initialize with zeros
    std::vector<uint8_t> initialData(10, 0);
    auto value = PropertyValue::create(PropertyID::Table, PropertyDataType::UnsignedChar, initialData);
    value.elementCount = 10;
    
    TEST_ASSERT_TRUE(store.registerProperty(desc, value).isOk());
    
    // Write to elements 3-5 (indices 3, 4, 5)
    std::vector<uint8_t> writeData = {0xAA, 0xBB, 0xCC};
    TEST_ASSERT_TRUE(store.writeProperty(PropertyID::Table, 3, 3, writeData).isOk());
    
    // Read back elements 3-5
    std::vector<uint8_t> readData(3);
    auto readRes = store.readProperty(PropertyID::Table, 3, 3, readData);
    TEST_ASSERT_TRUE(readRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(3, readRes.value());
    TEST_ASSERT_EQUAL(0xAA, readData[0]);
    TEST_ASSERT_EQUAL(0xBB, readData[1]);
    TEST_ASSERT_EQUAL(0xCC, readData[2]);
}

void test_PropertyStore_InvalidIndex(void) {
    PropertyStore store(InterfaceObjectType::device(), InterfaceObjectIndex(0));
    
    PropertyDescriptor desc;
    desc.id = PropertyID::ProgMode;
    desc.type = PropertyDataType::UnsignedChar;
    desc.access = PropertyAccess::ReadWrite;
    desc.maxElements = 1;
    
    auto value = PropertyValue::fromUInt8(PropertyID::ProgMode, 0);
    TEST_ASSERT_TRUE(store.registerProperty(desc, value).isOk());
    
    // Try to read with invalid index (0 or > maxElements)
    std::vector<uint8_t> data(1);
    auto readRes = store.readProperty(PropertyID::ProgMode, 0, 1, data);
    TEST_ASSERT_TRUE(readRes.isError());

    readRes = store.readProperty(PropertyID::ProgMode, 2, 1, data);
    TEST_ASSERT_TRUE(readRes.isError());
}

void test_PropertyStore_GetAllPropertyIds_BufferHandling(void) {
    PropertyStore store(InterfaceObjectType::device(), InterfaceObjectIndex(0));

    PropertyDescriptor progMode{};
    progMode.id = PropertyID::ProgMode;
    progMode.type = PropertyDataType::UnsignedChar;
    progMode.access = PropertyAccess::ReadWrite;
    progMode.maxElements = 1;

    PropertyDescriptor manufacturerId{};
    manufacturerId.id = PropertyID::ManufacturerId;
    manufacturerId.type = PropertyDataType::UnsignedInt;
    manufacturerId.access = PropertyAccess::ReadOnly;
    manufacturerId.maxElements = 1;

    TEST_ASSERT_TRUE(store.registerProperty(progMode, PropertyValue::fromUInt8(PropertyID::ProgMode, 1)).isOk());
    TEST_ASSERT_TRUE(store.registerProperty(manufacturerId, PropertyValue::fromUInt16(PropertyID::ManufacturerId, 0x1234)).isOk());

    auto neededRes = store.getAllPropertyIds(std::span<PropertyID>{});
    TEST_ASSERT_TRUE(neededRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(2, neededRes.value());

    std::vector<PropertyID> tooSmall(1);
    auto tooSmallRes = store.getAllPropertyIds(std::span<PropertyID>{tooSmall});
    TEST_ASSERT_TRUE(tooSmallRes.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::BufferTooSmall, tooSmallRes.error());

    std::vector<PropertyID> ids(neededRes.value());
    auto fillRes = store.getAllPropertyIds(std::span<PropertyID>{ids});
    TEST_ASSERT_TRUE(fillRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(2, fillRes.value());
    TEST_ASSERT_EQUAL(PropertyID::ManufacturerId, ids[0]);
    TEST_ASSERT_EQUAL(PropertyID::ProgMode, ids[1]);
}

// ============================================================================
// PropertyStoreManager Tests
// ============================================================================

void test_PropertyStoreManager_AddObject(void) {
    PropertyStoreManager manager;
    
    auto* store = manager.addObject(InterfaceObjectType::device(), InterfaceObjectIndex(0)); // Device object
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_EQUAL_UINT16(InterfaceObjectType::device().value(),
                             static_cast<uint16_t>(store->getObjectType().value()));
    TEST_ASSERT_EQUAL(0, store->getObjectIndex().value());
    
    TEST_ASSERT_EQUAL(1, manager.getObjectCount());
}

void test_PropertyStoreManager_GetObject(void) {
    PropertyStoreManager manager;
    
    manager.addObject(InterfaceObjectType::addressTable(), InterfaceObjectIndex(5));
    manager.addObject(InterfaceObjectType::associationTable(), InterfaceObjectIndex(10));
    
    auto* store = manager.getObject(InterfaceObjectIndex(5));
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_EQUAL_UINT16(InterfaceObjectType::addressTable().value(),
                             static_cast<uint16_t>(store->getObjectType().value()));
    
    store = manager.getObject(InterfaceObjectIndex(10));
    TEST_ASSERT_NOT_NULL(store);
    TEST_ASSERT_EQUAL_UINT16(InterfaceObjectType::associationTable().value(),
                             static_cast<uint16_t>(store->getObjectType().value()));
    
    store = manager.getObject(InterfaceObjectIndex(99));
    TEST_ASSERT_NULL(store);
}

void test_PropertyStoreManager_ReadWrite(void) {
    PropertyStoreManager manager;
    
    auto* store = manager.addObject(InterfaceObjectType::device(), InterfaceObjectIndex(0));
    
    PropertyDescriptor desc;
    desc.id = PropertyID::ProgMode;
    desc.type = PropertyDataType::UnsignedChar;
    desc.access = PropertyAccess::ReadWrite;
    desc.maxElements = 1;
    
    auto value = PropertyValue::fromUInt8(PropertyID::ProgMode, 0);
    TEST_ASSERT_TRUE(store->registerProperty(desc, value).isOk());
    
    // Write via manager
    std::vector<uint8_t> writeData = {1};
    TEST_ASSERT_TRUE(manager.writeProperty(InterfaceObjectIndex(0), PropertyID::ProgMode, 1, 1, writeData).isOk());
    
    // Read via manager
    std::vector<uint8_t> readData(1);
    auto readRes = manager.readProperty(InterfaceObjectIndex(0), PropertyID::ProgMode, 1, 1, readData);
    TEST_ASSERT_TRUE(readRes.isOk());
    TEST_ASSERT_EQUAL(1, readData[0]);
}

void test_PropertyStoreManager_GetAllObjectIndices(void) {
    PropertyStoreManager manager;

    manager.addObject(InterfaceObjectType::associationTable(), InterfaceObjectIndex(10));
    manager.addObject(InterfaceObjectType::device(), InterfaceObjectIndex(0));

    auto sizeRes = manager.getAllObjectIndices({});
    TEST_ASSERT_TRUE(sizeRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(2, sizeRes.value());

    InterfaceObjectIndex tooSmall[1]{};
    auto smallRes = manager.getAllObjectIndices(tooSmall);
    TEST_ASSERT_TRUE(smallRes.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::BufferTooSmall), static_cast<int>(smallRes.error()));

    InterfaceObjectIndex indices[2]{};
    auto fillRes = manager.getAllObjectIndices(indices);
    TEST_ASSERT_TRUE(fillRes.isOk());
    TEST_ASSERT_EQUAL_UINT32(2, fillRes.value());
    TEST_ASSERT_EQUAL(0, indices[0].value());
    TEST_ASSERT_EQUAL(10, indices[1].value());
}

// ============================================================================
// PropertyServices Tests
// ============================================================================

void test_PropertyServices_ValueRead(void) {
    PropertyStoreManager manager;
    auto* store = manager.addObject(InterfaceObjectType::device(), InterfaceObjectIndex(0));
    
    PropertyDescriptor desc;
    desc.id = PropertyID::ManufacturerId;
    desc.type = PropertyDataType::UnsignedInt;
    desc.access = PropertyAccess::ReadOnly;
    desc.maxElements = 1;
    
    auto value = PropertyValue::fromUInt16(PropertyID::ManufacturerId, 0x1234);
    TEST_ASSERT_TRUE(store->registerProperty(desc, value).isOk());
    
    PropertyServices service(manager);
    
    bool responseSent = false;
    PropertyValueResponse receivedResponse;
    
    service.setValueResponseCallback([&](const IndividualAddress& dest, const PropertyValueResponse& response) {
        responseSent = true;
        receivedResponse = response;
    });
    
    PropertyValueReadRequest request;
    request.objectIndex = InterfaceObjectIndex(0);
    request.propertyId = PropertyID::ManufacturerId;
    request.startIndex = 1;
    request.elementCount = 1;
    
    IndividualAddress source(1, 2, 3);
    TEST_ASSERT_TRUE(service.handleValueRead(source, request).isOk());
    TEST_ASSERT_TRUE(responseSent);
    TEST_ASSERT_EQUAL(0, receivedResponse.objectIndex.value());
    TEST_ASSERT_EQUAL(2, receivedResponse.data.size());
    
    uint16_t readValue = (receivedResponse.data[0] << 8) | receivedResponse.data[1];
    TEST_ASSERT_EQUAL(0x1234, readValue);
}

void test_PropertyServices_ValueWrite(void) {
    PropertyStoreManager manager;
    auto* store = manager.addObject(InterfaceObjectType::device(), InterfaceObjectIndex(0));
    
    PropertyDescriptor desc;
    desc.id = PropertyID::ProgMode;
    desc.type = PropertyDataType::UnsignedChar;
    desc.access = PropertyAccess::ReadWrite;
    desc.maxElements = 1;
    
    auto value = PropertyValue::fromUInt8(PropertyID::ProgMode, 0);
    TEST_ASSERT_TRUE(store->registerProperty(desc, value).isOk());
    
    PropertyServices service(manager);
    
    PropertyValueWriteRequest request;
    request.objectIndex = InterfaceObjectIndex(0);
    request.propertyId = PropertyID::ProgMode;
    request.startIndex = 1;
    request.elementCount = 1;
    request.data = {1};
    
    IndividualAddress source(1, 2, 3);
    TEST_ASSERT_TRUE(service.handleValueWrite(source, request).isOk());
    
    // Verify written
    std::vector<uint8_t> readData(1);
    auto readRes = manager.readProperty(InterfaceObjectIndex(0), PropertyID::ProgMode, 1, 1, readData);
    TEST_ASSERT_TRUE(readRes.isOk());
    TEST_ASSERT_EQUAL(1, readData[0]);
}

void test_PropertyServices_EncodeDecodeValueRead(void) {
    PropertyValueReadRequest request;
    request.objectIndex = InterfaceObjectIndex(5);
    request.propertyId = static_cast<PropertyID>(12);
    request.startIndex = 0x0123;
    request.elementCount = 7;

    std::array<uint8_t, PropertyServices::kEncodedValueReadRequestLength> encoded{};
    TEST_ASSERT_TRUE(PropertyServices::encodeValueReadRequest(request, encoded).isOk());

    auto decoded = PropertyServices::decodeValueReadRequest(encoded);
    TEST_ASSERT_EQUAL(5, decoded.objectIndex.value());
    TEST_ASSERT_EQUAL(12, static_cast<uint8_t>(decoded.propertyId));
    TEST_ASSERT_EQUAL(0x0123, decoded.startIndex);
    TEST_ASSERT_EQUAL(7, decoded.elementCount);
}

void test_PropertyServices_EncodeDecodeValueResponse(void) {
    PropertyValueResponse response;
    response.objectIndex = InterfaceObjectIndex(3);
    response.propertyId = static_cast<PropertyID>(8);
    response.startIndex = 0x0045;
    response.elementCount = 4;
    response.data = {0xAA, 0xBB, 0xCC, 0xDD};

    std::array<uint8_t, 16> encoded{};
    auto encodeRes = PropertyServices::encodeValueResponse(response, encoded);
    TEST_ASSERT_TRUE(encodeRes.isOk());

    auto decoded = PropertyServices::decodeValueResponse(
        std::span<const uint8_t>(encoded.data(), encodeRes.value()));
    
    TEST_ASSERT_EQUAL(3, decoded.objectIndex.value());
    TEST_ASSERT_EQUAL(8, static_cast<uint8_t>(decoded.propertyId));
    TEST_ASSERT_EQUAL(0x0045, decoded.startIndex);
    TEST_ASSERT_EQUAL(4, decoded.elementCount);
    TEST_ASSERT_EQUAL(4, decoded.data.size());
    TEST_ASSERT_EQUAL(0xAA, decoded.data[0]);
    TEST_ASSERT_EQUAL(0xDD, decoded.data[3]);
}

void test_PropertyServices_EncodeValueWriteAndDescriptionMessages(void) {
    PropertyValueWriteRequest writeRequest;
    writeRequest.objectIndex = InterfaceObjectIndex(2);
    writeRequest.propertyId = static_cast<PropertyID>(9);
    writeRequest.startIndex = 0x0011;
    writeRequest.elementCount = 2;
    writeRequest.data = {0x10, 0x20};

    std::array<uint8_t, 16> writeEncoded{};
    auto writeRes = PropertyServices::encodeValueWriteRequest(writeRequest, writeEncoded);
    TEST_ASSERT_TRUE(writeRes.isOk());

    auto decodedWrite = PropertyServices::decodeValueWriteRequest(
        std::span<const uint8_t>(writeEncoded.data(), writeRes.value()));
    TEST_ASSERT_EQUAL(2, decodedWrite.objectIndex.value());
    TEST_ASSERT_EQUAL(9, static_cast<uint8_t>(decodedWrite.propertyId));
    TEST_ASSERT_EQUAL(0x0011, decodedWrite.startIndex);
    TEST_ASSERT_EQUAL(2, decodedWrite.elementCount);
    TEST_ASSERT_EQUAL(2, decodedWrite.data.size());
    TEST_ASSERT_EQUAL(0x10, decodedWrite.data[0]);
    TEST_ASSERT_EQUAL(0x20, decodedWrite.data[1]);

    std::array<uint8_t, PropertyServices::kEncodedDescriptionReadRequestLength> descriptionRead{};
    TEST_ASSERT_TRUE(PropertyServices::encodeDescriptionReadRequest(
        InterfaceObjectIndex(4),
        static_cast<PropertyID>(7),
        PropertyIndex(3),
        descriptionRead).isOk());
    TEST_ASSERT_EQUAL_HEX8(0x04, descriptionRead[2]);
    TEST_ASSERT_EQUAL_HEX8(0x07, descriptionRead[3]);
    TEST_ASSERT_EQUAL_HEX8(0x03, descriptionRead[4]);

    std::array<uint8_t, PropertyServices::kEncodedDescriptionResponseLength> descriptionResponse{};
    TEST_ASSERT_TRUE(PropertyServices::encodeDescriptionResponse(
        InterfaceObjectIndex(4),
        static_cast<PropertyID>(7),
        PropertyIndex(3),
        PropertyWriteAccess::Allowed,
        PropertyDataType::UnsignedInt,
        0x0123,
        2,
        1,
        descriptionResponse).isOk());
    TEST_ASSERT_EQUAL_HEX8(0x04, descriptionResponse[2]);
    TEST_ASSERT_EQUAL_HEX8(0x07, descriptionResponse[3]);
    TEST_ASSERT_EQUAL_HEX8(0x03, descriptionResponse[4]);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(PropertyDataType::UnsignedInt) | 0x80, descriptionResponse[5]);
    TEST_ASSERT_EQUAL_HEX8(0x01, descriptionResponse[6]);
    TEST_ASSERT_EQUAL_HEX8(0x23, descriptionResponse[7]);
    TEST_ASSERT_EQUAL_HEX8(0x21, descriptionResponse[8]);
}

// ============================================================================
// Test Runner
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Property Tests
    RUN_TEST(test_PropertyValue_CreateFromUInt8);
    RUN_TEST(test_PropertyValue_CreateFromUInt16);
    RUN_TEST(test_PropertyValue_CreateFromUInt32);
    RUN_TEST(test_PropertyDescriptor_GetElementSize);
    
    // PropertyStore Tests
    RUN_TEST(test_PropertyStore_RegisterAndRead);
    RUN_TEST(test_PropertyStore_WriteProperty);
    RUN_TEST(test_PropertyStore_ReadOnlyProtection);
    RUN_TEST(test_PropertyStore_MultiElement);
    RUN_TEST(test_PropertyStore_InvalidIndex);
    RUN_TEST(test_PropertyStore_GetAllPropertyIds_BufferHandling);
    
    // PropertyStoreManager Tests
    RUN_TEST(test_PropertyStoreManager_AddObject);
    RUN_TEST(test_PropertyStoreManager_GetObject);
    RUN_TEST(test_PropertyStoreManager_ReadWrite);
    RUN_TEST(test_PropertyStoreManager_GetAllObjectIndices);
    
    // PropertyServices Tests
    RUN_TEST(test_PropertyServices_ValueRead);
    RUN_TEST(test_PropertyServices_ValueWrite);
    RUN_TEST(test_PropertyServices_EncodeDecodeValueRead);
    RUN_TEST(test_PropertyServices_EncodeDecodeValueResponse);
    RUN_TEST(test_PropertyServices_EncodeValueWriteAndDescriptionMessages);
    
    return UNITY_END();
}
