// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_linux_uart_spi.cpp
 * @brief Unit tests for Linux UART and SPI real implementations
 */

#include "unity.h"
#include "knx/platform/linux_platform.hpp"
#include "knx/platform/uart_interface.hpp"
#include "knx/platform/spi_interface.hpp"
#include <cstring>
#include <span>

using namespace knx::platform;

void setUp(void) {
}

void tearDown(void) {
}

// ============================================================================
// UART Tests
// ============================================================================

void test_uart_loopback_mode() {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    TEST_ASSERT_NOT_NULL(uart);
    
    // Enable loopback for testing
    uart->enableLoopback(true);
    
    // Initialize UART (will use loopback since no device)
    TEST_ASSERT_TRUE(uart->init(19200, 8, 1, 2));  // 19200 8E1 for KNX TP1
    
    // Send data
    uint8_t txData[] = {0xAA, 0x55, 0x12, 0x34, 0x56};
    uint32_t sent = uart->write(std::span<const uint8_t>(txData, sizeof(txData)));
    TEST_ASSERT_EQUAL_UINT32(sizeof(txData), sent);
    
    // Check available bytes
    TEST_ASSERT_EQUAL_UINT32(sizeof(txData), uart->available());
    
    // Receive data
    uint8_t rxData[10];
    uint32_t received = uart->read(std::span<uint8_t>(rxData, sizeof(rxData)));
    TEST_ASSERT_EQUAL_UINT32(sizeof(txData), received);
    
    // Verify data matches
    TEST_ASSERT_EQUAL_UINT8_ARRAY(txData, rxData, sizeof(txData));
    
    uart->deinit();
}

void test_uart_multiple_sends() {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    uart->enableLoopback(true);
    TEST_ASSERT_TRUE(uart->init(115200, 8, 1, 0));  // 115200 8N1
    
    // Send multiple packets
    uint8_t packet1[] = {0x01, 0x02, 0x03};
    uint8_t packet2[] = {0x04, 0x05, 0x06};
    uint8_t packet3[] = {0x07, 0x08, 0x09};
    
    TEST_ASSERT_EQUAL_UINT32(3, uart->write(std::span<const uint8_t>(packet1, 3)));
    TEST_ASSERT_EQUAL_UINT32(3, uart->write(std::span<const uint8_t>(packet2, 3)));
    TEST_ASSERT_EQUAL_UINT32(3, uart->write(std::span<const uint8_t>(packet3, 3)));
    
    // Should have 9 bytes available
    TEST_ASSERT_EQUAL_UINT32(9, uart->available());
    
    // Receive all at once
    uint8_t rxData[10];
    uint32_t received = uart->read(std::span<uint8_t>(rxData, 10));
    TEST_ASSERT_EQUAL_UINT32(9, received);
    
    // Verify concatenated data
    uint8_t expected[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, rxData, 9);
    
    uart->deinit();
}

void test_uart_partial_receive() {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    uart->enableLoopback(true);
    TEST_ASSERT_TRUE(uart->init(9600, 8, 2, 0));  // 9600 8N2
    
    // Send 10 bytes
    uint8_t txData[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    TEST_ASSERT_EQUAL_UINT32(10, uart->write(std::span<const uint8_t>(txData, 10)));
    
    // Receive in chunks
    uint8_t rxData[10];
    TEST_ASSERT_EQUAL_UINT32(3, uart->read(std::span<uint8_t>(rxData, 3)));
    TEST_ASSERT_EQUAL_UINT32(3, uart->read(std::span<uint8_t>(rxData + 3, 3)));
    TEST_ASSERT_EQUAL_UINT32(4, uart->read(std::span<uint8_t>(rxData + 6, 4)));
    
    // Verify all data received
    TEST_ASSERT_EQUAL_UINT8_ARRAY(txData, rxData, 10);
    
    // No more data
    TEST_ASSERT_EQUAL_UINT32(0, uart->available());
    
    uart->deinit();
}

void test_uart_baud_rate_configurations() {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    uart->enableLoopback(true);
    
    // Test common baud rates
    TEST_ASSERT_TRUE(uart->init(9600, 8, 1, 0));
    uart->deinit();
    
    TEST_ASSERT_TRUE(uart->init(19200, 8, 1, 0));
    uart->deinit();
    
    TEST_ASSERT_TRUE(uart->init(38400, 8, 1, 0));
    uart->deinit();
    
    TEST_ASSERT_TRUE(uart->init(57600, 8, 1, 0));
    uart->deinit();
    
    TEST_ASSERT_TRUE(uart->init(115200, 8, 1, 0));
    uart->deinit();
}

void test_uart_empty_buffer() {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    uart->enableLoopback(true);
    TEST_ASSERT_TRUE(uart->init(115200, 8, 1, 0));
    
    // Initially empty
    TEST_ASSERT_EQUAL_UINT32(0, uart->available());
    
    // Receive from empty buffer
    uint8_t rxData[10];
    TEST_ASSERT_EQUAL_UINT32(0, uart->read(std::span<uint8_t>(rxData, 10)));
    
    uart->deinit();
}

// ============================================================================
// SPI Tests
// ============================================================================

void test_spi_stub_mode_loopback() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    TEST_ASSERT_NOT_NULL(spi);
    
    // Initialize SPI (will use stub mode since no device)
    TEST_ASSERT_TRUE(spi->init(1000000, 0, 0));  // 1 MHz, mode 0
    
    // Transfer data (stub mode echoes TX to RX)
    uint8_t txData[] = {0x12, 0x34, 0x56, 0x78};
    uint8_t rxData[4] = {0};
    
    TEST_ASSERT_EQUAL_UINT32(sizeof(txData), spi->transfer(std::span<const uint8_t>(txData, sizeof(txData)), std::span<uint8_t>(rxData, sizeof(rxData))));
    
    // In stub mode, RX should equal TX (loopback)
    TEST_ASSERT_EQUAL_UINT8_ARRAY(txData, rxData, sizeof(txData));
    
    spi->deinit();
}

void test_spi_write_only() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    TEST_ASSERT_TRUE(spi->init(2000000, 1, 0));  // 2 MHz, mode 1
    
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), spi->write(std::span<const uint8_t>(data, sizeof(data))));
    
    spi->deinit();
}

void test_spi_read_only() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    TEST_ASSERT_TRUE(spi->init(500000, 2, 0));  // 500 kHz, mode 2
    
    uint8_t data[4];
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), spi->read(std::span<uint8_t>(data, sizeof(data))));
    
    // Stub mode returns zeros
    uint8_t expected[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, data, sizeof(data));
    
    spi->deinit();
}

void test_spi_multiple_transfers() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    TEST_ASSERT_TRUE(spi->init(4000000, 3, 0));  // 4 MHz, mode 3
    
    // Multiple transfers
    for (int i = 0; i < 5; i++) {
        uint8_t txData[] = {static_cast<uint8_t>(i), static_cast<uint8_t>(i + 1)};
        uint8_t rxData[2];
        
        TEST_ASSERT_EQUAL_UINT32(sizeof(txData), spi->transfer(std::span<const uint8_t>(txData, sizeof(txData)), std::span<uint8_t>(rxData, sizeof(rxData))));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(txData, rxData, sizeof(txData));
    }
    
    spi->deinit();
}

void test_spi_different_speeds() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    // Test different frequencies
    TEST_ASSERT_TRUE(spi->init(100000, 0, 0));    // 100 kHz
    spi->deinit();
    
    TEST_ASSERT_TRUE(spi->init(1000000, 0, 0));   // 1 MHz
    spi->deinit();
    
    TEST_ASSERT_TRUE(spi->init(8000000, 0, 0));   // 8 MHz
    spi->deinit();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    UNITY_BEGIN();
    
    // UART tests
    RUN_TEST(test_uart_loopback_mode);
    RUN_TEST(test_uart_multiple_sends);
    RUN_TEST(test_uart_partial_receive);
    RUN_TEST(test_uart_baud_rate_configurations);
    RUN_TEST(test_uart_empty_buffer);
    
    // SPI tests
    RUN_TEST(test_spi_stub_mode_loopback);
    RUN_TEST(test_spi_write_only);
    RUN_TEST(test_spi_read_only);
    RUN_TEST(test_spi_multiple_transfers);
    RUN_TEST(test_spi_different_speeds);
    
    return UNITY_END();
}
