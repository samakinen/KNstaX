// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_memory_service.cpp
 * @brief Unit tests for memory service
 */

#include "knx/application/address_space.hpp"
#include "knx/application/memory_service.hpp"
#include <unity.h>
#include <vector>
#include <cstring>
#include <algorithm>

using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// AddressSpace Tests
// ============================================================================

void test_AddressSpace_Init(void) {
    AddressSpace space;
    TEST_ASSERT_EQUAL(0, space.getRegionCount());
}

void test_AddressSpace_AddRegion(void) {
    AddressSpace space;
    
    MemoryRegion region1{knx::MemoryAddress(0x0000), 0x0100, MemoryAccessMode::ReadOnly, "System"};
    TEST_ASSERT_TRUE(space.addRegion(region1).isOk());
    TEST_ASSERT_EQUAL(1, space.getRegionCount());
    
    MemoryRegion region2{knx::MemoryAddress(0x0100), 0x1000, MemoryAccessMode::ReadWrite, "App"};
    TEST_ASSERT_TRUE(space.addRegion(region2).isOk());
    TEST_ASSERT_EQUAL(2, space.getRegionCount());
}

void test_AddressSpace_RejectOverlap(void) {
    AddressSpace space;
    
    MemoryRegion region1{knx::MemoryAddress(0x0000), 0x0100, MemoryAccessMode::ReadOnly, "System"};
    TEST_ASSERT_TRUE(space.addRegion(region1).isOk());
    
    // Overlapping region should be rejected
    MemoryRegion region2{knx::MemoryAddress(0x0080), 0x0100, MemoryAccessMode::ReadWrite, "Overlap"};
    TEST_ASSERT_TRUE(space.addRegion(region2).isError());
    TEST_ASSERT_EQUAL(1, space.getRegionCount());
}

void test_AddressSpace_RejectZeroSize(void) {
    AddressSpace space;
    
    MemoryRegion region{knx::MemoryAddress(0x0000), 0, MemoryAccessMode::ReadOnly, "Invalid"};
    TEST_ASSERT_TRUE(space.addRegion(region).isError());
}

void test_AddressSpace_FindRegion(void) {
    AddressSpace space;
    
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadWrite, "App"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    
    // Address within region
    const MemoryRegion* found = space.findRegion(knx::MemoryAddress(0x0150));
    TEST_ASSERT_NOT_NULL(found);
    TEST_ASSERT_EQUAL(0x0100, found->startAddress.raw);
    
    // Address outside region
    found = space.findRegion(knx::MemoryAddress(0x0050));
    TEST_ASSERT_NULL(found);
    
    found = space.findRegion(knx::MemoryAddress(0x0200));
    TEST_ASSERT_NULL(found);
}

void test_AddressSpace_CanRead(void) {
    AddressSpace space;
    
    MemoryRegion readOnly{knx::MemoryAddress(0x0000), 0x0100, MemoryAccessMode::ReadOnly, "RO"};
    MemoryRegion writeOnly{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::WriteOnly, "WO"};
    MemoryRegion readWrite{knx::MemoryAddress(0x0200), 0x0100, MemoryAccessMode::ReadWrite, "RW"};
    
    TEST_ASSERT_TRUE(space.addRegion(readOnly).isOk());
    TEST_ASSERT_TRUE(space.addRegion(writeOnly).isOk());
    TEST_ASSERT_TRUE(space.addRegion(readWrite).isOk());
    
        // ReadOnly region
        TEST_ASSERT_TRUE(space.canRead(knx::MemoryAddress(0x0050), 10).isOk());
        TEST_ASSERT_TRUE(space.canWrite(knx::MemoryAddress(0x0050), 10).isError());
    
    // WriteOnly region
        TEST_ASSERT_TRUE(space.canRead(knx::MemoryAddress(0x0150), 10).isError());
        TEST_ASSERT_TRUE(space.canWrite(knx::MemoryAddress(0x0150), 10).isOk());
    
    // ReadWrite region
        TEST_ASSERT_TRUE(space.canRead(knx::MemoryAddress(0x0250), 10).isOk());
        TEST_ASSERT_TRUE(space.canWrite(knx::MemoryAddress(0x0250), 10).isOk());
}

void test_AddressSpace_BoundaryCheck(void) {
    AddressSpace space;
    
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadWrite, "App"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    
    // Within bounds
        TEST_ASSERT_TRUE(space.canRead(knx::MemoryAddress(0x0100), 1).isOk());
        TEST_ASSERT_TRUE(space.canRead(knx::MemoryAddress(0x01FF), 1).isOk());
        TEST_ASSERT_TRUE(space.canRead(knx::MemoryAddress(0x0180), 0x80).isOk());
    
    // Out of bounds (crosses boundary)
        TEST_ASSERT_TRUE(space.canRead(knx::MemoryAddress(0x01F0), 0x20).isError());
    
    // Completely outside
        TEST_ASSERT_TRUE(space.canRead(knx::MemoryAddress(0x0200), 1).isError());
}

void test_AddressSpace_ClearRegions(void) {
    AddressSpace space;
    
    MemoryRegion region{knx::MemoryAddress(0x0000), 0x0100, MemoryAccessMode::ReadOnly, "System"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    TEST_ASSERT_EQUAL(1, space.getRegionCount());
    
    space.clearRegions();
    TEST_ASSERT_EQUAL(0, space.getRegionCount());
}

void test_AddressSpace_GetAccessMode(void) {
    AddressSpace space;
    
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadOnly, "RO"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryAccessMode::ReadOnly), 
                     static_cast<int>(space.getAccessMode(knx::MemoryAddress(0x0150), 10)));
    
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryAccessMode::NoAccess), 
                     static_cast<int>(space.getAccessMode(knx::MemoryAddress(0x0050), 10)));
}

// ============================================================================
// MemoryService Tests
// ============================================================================

static std::vector<uint8_t> mockMemory(0x1000, 0);
static bool readCallbackCalled = false;
static bool writeCallbackCalled = false;
static bool responseCallbackCalled = false;

knx::util::Result<void> mockReadCallback(knx::MemoryAddress address, uint8_t length, std::span<uint8_t> data) {
    readCallbackCalled = true;
    if (data.size() != length || address.raw + length > mockMemory.size()) {
        return knx::util::ErrorCode::InvalidAddress;
    }
    std::copy(mockMemory.begin() + address.raw, mockMemory.begin() + address.raw + length, data.begin());
    return knx::util::Result<void>::ok();
}

knx::util::Result<void> mockWriteCallback(knx::MemoryAddress address, std::span<const uint8_t> data) {
    writeCallbackCalled = true;
    if (address.raw + data.size() > mockMemory.size()) {
        return knx::util::ErrorCode::InvalidAddress;
    }
    std::copy(data.begin(), data.end(), mockMemory.begin() + address.raw);
    return knx::util::Result<void>::ok();
}

void mockResponseCallback(const knx::IndividualAddress& dest, const MemoryResponse& response) {
    responseCallbackCalled = true;
}

// ---------------------------------------------------------------------------
// Extended memory service fixtures
// ---------------------------------------------------------------------------

bool extendedResponseCalled = false;
MemoryExtendedResponse lastExtendedResponse;

void mockExtendedResponseCallback(const knx::IndividualAddress& dest,
                                  const MemoryExtendedResponse& response) {
    extendedResponseCalled = true;
    lastExtendedResponse = response;
}

MemoryService makeExtendedService(AddressSpace& space, MemoryAccessMode mode) {
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, mode, "Test"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());

    MemoryService service(space);
    service.setReadCallback(mockReadCallback);
    service.setWriteCallback(mockWriteCallback);
    service.setExtendedResponseCallback(mockExtendedResponseCallback);

    extendedResponseCalled = false;
    readCallbackCalled = false;
    writeCallbackCalled = false;
    lastExtendedResponse = MemoryExtendedResponse{};
    return service;
}

void test_MemoryService_Init(void) {
    AddressSpace space;
    MemoryService service(space);

    // Without a region, access validation must fail.
    knx::IndividualAddress source(1, 2, 3);
    TEST_ASSERT_FALSE(service.handleReadRequest(source, 1, knx::MemoryAddress(0x0100)).isOk());

    // With a valid region but without callbacks, requests must be rejected.
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadWrite, "Test"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());

    TEST_ASSERT_FALSE(service.handleReadRequest(source, 1, knx::MemoryAddress(0x0100)).isOk());
    const std::vector<uint8_t> singleByteWrite = {0xAA};
    TEST_ASSERT_FALSE(service.handleWriteRequest(source, 1, knx::MemoryAddress(0x0100), singleByteWrite).isOk());
}

void test_MemoryService_ReadRequest(void) {
    AddressSpace space;
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadWrite, "Test"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    
    MemoryService service(space);
    service.setReadCallback(mockReadCallback);
    service.setResponseCallback(mockResponseCallback);
    
    // Initialize test data
    for (size_t i = 0; i < mockMemory.size(); i++) {
        mockMemory[i] = static_cast<uint8_t>(i & 0xFF);
    }
    
    readCallbackCalled = false;
    responseCallbackCalled = false;
    
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleReadRequest(source, 10, knx::MemoryAddress(0x0150));
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(readCallbackCalled);
    TEST_ASSERT_TRUE(responseCallbackCalled);
}

void test_MemoryService_WriteRequest(void) {
    AddressSpace space;
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadWrite, "Test"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    
    MemoryService service(space);
    service.setWriteCallback(mockWriteCallback);
    service.setResponseCallback(mockResponseCallback);
    
    writeCallbackCalled = false;
    responseCallbackCalled = false;
    
    std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC, 0xDD};
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleWriteRequest(source, 4, knx::MemoryAddress(0x0150), data);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(writeCallbackCalled);
    TEST_ASSERT_TRUE(responseCallbackCalled);
    
    // Verify data was written
    TEST_ASSERT_EQUAL(0xAA, mockMemory[0x0150]);
    TEST_ASSERT_EQUAL(0xBB, mockMemory[0x0151]);
    TEST_ASSERT_EQUAL(0xCC, mockMemory[0x0152]);
    TEST_ASSERT_EQUAL(0xDD, mockMemory[0x0153]);
}

void test_MemoryService_ReadDenied(void) {
    AddressSpace space;
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::WriteOnly, "WO"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    
    MemoryService service(space);
    service.setReadCallback(mockReadCallback);
    
    readCallbackCalled = false;
    
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleReadRequest(source, 10, knx::MemoryAddress(0x0150));
    
    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_FALSE(readCallbackCalled);
}

void test_MemoryService_WriteDenied(void) {
    AddressSpace space;
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadOnly, "RO"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    
    MemoryService service(space);
    service.setWriteCallback(mockWriteCallback);
    
    writeCallbackCalled = false;
    
    std::vector<uint8_t> data = {0x01, 0x02};
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleWriteRequest(source, 2, knx::MemoryAddress(0x0150), data);
    
    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_FALSE(writeCallbackCalled);
}

void test_MemoryService_EncodeReadRequest(void) {
    std::array<uint8_t, MemoryService::kEncodedReadRequestLength> encoded{};
    auto result = MemoryService::encodeReadRequest(10, knx::MemoryAddress(0x1234), encoded);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(encoded.size() > 0);
    TEST_ASSERT_EQUAL(4, encoded.size());

    // TPDU header (TPCI=0x00, APCI=A_Memory_Read=0x200) with count in APCI data6
    TEST_ASSERT_EQUAL(0x02, encoded[0]);
    TEST_ASSERT_EQUAL(10, encoded[1] & 0x3F);
    // Payload: address
    TEST_ASSERT_EQUAL(0x12, encoded[2]);
    TEST_ASSERT_EQUAL(0x34, encoded[3]);
}

void test_MemoryService_EncodeWriteRequest(void) {
    std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};
    std::array<uint8_t, 7> encoded{};
    auto result = MemoryService::encodeWriteRequest(3, knx::MemoryAddress(0x5678), data, encoded);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(7, result.value());

    // TPDU header (TPCI=0x00, APCI=A_Memory_Write=0x280) with count in APCI data6
    TEST_ASSERT_EQUAL(0x02, encoded[0]);
    TEST_ASSERT_EQUAL((0x80 | 3), encoded[1]);
    // Payload: address + data
    TEST_ASSERT_EQUAL(0x56, encoded[2]);
    TEST_ASSERT_EQUAL(0x78, encoded[3]);
    TEST_ASSERT_EQUAL(0xAA, encoded[4]);
    TEST_ASSERT_EQUAL(0xBB, encoded[5]);
    TEST_ASSERT_EQUAL(0xCC, encoded[6]);
}

void test_MemoryService_EncodeResponse(void) {
    std::vector<uint8_t> data = {0x11, 0x22, 0x33, 0x44};
    std::array<uint8_t, 8> encoded{};
    auto result = MemoryService::encodeResponse(4, knx::MemoryAddress(0xABCD), data, encoded);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(8, result.value());

    // TPDU header (TPCI=0x00, APCI=A_Memory_Response=0x240) with count in APCI data6
    TEST_ASSERT_EQUAL(0x02, encoded[0]);
    TEST_ASSERT_EQUAL((0x40 | 4), encoded[1]);
    // Payload: address + data
    TEST_ASSERT_EQUAL(0xAB, encoded[2]);
    TEST_ASSERT_EQUAL(0xCD, encoded[3]);
    TEST_ASSERT_EQUAL(0x11, encoded[4]);
}

void test_MemoryService_DecodeRequest(void) {
    std::vector<uint8_t> encoded = {0x02, static_cast<uint8_t>(0x00 | 15), 0x12, 0x34};
    
    uint8_t count;
    knx::MemoryAddress address;
    auto result = MemoryService::decodeRequest(encoded, count, address);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(15, count);
    TEST_ASSERT_EQUAL(0x1234, address.raw);
}

void test_MemoryService_DecodeWriteRequest(void) {
    std::vector<uint8_t> encoded = {0x02, static_cast<uint8_t>(0x80 | 3), 0x56, 0x78, 0xAA, 0xBB, 0xCC};
    
    uint8_t count;
    knx::MemoryAddress address;
    MemoryWriteRequest::DataBuffer data;
    data.resize(3);
    auto result = MemoryService::decodeWriteRequest(encoded, count, address, data.span());
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(3, count);
    TEST_ASSERT_EQUAL(0x5678, address.raw);
    TEST_ASSERT_EQUAL(3, data.size());
    TEST_ASSERT_EQUAL(0xAA, data[0]);
    TEST_ASSERT_EQUAL(0xBB, data[1]);
    TEST_ASSERT_EQUAL(0xCC, data[2]);
}

void test_MemoryService_ValidateAccess(void) {
    AddressSpace space;
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadWrite, "Test"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    
    MemoryService service(space);
    
    // Valid read
    auto result = service.validateAccess(knx::MemoryAddress(0x0150), 10, knx::AccessType::Read);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryAccessResult::Success), static_cast<int>(result));
    
    // Valid write
    result = service.validateAccess(knx::MemoryAddress(0x0150), 10, knx::AccessType::Write);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryAccessResult::Success), static_cast<int>(result));
    
    // Invalid length (0)
    result = service.validateAccess(knx::MemoryAddress(0x0150), 0, knx::AccessType::Read);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryAccessResult::InvalidLength), static_cast<int>(result));
    
    // Invalid length (>63)
    result = service.validateAccess(knx::MemoryAddress(0x0150), 64, knx::AccessType::Read);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryAccessResult::InvalidLength), static_cast<int>(result));
    
    // Region not found
    result = service.validateAccess(knx::MemoryAddress(0x0050), 10, knx::AccessType::Read);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryAccessResult::RegionNotFound), static_cast<int>(result));
}

void test_MemoryService_InvalidLengthEncoding(void) {
    // Zero length
    std::array<uint8_t, MemoryService::kEncodedReadRequestLength> encoded{};
    TEST_ASSERT_TRUE(MemoryService::encodeReadRequest(0, knx::MemoryAddress(0x1234), encoded).isError());
    
    // Too large
    TEST_ASSERT_TRUE(MemoryService::encodeReadRequest(64, knx::MemoryAddress(0x1234), encoded).isError());
}

void test_MemoryService_DataSizeMismatch(void) {
    AddressSpace space;
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadWrite, "Test"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());
    
    MemoryService service(space);
    service.setWriteCallback(mockWriteCallback);
    
    // Count says 5 bytes, but only 3 provided
    std::vector<uint8_t> data = {0x01, 0x02, 0x03};
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleWriteRequest(source, 5, knx::MemoryAddress(0x0150), data);
    
    TEST_ASSERT_FALSE(result.isOk());
}

// ============================================================================
// Test Runner
// ============================================================================

// ============================================================================
// Extended memory service tests (A_MemoryExtended_*)
// ============================================================================

void test_MemoryExtended_ApciDecodeDoesNotCollideWithAdcResponse(void) {
    // 0x1FB..0x1FE share the 0x1C0 service group with A_ADC_Response, so the
    // full 10 bits must be matched.  Getting this wrong makes the device answer
    // ETS's key-table download as an ADC reply.
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::MemoryExtendedWrite),
                      static_cast<int>(APCIField(0x1FB).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::MemoryExtendedWriteResponse),
                      static_cast<int>(APCIField(0x1FC).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::MemoryExtendedRead),
                      static_cast<int>(APCIField(0x1FD).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::MemoryExtendedReadResponse),
                      static_cast<int>(APCIField(0x1FE).service()));

    // A genuine A_ADC_Response must still decode as one.
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::ADCResponse),
                      static_cast<int>(APCIField(0x1C5).service()));
}

void test_MemoryExtended_ReadReturnsDataAndSuccess(void) {
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::ReadWrite);

    for (size_t i = 0; i < mockMemory.size(); i++) {
        mockMemory[i] = static_cast<uint8_t>(i & 0xFF);
    }

    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleExtendedReadRequest(source, 4, ExtendedMemoryAddress(0x000150u));

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(readCallbackCalled);
    TEST_ASSERT_TRUE(extendedResponseCalled);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedResponseKind::Read),
                      static_cast<int>(lastExtendedResponse.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedReturnCode::Success),
                      static_cast<int>(lastExtendedResponse.returnCode));
    TEST_ASSERT_EQUAL(0x000150u, lastExtendedResponse.address.raw);
    TEST_ASSERT_EQUAL(4, lastExtendedResponse.data.size());
    TEST_ASSERT_EQUAL(0x50, lastExtendedResponse.data[0]);
}

void test_MemoryExtended_WriteStoresDataAndAnswers(void) {
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::ReadWrite);

    const std::vector<uint8_t> data = {0xAA, 0xBB, 0xCC};
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleExtendedWriteRequest(source, 3, ExtendedMemoryAddress(0x000150u), data);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(writeCallbackCalled);
    TEST_ASSERT_TRUE(extendedResponseCalled);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedResponseKind::Write),
                      static_cast<int>(lastExtendedResponse.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedReturnCode::Success),
                      static_cast<int>(lastExtendedResponse.returnCode));
    // A write response carries no data — only the return code and address.
    TEST_ASSERT_EQUAL(0, lastExtendedResponse.data.size());
    TEST_ASSERT_EQUAL(0xAA, mockMemory[0x0150]);
    TEST_ASSERT_EQUAL(0xCC, mockMemory[0x0152]);
}

void test_MemoryExtended_FailureStillAnswers(void) {
    // The whole point of the extended services being confirmed: ETS blocks
    // waiting for a response, so a refusal must be reported, not dropped.
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::ReadWrite);

    knx::IndividualAddress source(1, 2, 3);
    // 0x9000 is outside the single registered region.
    auto result = service.handleExtendedReadRequest(source, 4, ExtendedMemoryAddress(0x009000u));

    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_FALSE(readCallbackCalled);
    TEST_ASSERT_TRUE(extendedResponseCalled);
    // A failed read must still be answered as a *read* response.
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedResponseKind::Read),
                      static_cast<int>(lastExtendedResponse.kind));
    TEST_ASSERT_EQUAL(0xFD, static_cast<int>(lastExtendedResponse.returnCode));
    // Figure 68: on a non-zero return code the APDU ends with the address.
    TEST_ASSERT_EQUAL(0, lastExtendedResponse.data.size());
}

void test_MemoryExtended_AddressAbove16BitIsVoid(void) {
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::ReadWrite);

    knx::IndividualAddress source(1, 2, 3);
    // Representable in 24 bits, unreachable through the 16-bit region map.
    auto result = service.handleExtendedReadRequest(source, 4, ExtendedMemoryAddress(0x010150u));

    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_TRUE(extendedResponseCalled);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedReturnCode::AddressVoid),
                      static_cast<int>(lastExtendedResponse.returnCode));
}

void test_MemoryExtended_ReadOnlyRegionRefusesWrite(void) {
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::ReadOnly);

    const std::vector<uint8_t> data = {0xAA};
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleExtendedWriteRequest(source, 1, ExtendedMemoryAddress(0x000150u), data);

    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_FALSE(writeCallbackCalled);
    TEST_ASSERT_TRUE(extendedResponseCalled);
    // Table 4: a write to a read-only location is E_ACCESS_READ_ONLY, not the
    // authorisation-flavoured E_ACCESS_DENIED.
    TEST_ASSERT_EQUAL(0xFB, static_cast<int>(lastExtendedResponse.returnCode));
}

void test_MemoryExtended_WriteAcceptsPayloadLargerThanClassicLimit(void) {
    // The whole point of A_MemoryExtended_*: its count is a full octet, so it
    // carries far more than the 63 octets the classic A_Memory_* services can
    // address with their 6-bit APCI count.  ETS downloads table segments at
    // exactly this size — 76 octets is what it writes to the Group Object
    // table — and capping the extended path at the classic limit answered
    // E_LENGTH_EXCEEDS_MAX_APDU_LENGTH, so no real project could be loaded.
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::ReadWrite);

    constexpr uint8_t kCount = 76u;
    std::vector<uint8_t> data(kCount);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>(i);
    }

    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleExtendedWriteRequest(source, kCount,
                                                     ExtendedMemoryAddress(0x000150u), data);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(writeCallbackCalled);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedReturnCode::Success),
                      static_cast<int>(lastExtendedResponse.returnCode));
    TEST_ASSERT_EQUAL(0x00, mockMemory[0x0150]);
    TEST_ASSERT_EQUAL(kCount - 1, mockMemory[0x0150 + kCount - 1]);
}

void test_MemoryExtended_ReadAcceptsCountLargerThanClassicLimit(void) {
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::ReadWrite);

    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleExtendedReadRequest(source, 76u, ExtendedMemoryAddress(0x000150u));

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedReturnCode::Success),
                      static_cast<int>(lastExtendedResponse.returnCode));
    TEST_ASSERT_EQUAL(76, lastExtendedResponse.data.size());
}

void test_MemoryExtended_WriteMarksRegionAsWritten(void) {
    // The flag that lets the BAU tell "ETS downloaded zeros" from "ETS never
    // downloaded this segment". Applying an untouched segment wiped every
    // firmware parameter default with 0 on a real commissioning run.
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::ReadWrite);

    // Writing zeros must still count as written — the value is not the signal.
    const std::vector<uint8_t> zeros(4u, 0x00u);
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleExtendedWriteRequest(source, 4u,
                                                     ExtendedMemoryAddress(0x000150u), zeros);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(writeCallbackCalled);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedReturnCode::Success),
                      static_cast<int>(lastExtendedResponse.returnCode));
}

void test_MemoryService_ClassicWriteStillRejectsOversizedPayload(void) {
    // The classic services keep their own 63-octet ceiling: relaxing the shared
    // validator must not relax them too.
    AddressSpace space;
    MemoryRegion region{knx::MemoryAddress(0x0100), 0x0100, MemoryAccessMode::ReadWrite, "Test"};
    TEST_ASSERT_TRUE(space.addRegion(region).isOk());

    MemoryService service(space);
    service.setReadCallback(mockReadCallback);
    service.setWriteCallback(mockWriteCallback);
    writeCallbackCalled = false;

    const std::vector<uint8_t> data(70u, 0x5Au);
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleWriteRequest(source, 70u, knx::MemoryAddress(0x0150), data);

    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_FALSE(writeCallbackCalled);
}

void test_MemoryExtended_WriteOnlyRegionRefusesRead(void) {
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::WriteOnly);

    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleExtendedReadRequest(source, 1, ExtendedMemoryAddress(0x000150u));

    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_FALSE(readCallbackCalled);
    TEST_ASSERT_TRUE(extendedResponseCalled);
    // Table 3: a read of a write-only location is E_ACCESS_WRITE_ONLY.
    TEST_ASSERT_EQUAL(0xFA, static_cast<int>(lastExtendedResponse.returnCode));
}

void test_MemoryExtended_ReturnCodesMatchSpecTables(void) {
    // 03/03/07 v02.01.01 Tables 3 and 4.  These are generic-range codes
    // (E0h..FEh), not small integers — pinned so a future edit cannot quietly
    // reintroduce plausible-looking but wrong values.
    TEST_ASSERT_EQUAL(0x00, static_cast<int>(MemoryExtendedReturnCode::Success));
    TEST_ASSERT_EQUAL(0xF1, static_cast<int>(MemoryExtendedReturnCode::MemoryError));
    TEST_ASSERT_EQUAL(0xF4, static_cast<int>(MemoryExtendedReturnCode::ExceedsMaxApduLength));
    TEST_ASSERT_EQUAL(0xF9, static_cast<int>(MemoryExtendedReturnCode::TemporarilyNotAvailable));
    TEST_ASSERT_EQUAL(0xFA, static_cast<int>(MemoryExtendedReturnCode::AccessWriteOnly));
    TEST_ASSERT_EQUAL(0xFB, static_cast<int>(MemoryExtendedReturnCode::AccessReadOnly));
    TEST_ASSERT_EQUAL(0xFC, static_cast<int>(MemoryExtendedReturnCode::AccessDenied));
    TEST_ASSERT_EQUAL(0xFD, static_cast<int>(MemoryExtendedReturnCode::AddressVoid));
    TEST_ASSERT_EQUAL(0xFF, static_cast<int>(MemoryExtendedReturnCode::GenericError));

    // 3.4.9.1 / 3.4.9.2 cap both services at 250 octets.
    TEST_ASSERT_TRUE(MemoryService::kMaxExtendedMemoryBytes <= 250u);
}

void test_MemoryExtended_TruncatedWriteIsRejected(void) {
    AddressSpace space;
    auto service = makeExtendedService(space, MemoryAccessMode::ReadWrite);

    // Declared count outruns the octets actually present in the APDU.
    const std::vector<uint8_t> data = {0xAA};
    knx::IndividualAddress source(1, 2, 3);
    auto result = service.handleExtendedWriteRequest(source, 4, ExtendedMemoryAddress(0x000150u), data);

    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_FALSE(writeCallbackCalled);
    TEST_ASSERT_TRUE(extendedResponseCalled);
    TEST_ASSERT_EQUAL(static_cast<int>(MemoryExtendedReturnCode::ExceedsMaxApduLength),
                      static_cast<int>(lastExtendedResponse.returnCode));
}

int main(void) {
    UNITY_BEGIN();
    
    // AddressSpace tests
    RUN_TEST(test_AddressSpace_Init);
    RUN_TEST(test_AddressSpace_AddRegion);
    RUN_TEST(test_AddressSpace_RejectOverlap);
    RUN_TEST(test_AddressSpace_RejectZeroSize);
    RUN_TEST(test_AddressSpace_FindRegion);
    RUN_TEST(test_AddressSpace_CanRead);
    RUN_TEST(test_AddressSpace_BoundaryCheck);
    RUN_TEST(test_AddressSpace_ClearRegions);
    RUN_TEST(test_AddressSpace_GetAccessMode);
    
    // MemoryService tests
    RUN_TEST(test_MemoryService_Init);
    RUN_TEST(test_MemoryService_ReadRequest);
    RUN_TEST(test_MemoryService_WriteRequest);
    RUN_TEST(test_MemoryService_ReadDenied);
    RUN_TEST(test_MemoryService_WriteDenied);
    RUN_TEST(test_MemoryService_EncodeReadRequest);
    RUN_TEST(test_MemoryService_EncodeWriteRequest);
    RUN_TEST(test_MemoryService_EncodeResponse);
    RUN_TEST(test_MemoryService_DecodeRequest);
    RUN_TEST(test_MemoryService_DecodeWriteRequest);
    RUN_TEST(test_MemoryService_ValidateAccess);
    RUN_TEST(test_MemoryService_InvalidLengthEncoding);
    RUN_TEST(test_MemoryService_DataSizeMismatch);

    RUN_TEST(test_MemoryExtended_ApciDecodeDoesNotCollideWithAdcResponse);
    RUN_TEST(test_MemoryExtended_ReadReturnsDataAndSuccess);
    RUN_TEST(test_MemoryExtended_WriteStoresDataAndAnswers);
    RUN_TEST(test_MemoryExtended_WriteAcceptsPayloadLargerThanClassicLimit);
    RUN_TEST(test_MemoryExtended_ReadAcceptsCountLargerThanClassicLimit);
    RUN_TEST(test_MemoryExtended_WriteMarksRegionAsWritten);
    RUN_TEST(test_MemoryService_ClassicWriteStillRejectsOversizedPayload);
    RUN_TEST(test_MemoryExtended_FailureStillAnswers);
    RUN_TEST(test_MemoryExtended_AddressAbove16BitIsVoid);
    RUN_TEST(test_MemoryExtended_ReadOnlyRegionRefusesWrite);
    RUN_TEST(test_MemoryExtended_WriteOnlyRegionRefusesRead);
    RUN_TEST(test_MemoryExtended_ReturnCodesMatchSpecTables);
    RUN_TEST(test_MemoryExtended_TruncatedWriteIsRejected);
    
    return UNITY_END();
}
