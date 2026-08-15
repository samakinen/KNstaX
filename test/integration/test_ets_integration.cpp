// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <gtest/gtest.h>
#include "knx/ets/ets_config_loader.hpp"
#include "knx/platform/linux_platform.hpp"
#include <cstring>
#include <span>

using namespace knx::ets;
using namespace knx::platform;

/**
 * Integration tests for ETS configuration loading with Linux platform
 */
class EtsIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::remove("/tmp/knx_eeprom.bin");
        std::remove("/tmp/test_ets_app.bin");
    }
    
    void TearDown() override {
        std::remove("/tmp/knx_eeprom.bin");
        std::remove("/tmp/test_ets_app.bin");
    }
    
    /**
     * Create a sample ETS application binary with complete configuration
     */
    std::vector<uint8_t> createCompleteAppConfig() {
        std::vector<uint8_t> buffer;
        
        // Header: Device Configuration
        buffer.push_back(0xAE);                 // Magic byte
        buffer.push_back(0x01); buffer.push_back(0x02);  // Indiv addr: 1.1.2
        buffer.push_back(0x00); buffer.push_back(0xBC);  // Manufacturer: Siemens
        buffer.push_back(0xAA); buffer.push_back(0xBB);  // Serial: 0xAABBCCDD
        buffer.push_back(0xCC); buffer.push_back(0xDD);
        buffer.push_back(0x02);                 // App version: 2
        buffer.push_back(0x01);                 // Configured: yes
        
        // Address Table (2 group addresses)
        buffer.push_back(0x00); buffer.push_back(0x02);  // Count: 2
        buffer.push_back(0x01); buffer.push_back(0x01);  // GA: 1/1 (Temperature)
        buffer.push_back(0x01); buffer.push_back(0x02);  // GA: 1/2 (Humidity)
        
        // Association Table (2 associations)
        buffer.push_back(0x00); buffer.push_back(0x02);  // Count: 2
        buffer.push_back(0x00); buffer.push_back(0x00);  // ASAP 0 -> GO 0
        buffer.push_back(0x01); buffer.push_back(0x01);  // ASAP 1 -> GO 1
        
        // Group Object Table (2 objects)
        buffer.push_back(0x00); buffer.push_back(0x02);  // Count: 2
        // GO 0: DPT 9.001 (2-byte float), Read/Write/Transmit, initial=20.0
        buffer.push_back(0x09); buffer.push_back(0x01);  // DPT
        buffer.push_back(0x0F);                 // Flags
        buffer.push_back(0x0C); buffer.push_back(0x1A);  // Initial: 20.0
        // GO 1: DPT 5.001 (Unsigned 8-bit), Read/Write/Transmit, initial=60
        buffer.push_back(0x05); buffer.push_back(0x01);  // DPT
        buffer.push_back(0x0F);                 // Flags
        buffer.push_back(0x00); buffer.push_back(0x3C);  // Initial: 60
        
        return buffer;
    }
};

TEST_F(EtsIntegrationTest, LoadConfigAndQueryGroupObjects) {
    EtsConfigLoader loader;
    auto config = createCompleteAppConfig();
    
    ASSERT_TRUE(loader.loadFromBuffer(config).isOk());
    ASSERT_TRUE(loader.isValid());
    
    // Verify group objects
    EXPECT_EQ(loader.groupObjectCount(), 2);
    
    const auto* go0 = loader.findGroupObject(knx::GroupObjectIndex(0));
    ASSERT_NE(go0, nullptr);
    EXPECT_EQ(go0->dpt, 0x0901);  // DPT 9.001
    EXPECT_EQ(go0->flags, 0x0F);
    
    const auto* go1 = loader.findGroupObject(knx::GroupObjectIndex(1));
    ASSERT_NE(go1, nullptr);
    EXPECT_EQ(go1->dpt, 0x0501);  // DPT 5.001
}

TEST_F(EtsIntegrationTest, LoadConfigAndQueryAddressMappings) {
    EtsConfigLoader loader;
    auto config = createCompleteAppConfig();
    
    ASSERT_TRUE(loader.loadFromBuffer(config.data(), config.size()).isOk());
    
    // Test group address to ASAP lookup
    int asap0 = loader.findAsapByAddress(knx::GroupAddress(0x0101));  // GA: 1/1
    EXPECT_EQ(asap0, 0);
    
    int asap1 = loader.findAsapByAddress(knx::GroupAddress(0x0102));  // GA: 1/2
    EXPECT_EQ(asap1, 1);
    
    // Test ASAP to group object lookup
    knx::GroupObjectIndex go0 = loader.findGoIndexByAsap(0);
    EXPECT_EQ(go0.value(), 0);
    
    knx::GroupObjectIndex go1 = loader.findGoIndexByAsap(1);
    EXPECT_EQ(go1.value(), 1);
}

TEST_F(EtsIntegrationTest, LoadConfigFromFile) {
    // Write config to file
    auto config = createCompleteAppConfig();
    {
        std::ofstream file("/tmp/test_ets_app.bin", std::ios::binary);
        file.write(reinterpret_cast<const char*>(config.data()), config.size());
    }
    
    // Load from file
    EtsConfigLoader loader;
    ASSERT_TRUE(loader.loadFromFile("/tmp/test_ets_app.bin").isOk());
    ASSERT_TRUE(loader.isValid());
    
    // Verify configuration
    EXPECT_EQ(loader.deviceConfig().individualAddress.raw, 0x0102);
    EXPECT_EQ(loader.groupObjectCount(), 2);
}

TEST_F(EtsIntegrationTest, PersistConfigToLinuxPlatformEeprom) {
    {
        LinuxPlatform platform;
        auto& memory = platform.memory();
        
        // Simulate ETS config being stored in EEPROM
        auto config = createCompleteAppConfig();
        
        // Write to offset 0 (device config area)
        ASSERT_TRUE(memory.write(0, config));
        memory.commit();
    }
    
    // Verify persistence in new platform instance
    {
        LinuxPlatform platform2;
        auto& memory2 = platform2.memory();
        
        // Read back configuration
        std::vector<uint8_t> readBuffer(50);
        ASSERT_TRUE(memory2.read(0, readBuffer));
        
        // Verify magic byte and device config
        EXPECT_EQ(readBuffer[0], 0xAE);
        
        // Verify individual address
        uint16_t indivAddr = (static_cast<uint16_t>(readBuffer[1]) << 8) | readBuffer[2];
        EXPECT_EQ(indivAddr, 0x0102);
    }
}

TEST_F(EtsIntegrationTest, FullWorkflow) {
    // Step 1: Create and save ETS config to file
    auto config = createCompleteAppConfig();
    {
        std::ofstream file("/tmp/test_ets_app.bin", std::ios::binary);
        file.write(reinterpret_cast<const char*>(config.data()), config.size());
    }
    
    // Step 2: Load config from file
    EtsConfigLoader loader;
    ASSERT_TRUE(loader.loadFromFile("/tmp/test_ets_app.bin").isOk());
    
        // Step 3: Store loaded config in platform EEPROM
    {
        LinuxPlatform platform;
        auto& memory = platform.memory();
        
        // Write complete config to EEPROM offset 0
            ASSERT_TRUE(memory.write(0, config));
            memory.commit();
    }
    
    // Step 4: Reload and verify from new platform instance
    {
        LinuxPlatform platform2;
        auto& memory2 = platform2.memory();
        
        std::vector<uint8_t> readBuffer(config.size());
        ASSERT_TRUE(memory2.read(0, readBuffer));
        
        // Reload config from persisted EEPROM
        EtsConfigLoader loader2;
        ASSERT_TRUE(loader2.loadFromBuffer(readBuffer).isOk());
        
        // Verify all properties match
        EXPECT_EQ(loader2.deviceConfig().individualAddress.raw, 
              loader.deviceConfig().individualAddress.raw);
        EXPECT_EQ(loader2.groupObjectCount(), loader.groupObjectCount());
        EXPECT_EQ(loader2.addressTableSize(), loader.addressTableSize());
    }
}

TEST_F(EtsIntegrationTest, MultipleConfigReloads) {
    const size_t CONFIG_OFFSET = 0x100;
    
    {
        LinuxPlatform platform;
        auto& memory = platform.memory();
        
        auto config = createCompleteAppConfig();
        memory.write(CONFIG_OFFSET, config);
        memory.commit();
    }
    
    // Reload multiple times
    for (int i = 0; i < 3; ++i) {
        LinuxPlatform platform;
        auto& memory = platform.memory();
        
        std::vector<uint8_t> buffer(50);
        memory.read(CONFIG_OFFSET, buffer);
        
        EtsConfigLoader loader;
        ASSERT_TRUE(loader.loadFromBuffer(buffer).isOk());
        EXPECT_EQ(loader.deviceConfig().individualAddress.raw, 0x0102);
    }
}

TEST_F(EtsIntegrationTest, ConfigWithLargeAddressTable) {
    // Create a config with many group addresses
    std::vector<uint8_t> buffer;
    
    // Device config
    buffer.push_back(0xAE);
    buffer.insert(buffer.end(), {0x01, 0x01, 0x00, 0xBC, 0x12, 0x34, 0x56, 0x78, 0x01, 0x01});
    
    // Address table (256 group addresses)
    buffer.push_back(0x01); buffer.push_back(0x00);  // Count: 256
    for (uint16_t i = 0; i < 256; ++i) {
        buffer.push_back(static_cast<uint8_t>(i >> 8));
        buffer.push_back(static_cast<uint8_t>(i & 0xFF));
    }
    
    // Association table (256 entries)
    buffer.push_back(0x01); buffer.push_back(0x00);  // Count: 256
    for (uint8_t i = 0; i < 256; ++i) {
        buffer.push_back(i);
        buffer.push_back(0x00);
    }
    
    // Group object table (256 objects)
    buffer.push_back(0x01); buffer.push_back(0x00);  // Count: 256
    for (uint8_t i = 0; i < 256; ++i) {
        buffer.insert(buffer.end(), {0x05, 0x01, 0x0F, 0x00, i});
    }
    
    // Load and verify
    EtsConfigLoader loader;
    ASSERT_TRUE(loader.loadFromBuffer(buffer).isOk());
    EXPECT_EQ(loader.addressTableSize(), 256);
    EXPECT_EQ(loader.associationTableSize(), 256);
    EXPECT_EQ(loader.groupObjectCount(), 256);
}
