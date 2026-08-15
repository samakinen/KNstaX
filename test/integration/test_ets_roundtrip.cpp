// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <unity.h>
#include "knx/ets/ets_config_loader.hpp"
#include "knx/ets/ets_migration_tool.hpp"
#include "knx/ets/ets_format_validator.hpp"
#include <vector>
#include <cstring>
#include <span>

using namespace knx::ets;

// Build a v1 sample configuration
static std::vector<uint8_t> createV1Config() {
    // Payload components
    std::vector<uint8_t> payload;
    payload.reserve(64);

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

    // Group Object Table (3 group objects, 5 bytes each)
    payload.push_back(0x00); payload.push_back(0x03);  // Count: 3
    payload.push_back(0x00); payload.push_back(0x01);  // DPT
    payload.push_back(0x0F);                 // Flags
    payload.push_back(0x00); payload.push_back(0x00);  // Initial
    payload.push_back(0x05); payload.push_back(0x01);  // DPT
    payload.push_back(0x0F);                 // Flags
    payload.push_back(0x00); payload.push_back(0x32);  // Initial
    payload.push_back(0x09); payload.push_back(0x01);  // DPT
    payload.push_back(0x0C);                 // Flags
    payload.push_back(0x0C); payload.push_back(0x1A);  // Initial

    // Build header
    std::vector<uint8_t> buffer(20, 0x00);
    buffer[0] = 0xAE; buffer[1] = 0xDD;  // Magic
    buffer[2] = 0x01;                    // Version
    buffer[3] = 0x00;                    // Minor version
    buffer[4] = 0x00;                    // Flags
    buffer[5] = 0x00;                    // Reserved

    uint16_t headerCrc = FormatValidator::crc16_ccitt(std::span<const uint8_t>(buffer).first(6));
    buffer[6] = (headerCrc >> 8) & 0xFF;
    buffer[7] = headerCrc & 0xFF;

    uint32_t payloadCrc = FormatValidator::crc32(payload);
    buffer[8] = (payloadCrc >> 24) & 0xFF;
    buffer[9] = (payloadCrc >> 16) & 0xFF;
    buffer[10] = (payloadCrc >> 8) & 0xFF;
    buffer[11] = payloadCrc & 0xFF;

    uint16_t deviceConfigSize = 10;
    uint16_t addressTableSize = 2 + (3 * 2);
    uint16_t associationTableSize = 2 + (3 * 2);
    uint16_t groupObjectTableSize = 2 + (3 * 5);

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

void setUp(void) {}
void tearDown(void) {}

// Test round-trip: load v1 -> save v1 -> load v1
void test_RoundTrip_V1Simple(void) {
    auto v1Buffer = createV1Config();
    
    EtsConfigLoader loader1;
    TEST_ASSERT_TRUE(loader1.loadFromBuffer(v1Buffer).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, loader1.formatVersion());
    
    uint32_t v1Size = loader1.calculateRequiredSize();
    std::vector<uint8_t> saved(v1Size);
    TEST_ASSERT_TRUE(loader1.saveToBuffer(saved, v1Size).isOk());
    
    EtsConfigLoader loader2;
    TEST_ASSERT_TRUE(loader2.loadFromBuffer(std::span<const uint8_t>(saved).first(v1Size)).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, loader2.formatVersion());
    
    TEST_ASSERT_EQUAL_HEX16(0x0101, loader2.deviceConfig().individualAddress.raw);
    TEST_ASSERT_EQUAL_HEX16(0x00BC, loader2.deviceConfig().manufacturerId);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, loader2.deviceConfig().serialNumber);
    TEST_ASSERT_EQUAL_INT(3, loader2.groupObjectCount());
    TEST_ASSERT_EQUAL_INT(3, loader2.addressTableSize());
}

// Test round-trip: create -> save v1 -> load v1 -> save v1 -> load v1
void test_RoundTrip_V1MultipleIterations(void) {
    auto v1Buffer = createV1Config();
    
    EtsConfigLoader loader;
    (void)loader.loadFromBuffer(v1Buffer);
    
    // First round-trip
    uint32_t size1 = loader.calculateRequiredSize();
    std::vector<uint8_t> buffer1(size1);
    TEST_ASSERT_TRUE(loader.saveToBuffer(buffer1, size1).isOk());
    
    EtsConfigLoader loader2;
    TEST_ASSERT_TRUE(loader2.loadFromBuffer(std::span<const uint8_t>(buffer1).first(size1)).isOk());
    
    // Second round-trip
    uint32_t size2 = loader2.calculateRequiredSize();
    std::vector<uint8_t> buffer2(size2);
    TEST_ASSERT_TRUE(loader2.saveToBuffer(buffer2, size2).isOk());
    
    // Buffers should be identical
    TEST_ASSERT_EQUAL_UINT32(size1, size2);
    TEST_ASSERT_EQUAL_MEMORY(buffer1.data(), buffer2.data(), size1);
}

// Test checksum validation after saving
void test_SavedV1HasValidChecksums(void) {
    auto v1Buffer = createV1Config();
    
    EtsConfigLoader loader;
    (void)loader.loadFromBuffer(v1Buffer);
    
    // Save as v1
    uint32_t size = loader.calculateRequiredSize();
    std::vector<uint8_t> buffer(size);
    TEST_ASSERT_TRUE(loader.saveToBuffer(buffer, size).isOk());
    
    // Validate checksums
    auto validation = FormatValidator::validateFull(std::span<const uint8_t>(buffer).first(size));
    TEST_ASSERT_TRUE(validation.valid);
    TEST_ASSERT_TRUE(validation.checksumValid);
    TEST_ASSERT_EQUAL_UINT8(1, validation.detectedVersion);
}

// Test migration tool
void test_MigrationTool_CopyV1(void) {
    auto sourceBuffer = createV1Config();
    
    // Calculate required size
    EtsConfigLoader tempLoader;
    std::span<const uint8_t> src{sourceBuffer};
    (void)tempLoader.loadFromBuffer(src);
    uint32_t targetSize = tempLoader.calculateRequiredSize();

    std::vector<uint8_t> targetBuffer(targetSize);
    std::span<uint8_t> tgt{targetBuffer};

    auto result = EtsMigrationTool::migrateToV1(src, tgt);
    
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_UINT8(1, result.sourceVersion);
    TEST_ASSERT_EQUAL_UINT8(1, result.targetVersion);
    TEST_ASSERT_TRUE(result.bytesWritten > 0);
}

// Test migration validation
void test_MigrationValidation(void) {
    auto v1Buffer = createV1Config();
    
    // Save
    EtsConfigLoader loader;
    (void)loader.loadFromBuffer(std::span<const uint8_t>(v1Buffer));
    
    uint32_t v1Size = loader.calculateRequiredSize();
    std::vector<uint8_t> saved(v1Size);
    (void)loader.saveToBuffer(std::span<uint8_t>(saved), v1Size);
    
    // Validate migration
    auto result = EtsMigrationTool::validateMigration(
        std::span<const uint8_t>(v1Buffer),
        std::span<const uint8_t>(saved).first(v1Size));
    
    TEST_ASSERT_TRUE(result.isOk());
}

// Test buffer size calculation
void test_CalculateRequiredSize(void) {
    auto v1Buffer = createV1Config();
    
    EtsConfigLoader loader;
    (void)loader.loadFromBuffer(std::span<const uint8_t>(v1Buffer));
    
    uint32_t v1Size = loader.calculateRequiredSize();
    TEST_ASSERT_EQUAL_UINT32(63, v1Size);  // 20-byte header + 43-byte payload
    TEST_ASSERT_TRUE(v1Size >= 20);
}

// Test save to undersized buffer
void test_SaveToUndersizedBuffer(void) {
    auto v1Buffer = createV1Config();
    
    EtsConfigLoader loader;
    (void)loader.loadFromBuffer(std::span<const uint8_t>(v1Buffer));
    
    // Try to save to too-small buffer
    uint32_t smallSize = 10;  // Way too small
    std::vector<uint8_t> smallBuffer(smallSize);
    
    TEST_ASSERT_TRUE(loader.saveToBuffer(std::span<uint8_t>(smallBuffer), smallSize).isError());
}

// Test that all group objects preserve through round-trip
void test_RoundTrip_GroupObjectDataPreserved(void) {
    auto v1Buffer = createV1Config();
    
    EtsConfigLoader loader1;
    (void)loader1.loadFromBuffer(std::span<const uint8_t>(v1Buffer));
    
    const auto& originalGOs = loader1.groupObjects();
    
    // Save and reload
    uint32_t size = loader1.calculateRequiredSize();
    std::vector<uint8_t> buffer(size);
    (void)loader1.saveToBuffer(std::span<uint8_t>(buffer), size);
    
    EtsConfigLoader loader2;
    (void)loader2.loadFromBuffer(std::span<const uint8_t>(buffer).first(size));
    
    const auto& loadedGOs = loader2.groupObjects();
    
    TEST_ASSERT_EQUAL_INT(originalGOs.size(), loadedGOs.size());
    
    for (size_t i = 0; i < originalGOs.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(originalGOs[i].index.value(), loadedGOs[i].index.value());
        TEST_ASSERT_EQUAL_HEX16(originalGOs[i].dpt, loadedGOs[i].dpt);
        TEST_ASSERT_EQUAL_HEX8(originalGOs[i].flags, loadedGOs[i].flags);
        TEST_ASSERT_EQUAL_HEX16(originalGOs[i].initialValue, loadedGOs[i].initialValue);
    }
}

// Test migration of already-v1 data (should be no-op copy)
void test_Migration_AlreadyV1(void) {
    auto v1Buffer = createV1Config();
    
    // Try to "migrate" v1 to v1
    uint32_t outputSize = v1Buffer.size();
    std::vector<uint8_t> outputBuffer(outputSize);
    
    auto result = EtsMigrationTool::migrateToV1(
        std::span<const uint8_t>(v1Buffer),
        std::span<uint8_t>(outputBuffer));
    
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_UINT8(1, result.sourceVersion);
    
    // Should be identical
    TEST_ASSERT_EQUAL_MEMORY(v1Buffer.data(), outputBuffer.data(), v1Buffer.size());
}

int main(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_RoundTrip_V1Simple);
    RUN_TEST(test_RoundTrip_V1MultipleIterations);
    RUN_TEST(test_SavedV1HasValidChecksums);
    RUN_TEST(test_MigrationTool_CopyV1);
    RUN_TEST(test_MigrationValidation);
    RUN_TEST(test_CalculateRequiredSize);
    RUN_TEST(test_SaveToUndersizedBuffer);
    RUN_TEST(test_RoundTrip_GroupObjectDataPreserved);
    RUN_TEST(test_Migration_AlreadyV1);
    
    return UNITY_END();
}
