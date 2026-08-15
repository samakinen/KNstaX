// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <unity.h>
#include "knx/ets/ets_config_loader.hpp"
#include "knx/ets/ets_format_validator.hpp"
#include <cstring>
#include <fstream>
#include <vector>
#include <span>

using namespace knx::ets;

// Build a v1 payload (without header)
static std::vector<uint8_t> createSamplePayload() {
    std::vector<uint8_t> payload;

    // Device Config
    payload.push_back(0x01); payload.push_back(0x01);  // Indiv addr: 1.1.1
    payload.push_back(0x00); payload.push_back(0xBC);  // Manufacturer ID
    payload.push_back(0x12); payload.push_back(0x34);  // Serial
    payload.push_back(0x56); payload.push_back(0x78);
    payload.push_back(0x01);                 // App version
    payload.push_back(0x01);                 // Configured

    // Address Table (3 group addresses)
    payload.push_back(0x00); payload.push_back(0x03);  // Count: 3
    payload.push_back(0x01); payload.push_back(0x01);  // GA: 1/1
    payload.push_back(0x01); payload.push_back(0x02);  // GA: 1/2
    payload.push_back(0x02); payload.push_back(0x01);  // GA: 2/1

    // Association Table (3 entries)
    payload.push_back(0x00); payload.push_back(0x03);  // Count: 3
    payload.push_back(0x00); payload.push_back(0x00);  // ASAP 0 -> GO 0
    payload.push_back(0x01); payload.push_back(0x01);  // ASAP 1 -> GO 1
    payload.push_back(0x02); payload.push_back(0x02);  // ASAP 2 -> GO 2

    // Group Object Table (3 group objects)
    payload.push_back(0x00); payload.push_back(0x03);  // Count: 3
    // GO 0: DPT 1.001 (Switch), RW, init=0
    payload.push_back(0x00); payload.push_back(0x01);  // DPT
    payload.push_back(0x0F);                 // Flags
    payload.push_back(0x00); payload.push_back(0x00);  // Initial
    // GO 1: DPT 5.001 (Unsigned 8-bit), RW, init=50
    payload.push_back(0x05); payload.push_back(0x01);  // DPT
    payload.push_back(0x0F);                 // Flags
    payload.push_back(0x00); payload.push_back(0x32);  // Initial
    // GO 2: DPT 9.001 (2-byte float), R, init=21.5
    payload.push_back(0x09); payload.push_back(0x01);  // DPT
    payload.push_back(0x0C);                 // Flags
    payload.push_back(0x0C); payload.push_back(0x1A);  // Initial

    return payload;
}

// Helper function to create sample ETS config (v1)
std::vector<uint8_t> createSampleConfig() {
    const uint16_t deviceConfigSize = 10;
    const uint16_t addressTableSize = 2 + (3 * 2);
    const uint16_t associationTableSize = 2 + (3 * 2);
    const uint16_t groupObjectTableSize = 2 + (3 * 5);

    auto payload = createSamplePayload();

    // Build header
    std::vector<uint8_t> buffer(20, 0x00);
    buffer[0] = 0xAE; buffer[1] = 0xDD;  // Magic
    buffer[2] = 0x01;                    // Version
    buffer[3] = 0x00;                    // Minor version
    buffer[4] = 0x00;                    // Flags
    buffer[5] = 0x00;                    // Reserved

    uint16_t headerCrc = FormatValidator::crc16_ccitt(std::span<const uint8_t>(buffer.data(), 6));
    buffer[6] = (headerCrc >> 8) & 0xFF;
    buffer[7] = headerCrc & 0xFF;

    uint32_t payloadCrc = FormatValidator::crc32(std::span<const uint8_t>(payload.data(), payload.size()));
    buffer[8] = (payloadCrc >> 24) & 0xFF;
    buffer[9] = (payloadCrc >> 16) & 0xFF;
    buffer[10] = (payloadCrc >> 8) & 0xFF;
    buffer[11] = payloadCrc & 0xFF;

    buffer[12] = (deviceConfigSize >> 8) & 0xFF;
    buffer[13] = deviceConfigSize & 0xFF;
    buffer[14] = (addressTableSize >> 8) & 0xFF;
    buffer[15] = addressTableSize & 0xFF;
    buffer[16] = (associationTableSize >> 8) & 0xFF;
    buffer[17] = associationTableSize & 0xFF;
    buffer[18] = (groupObjectTableSize >> 8) & 0xFF;
    buffer[19] = groupObjectTableSize & 0xFF;

    buffer.insert(buffer.end(), payload.begin(), payload.end());
    return buffer;
}

void setUp(void) {
    // Nothing to set up for these tests
}

void tearDown(void) {
    // Clean up after tests
    std::remove("/tmp/test_ets_config.bin");
}

void test_LoadValidConfigFromBuffer(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    TEST_ASSERT_TRUE(loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size())).isOk());
    TEST_ASSERT_TRUE(loader.isValid());
}

void test_ParseDeviceConfig(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    (void)loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()));
    const auto& deviceConfig = loader.deviceConfig();
    
    TEST_ASSERT_EQUAL_HEX16(0x0101, deviceConfig.individualAddress.raw);
    TEST_ASSERT_EQUAL_HEX16(0x00BC, deviceConfig.manufacturerId);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, deviceConfig.serialNumber);
    TEST_ASSERT_EQUAL_INT(0x01, deviceConfig.appVersion);
    TEST_ASSERT_EQUAL_INT(0x01, deviceConfig.configured);
}

void test_ParseAddressTable(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    (void)loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()));
    const auto& addrTable = loader.addressTable();
    
    TEST_ASSERT_EQUAL_INT(3, addrTable.size());
    TEST_ASSERT_EQUAL_HEX16(0x0101, addrTable[0].groupAddress.raw);
    TEST_ASSERT_EQUAL_HEX16(0x0102, addrTable[1].groupAddress.raw);
    TEST_ASSERT_EQUAL_HEX16(0x0201, addrTable[2].groupAddress.raw);
}

void test_ParseAssociationTable(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    (void)loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()));
    const auto& assocTable = loader.associationTable();
    
    TEST_ASSERT_EQUAL_INT(3, assocTable.size());
    TEST_ASSERT_EQUAL_INT(0, assocTable[0].asap);
    TEST_ASSERT_EQUAL_INT(0, assocTable[0].groupObjectIndex.value());
    TEST_ASSERT_EQUAL_INT(1, assocTable[1].groupObjectIndex.value());
    TEST_ASSERT_EQUAL_INT(2, assocTable[2].groupObjectIndex.value());
}

void test_ParseGroupObjectTable(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    (void)loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()));
    const auto& goTable = loader.groupObjects();
    
    TEST_ASSERT_EQUAL_INT(3, goTable.size());
    TEST_ASSERT_EQUAL_INT(0, goTable[0].index.value());
    TEST_ASSERT_EQUAL_HEX16(0x0001, goTable[0].dpt);
    TEST_ASSERT_EQUAL_INT(0x0F, goTable[0].flags);
    TEST_ASSERT_EQUAL_INT(0, goTable[0].initialValue);
    
    TEST_ASSERT_EQUAL_INT(1, goTable[1].index.value());
    TEST_ASSERT_EQUAL_HEX16(0x0501, goTable[1].dpt);
    TEST_ASSERT_EQUAL_INT(50, goTable[1].initialValue);
    
    TEST_ASSERT_EQUAL_INT(2, goTable[2].index.value());
    TEST_ASSERT_EQUAL_HEX16(0x0901, goTable[2].dpt);
}

void test_FindGroupObjectByIndex(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    (void)loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()));
    
    const auto* go = loader.findGroupObject(knx::GroupObjectIndex(1));
    TEST_ASSERT_NOT_NULL(go);
    TEST_ASSERT_EQUAL_HEX16(0x0501, go->dpt);
    TEST_ASSERT_EQUAL_INT(50, go->initialValue);
    
    const auto* goNotFound = loader.findGroupObject(knx::GroupObjectIndex(99));
    TEST_ASSERT_NULL(goNotFound);
}

void test_FindAsapByAddress(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    (void)loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()));
    
    int asap = loader.findAsapByAddress(knx::GroupAddress(0x0101));
    TEST_ASSERT_EQUAL_INT(0, asap);
    
    asap = loader.findAsapByAddress(knx::GroupAddress(0x0102));
    TEST_ASSERT_EQUAL_INT(1, asap);
    
    asap = loader.findAsapByAddress(knx::GroupAddress(0x9999));
    TEST_ASSERT_EQUAL_INT(-1, asap);
}

void test_FindGoIndexByAsap(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    (void)loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()));
    
    knx::GroupObjectIndex goIndex = loader.findGoIndexByAsap(0);
    TEST_ASSERT_EQUAL_INT(0, goIndex.value());
    
    goIndex = loader.findGoIndexByAsap(2);
    TEST_ASSERT_EQUAL_INT(2, goIndex.value());
    
    goIndex = loader.findGoIndexByAsap(99);
    TEST_ASSERT_FALSE(goIndex.isValid());
}

void test_RejectInvalidBuffer(void) {
    EtsConfigLoader loader;
    
    // Too small buffer
    std::vector<uint8_t> smallBuffer(5);
    TEST_ASSERT_TRUE(loader.loadFromBuffer(std::span<const uint8_t>(smallBuffer.data(), smallBuffer.size())).isError());
    
    // Invalid magic byte
    std::vector<uint8_t> invalidMagic(32, 0xFF);
    invalidMagic[0] = 0xFF;
    TEST_ASSERT_TRUE(loader.loadFromBuffer(std::span<const uint8_t>(invalidMagic.data(), invalidMagic.size())).isError());
}

void test_CounterGetters(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    (void)loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()));
    
    TEST_ASSERT_EQUAL_INT(3, loader.groupObjectCount());
    TEST_ASSERT_EQUAL_INT(3, loader.addressTableSize());
    TEST_ASSERT_EQUAL_INT(3, loader.associationTableSize());
}

void test_EmptyConfig(void) {
    EtsConfigLoader loader;
    std::vector<uint8_t> emptyBuffer(32, 0);
    emptyBuffer[0] = 0xAE;
    
    TEST_ASSERT_TRUE(loader.loadFromBuffer(std::span<const uint8_t>(emptyBuffer.data(), emptyBuffer.size())).isError());
}

void test_LoadWithNoValidation(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    // Load without validation (fast path)
    TEST_ASSERT_TRUE(loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()), 
                                          EtsConfigLoader::ValidationLevel::NoValidation).isOk());
    TEST_ASSERT_TRUE(loader.isValid());
}

void test_LoadWithDiagnostics(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    auto result = loader.loadWithDiagnostics(std::span<const uint8_t>(buffer.data(), buffer.size()));
    
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_UINT8(1, result.formatVersion);
    TEST_ASSERT_TRUE(result.checksumValid);
}

void test_FormatVersionDetection(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    (void)loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()));
    
    TEST_ASSERT_EQUAL_UINT8(1, loader.formatVersion());
}

void test_LoadV1FormatWithFullValidation(void) {
    EtsConfigLoader loader;
    auto buffer = createSampleConfig();
    
    // Version 1 format should pass full validation
    TEST_ASSERT_TRUE(loader.loadFromBuffer(std::span<const uint8_t>(buffer.data(), buffer.size()), 
                                          EtsConfigLoader::ValidationLevel::Full).isOk());
    TEST_ASSERT_TRUE(loader.isValid());
}

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_LoadValidConfigFromBuffer);
    RUN_TEST(test_ParseDeviceConfig);
    RUN_TEST(test_ParseAddressTable);
    RUN_TEST(test_ParseAssociationTable);
    RUN_TEST(test_ParseGroupObjectTable);
    RUN_TEST(test_FindGroupObjectByIndex);
    RUN_TEST(test_FindAsapByAddress);
    RUN_TEST(test_FindGoIndexByAsap);
    RUN_TEST(test_RejectInvalidBuffer);
    RUN_TEST(test_CounterGetters);
    RUN_TEST(test_EmptyConfig);
    RUN_TEST(test_LoadWithNoValidation);
    RUN_TEST(test_LoadWithDiagnostics);
    RUN_TEST(test_FormatVersionDetection);
    RUN_TEST(test_LoadV1FormatWithFullValidation);
    
    return UNITY_END();
}
