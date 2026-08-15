// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <gtest/gtest.h>
#include "knx/platform/linux_platform.hpp"
#include <fstream>
#include <cstdio>
#include <span>

using namespace knx::platform;

/**
 * Test suite for Linux platform with file-backed EEPROM
 */
class LinuxPlatformTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean up any leftover EEPROM file before each test
        std::remove("/tmp/knx_eeprom.bin");
    }
    
    void TearDown() override {
        // Clean up after each test
        std::remove("/tmp/knx_eeprom.bin");
    }
};

TEST_F(LinuxPlatformTest, PlatformInitialization) {
    LinuxPlatform platform;
    
    // Verify basic properties
    EXPECT_NE(platform.uniqueSerialNumber(), 0);
    
    uint8_t mac[6];
    platform.macAddress(mac);
    EXPECT_EQ(mac[0], 0x02);  // Locally administered
}

TEST_F(LinuxPlatformTest, MemoryRead) {
    LinuxPlatform platform;
    auto& memory = platform.memory();
    
    // Write some data
    uint8_t writeData[] = {0x01, 0x02, 0x03, 0x04};
    ASSERT_TRUE(memory.write(0, std::span<const uint8_t>(writeData, sizeof(writeData))));
    
    // Read it back
    uint8_t readData[sizeof(writeData)];
    ASSERT_TRUE(memory.read(0, std::span<uint8_t>(readData, sizeof(readData))));
    
    for (size_t i = 0; i < sizeof(writeData); ++i) {
        EXPECT_EQ(readData[i], writeData[i]);
    }
}

TEST_F(LinuxPlatformTest, MemoryWrite) {
    LinuxPlatform platform;
    auto& memory = platform.memory();
    
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
    ASSERT_TRUE(memory.write(100, std::span<const uint8_t>(data, sizeof(data))));
}

TEST_F(LinuxPlatformTest, MemoryErase) {
    LinuxPlatform platform;
    auto& memory = platform.memory();
    
    uint8_t writeData[] = {0xFF, 0xFF, 0xFF, 0xFF};
    memory.write(50, std::span<const uint8_t>(writeData, sizeof(writeData)));

    uint8_t readData[sizeof(writeData)];
    memory.read(50, std::span<uint8_t>(readData, sizeof(readData)));
    
    for (size_t i = 0; i < sizeof(writeData); ++i) {
        EXPECT_EQ(readData[i], 0xFF);
    }
    
    // Erase the region
    ASSERT_TRUE(memory.erase(50, sizeof(writeData)).isOk());
    
    // Verify erasure
    memory.read(50, std::span<uint8_t>(readData, sizeof(readData)));
    for (size_t i = 0; i < sizeof(writeData); ++i) {
        EXPECT_EQ(readData[i], 0x00);
    }
}

TEST_F(LinuxPlatformTest, MemoryCommit) {
    {
        LinuxPlatform platform;
        auto& memory = platform.memory();
        
        uint8_t data[] = {0x12, 0x34, 0x56, 0x78};
        memory.write(200, std::span<const uint8_t>(data, sizeof(data)));
        
        memory.commit();
    }
    
    // Create a new platform and verify data persisted
    {
        LinuxPlatform platform2;
        auto& memory2 = platform2.memory();
        
        uint8_t readData[4];
        ASSERT_TRUE(memory2.read(200, std::span<uint8_t>(readData, sizeof(readData))));
        
        EXPECT_EQ(readData[0], 0x12);
        EXPECT_EQ(readData[1], 0x34);
        EXPECT_EQ(readData[2], 0x56);
        EXPECT_EQ(readData[3], 0x78);
    }
}

TEST_F(LinuxPlatformTest, MemoryPersistence) {
    // First instance: write and commit data
    {
        LinuxPlatform platform1;
        auto& memory1 = platform1.memory();
        
        uint8_t testPattern[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
        ASSERT_TRUE(memory1.write(10, testPattern, sizeof(testPattern)));
        ASSERT_TRUE(memory1.commit());
    }
    
    // Second instance: verify persistence
    {
        LinuxPlatform platform2;
        auto& memory2 = platform2.memory();
        
        uint8_t readData[8];
        ASSERT_TRUE(memory2.read(10, std::span<uint8_t>(readData, sizeof(readData))));
        
        uint8_t expected[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
        for (size_t i = 0; i < sizeof(expected); ++i) {
            EXPECT_EQ(readData[i], expected[i]) << "Mismatch at index " << i;
        }
    }
}

TEST_F(LinuxPlatformTest, MemoryOutOfBounds) {
    LinuxPlatform platform;
    auto& memory = platform.memory();
    
    uint32_t memSize = memory.size();
    uint8_t buf[10];

    // Try to read beyond buffer size
    EXPECT_EQ(memory.read(memSize - 5, std::span<uint8_t>(buf, 10)), 0u);

    // Try to write beyond buffer size
    EXPECT_EQ(memory.write(memSize, std::span<const uint8_t>(buf, 1)), 0u);
    
    // Try to erase beyond buffer size
    EXPECT_FALSE(memory.erase(memSize - 5, 10));
}

TEST_F(LinuxPlatformTest, TimeKeeping) {
    LinuxPlatform platform;
    
    uint32_t start = platform.millis();
    uint64_t startMicros = platform.micros();
    
    platform.delay(10);
    
    uint32_t end = platform.millis();
    uint64_t endMicros = platform.micros();
    
    // Time should have elapsed (allowing 5ms tolerance)
    EXPECT_GE(end - start, 5);
    EXPECT_GE(endMicros - startMicros, 5000);
}

TEST_F(LinuxPlatformTest, TaskCreationAndDeletion) {
    LinuxPlatform platform;
    
    bool taskExecuted = false;
    
    TaskConfig config;
    config.function = [&taskExecuted]() {
        taskExecuted = true;
    };
    
    auto task = platform.createTask(config);
    EXPECT_NE(task, nullptr);
    
    // Give the task time to execute
    platform.delay(50);
    
    EXPECT_TRUE(taskExecuted);
    platform.deleteTask(task);
}

TEST_F(LinuxPlatformTest, MutexCreationAndUsage) {
    LinuxPlatform platform;
    
    auto mutex = platform.createMutex();
    EXPECT_NE(mutex, nullptr);
    
    // Lock and unlock
    EXPECT_TRUE(platform.mutexLock(mutex, 0));
    platform.mutexUnlock(mutex);
    
    // Lock with timeout
    EXPECT_TRUE(platform.mutexLock(mutex, 100));
    platform.mutexUnlock(mutex);
    
    platform.deleteMutex(mutex);
}

TEST_F(LinuxPlatformTest, SemaphoreOperations) {
    LinuxPlatform platform;
    
    auto sem = platform.createSemaphore(1);
    EXPECT_NE(sem, nullptr);
    
    // Take available semaphore
    EXPECT_TRUE(platform.semaphoreTake(sem, 0));
    
    // Try to take unavailable semaphore with timeout
    EXPECT_FALSE(platform.semaphoreTake(sem, 10));
    
    // Give semaphore
    platform.semaphoreGive(sem);
    
    // Now we should be able to take it
    EXPECT_TRUE(platform.semaphoreTake(sem, 0));
    
    platform.deleteSemaphore(sem);
}

TEST_F(LinuxPlatformTest, QueueOperations) {
    LinuxPlatform platform;
    
    auto queue = platform.createQueue(sizeof(uint32_t), 5);
    EXPECT_NE(queue, nullptr);
    
    uint32_t sendData = 0x12345678;
    ASSERT_TRUE(platform.queueSend(queue, &sendData, 0).isOk());
    
    uint32_t receiveData = 0;
    ASSERT_TRUE(platform.queueReceive(queue, &receiveData, 0).isOk());
    
    EXPECT_EQ(receiveData, sendData);
    
    platform.deleteQueue(queue);
}

TEST_F(LinuxPlatformTest, EventGroupOperations) {
    LinuxPlatform platform;
    
    auto eventGroup = platform.createEventGroup();
    EXPECT_NE(eventGroup, nullptr);
    
    platform.eventGroupSetBits(eventGroup, 0x01);
    
    uint32_t waitResult = platform.eventGroupWaitBits(
        eventGroup,
        0x01,
        knx::platform::EventGroupClearMode::Keep,
        knx::platform::EventGroupWaitMode::Any,
        0);
    EXPECT_EQ(waitResult & 0x01, 0x01);
    
    platform.eventGroupClearBits(eventGroup, 0x01);
    
    platform.deleteEventGroup(eventGroup);
}

TEST_F(LinuxPlatformTest, NetworkInterfaceStub) {
    LinuxPlatform platform;
    auto* network = platform.network();
    
    ASSERT_NE(network, nullptr);
    EXPECT_TRUE(network->init().isOk());
    
    uint8_t mac[6];
    network->macAddress(mac);
    EXPECT_EQ(mac[0], 0x02);  // Locally administered
}

TEST_F(LinuxPlatformTest, UartInterfaceStub) {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    ASSERT_NE(uart, nullptr);
    UartConfig uartConfig;
    uartConfig.baudRate = 115200;
    uartConfig.dataBits = 8;
    uartConfig.stopBits = 1;
    uartConfig.parity = UartConfig::Parity::Even;
    EXPECT_TRUE(uart->init(uartConfig).isOk());
    
    uint8_t data[] = {0x01, 0x02, 0x03};
    EXPECT_EQ(uart->write(std::span<const uint8_t>(data, sizeof(data))), sizeof(data));
    
    uart->close();
}

TEST_F(LinuxPlatformTest, SpiInterfaceStub) {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    ASSERT_NE(spi, nullptr);
    SpiConfig spiConfig;
    spiConfig.clockHz = 1000000;
    spiConfig.mode = SpiMode::Mode0;
    spiConfig.bitOrder = SpiBitOrder::MsbFirst;
    EXPECT_TRUE(spi->init(spiConfig).isOk());
    
    uint8_t txData[] = {0xAA, 0xBB};
    uint8_t rxData[2];
    EXPECT_EQ(spi->transfer(std::span<const uint8_t>(txData, sizeof(txData)), std::span<uint8_t>(rxData, sizeof(rxData))), sizeof(txData));
}
