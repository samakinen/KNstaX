// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_linux_uart_spi_real.cpp
 * @brief Integration tests for Linux UART and SPI real implementations
 * 
 * These tests verify the API compliance of the Linux platform implementations.
 * They will fail gracefully if no hardware is available (/dev/ttyUSB*, /dev/spidev*).
 */

#include "unity.h"
#include "knx/platform/uart_interface.hpp"
#include "knx/platform/spi_interface.hpp"
#include <cstring>
#include <span>

// Forward declaration to avoid abstract class issues
namespace knx { namespace platform { class Platform; }}

extern knx::platform::UartInterface* createUart();
extern knx::platform::SpiInterface* createSpi();
extern void destroyUart(knx::platform::UartInterface* uart);
extern void destroySpi(knx::platform::SpiInterface* spi);

using namespace knx::platform;

void setUp(void) {
}

void tearDown(void) {
}

// ============================================================================
// UART API Compliance Tests
// ============================================================================

void test_uart_api_lifecycle() {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    TEST_ASSERT_NOT_NULL(uart);
    
    // Initially closed
    TEST_ASSERT_FALSE(uart->isOpen());
    
    // Configure for KNX TP1 (19200 8E1)
    UartConfig config;
    config.baudRate = 19200;
    config.dataBits = 8;
    config.stopBits = 1;
    config.parity = UartConfig::Parity::Even;
    
    // Init may fail if no device present (acceptable)
    bool initOk = uart->init(config);
    
    if (initOk) {
        TEST_ASSERT_TRUE(uart->isOpen());
        
        // Close
        uart->close();
        TEST_ASSERT_FALSE(uart->isOpen());
    } else {
        // No UART device available - test passes (expected on systems without serial ports)
        TEST_PASS();
    }
}

void test_uart_config_validation() {
    UartConfig config;
    
    // Valid default config
    config.baudRate = 19200;
    config.dataBits = 8;
    config.stopBits = 1;
    config.parity = UartConfig::Parity::Even;
    TEST_ASSERT_TRUE(config.validate().isOk());
    
    // Invalid baud rate
    config.baudRate = 100;  // Too low
    TEST_ASSERT_FALSE(config.validate().isOk());
    
    config.baudRate = 10000000;  // Too high
    TEST_ASSERT_FALSE(config.validate().isOk());
    
    // Valid baud rates
    config.baudRate = 9600;
    TEST_ASSERT_TRUE(config.validate().isOk());
    
    config.baudRate = 115200;
    TEST_ASSERT_TRUE(config.validate().isOk());
    
    // Invalid data bits
    config.baudRate = 19200;
    config.dataBits = 4;  // Too low
    TEST_ASSERT_FALSE(config.validate().isOk());
    
    config.dataBits = 9;  // Too high
    TEST_ASSERT_FALSE(config.validate().isOk());
    
    // Valid data bits
    config.dataBits = 7;
    TEST_ASSERT_TRUE(config.validate().isOk());
    
    config.dataBits = 8;
    TEST_ASSERT_TRUE(config.validate().isOk());
    
    // Invalid stop bits
    config.stopBits = 0;
    TEST_ASSERT_FALSE(config.validate().isOk());
    
    config.stopBits = 3;
    TEST_ASSERT_FALSE(config.validate().isOk());
    
    // Valid stop bits
    config.stopBits = 1;
    TEST_ASSERT_TRUE(config.validate().isOk());
    
    config.stopBits = 2;
    TEST_ASSERT_TRUE(config.validate().isOk());
}

void test_uart_read_write_api() {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    UartConfig config;
    config.baudRate = 115200;
    config.dataBits = 8;
    config.stopBits = 1;
    config.parity = UartConfig::Parity::None;
    
    if (!uart->init(config)) {
        TEST_PASS();  // No device, skip test
        return;
    }
    
    // Test available() - should be 0 initially
    TEST_ASSERT_EQUAL(0, uart->available());
    
    // Test read() when no data - should return -1
    TEST_ASSERT_EQUAL(-1, uart->read());
    
    // Test write single byte (won't verify reception without loopback)
    size_t written = uart->write(0xAA);
    TEST_ASSERT_TRUE(written == 0 || written == 1);  // 0 if buffer full, 1 if written
    
    // Test write multiple bytes
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    written = uart->write(std::span<const uint8_t>(data, sizeof(data)));
    TEST_ASSERT_TRUE(written <= sizeof(data));
    
    // Test flush
    uart->flush();  // Should not crash
    
    // Test clear
    uart->clear();  // Should not crash
    TEST_ASSERT_FALSE(uart->overflow());  // Should not be overflowing
    
    uart->close();
}

void test_uart_callback_api() {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    // Test callback registration (won't be called without actual data)
    static int callbackCount = 0;
    auto callback = [](void* context) {
        int* count = static_cast<int*>(context);
        (*count)++;
    };
    
    uart->setRxCallback(callback, &callbackCount);
    
    // Callback set successfully (no crash)
    TEST_PASS();
}

void test_uart_multiple_configs() {
    LinuxPlatform platform;
    auto* uart = platform.uart();
    
    // Test various standard baud rates
    struct TestConfig {
        uint32_t baudRate;
        UartConfig::Parity parity;
    };
    
    TestConfig configs[] = {
        {9600, UartConfig::Parity::None},
        {19200, UartConfig::Parity::Even},
        {38400, UartConfig::Parity::Odd},
        {115200, UartConfig::Parity::None},
    };
    
    for (const auto& tc : configs) {
        UartConfig config;
        config.baudRate = tc.baudRate;
        config.dataBits = 8;
        config.stopBits = 1;
        config.parity = tc.parity;
        
        if (uart->init(config)) {
            TEST_ASSERT_TRUE(uart->isOpen());
            uart->close();
            TEST_ASSERT_FALSE(uart->isOpen());
        }
        // If init fails, device not available - acceptable
    }
    
    TEST_PASS();
}

// ============================================================================
// SPI API Compliance Tests
// ============================================================================

void test_spi_api_lifecycle() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    TEST_ASSERT_NOT_NULL(spi);
    
    // Initially closed
    TEST_ASSERT_FALSE(spi->isOpen());
    
    // Configure SPI
    SpiConfig config;
    config.clockHz = 1000000;  // 1 MHz
    config.mode = SpiMode::Mode0;
    config.bitOrder = SpiBitOrder::MsbFirst;
    config.csPin = -1;  // No CS pin control
    
    // Init may fail if no device present (acceptable)
    bool initOk = spi->init(config);
    
    if (initOk) {
        TEST_ASSERT_TRUE(spi->isOpen());
        
        // Close
        spi->close();
        TEST_ASSERT_FALSE(spi->isOpen());
    } else {
        // No SPI device available - test passes
        TEST_PASS();
    }
}

void test_spi_transfer_api() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    SpiConfig config;
    config.clockHz = 2000000;  // 2 MHz
    config.mode = SpiMode::Mode0;
    
    if (!spi->init(config)) {
        TEST_PASS();  // No device, skip test
        return;
    }
    
    // Test single byte transfer
    uint8_t rxByte = spi->transfer(0xAA);
    (void)rxByte;  // May return anything depending on SPI slave
    
    // Test buffer transfer (TX/RX separate)
    uint8_t txData[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t rxData[4] = {0};
    size_t transferred = spi->transfer(std::span<const uint8_t>(txData, sizeof(txData)), std::span<uint8_t>(rxData, sizeof(rxData)));
    TEST_ASSERT_EQUAL(sizeof(txData), transferred);
    
    // Test in-place transfer
    uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
    transferred = spi->transfer(std::span<uint8_t>(data, sizeof(data)));
    TEST_ASSERT_EQUAL(sizeof(data), transferred);
    
    // Test NULL buffer handling (TX only)
    transferred = spi->transfer(std::span<const uint8_t>{}, std::span<uint8_t>(rxData, sizeof(rxData)));
    TEST_ASSERT_EQUAL(sizeof(rxData), transferred);
    
    // Test NULL buffer handling (RX only)
    transferred = spi->transfer(std::span<const uint8_t>(txData, sizeof(txData)), std::span<uint8_t>{});
    TEST_ASSERT_EQUAL(sizeof(txData), transferred);
    
    spi->close();
}

void test_spi_transaction_api() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    SpiConfig config;
    config.clockHz = 1000000;
    config.mode = SpiMode::Mode0;
    config.csPin = -1;  // Manual CS control
    
    if (!spi->init(config)) {
        TEST_PASS();
        return;
    }
    
    // Test transaction begin/end (should not crash)
    spi->beginTransaction();
    
    uint8_t data = spi->transfer(0x55);
    (void)data;
    
    spi->endTransaction();
    
    // Test manual CS control (may log warning if not implemented)
    spi->setCs(knx::platform::ChipSelectLevel::Assert);
    spi->setCs(knx::platform::ChipSelectLevel::Deassert);
    
    spi->close();
}

void test_spi_different_modes() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    // Test all 4 SPI modes
    SpiMode modes[] = {SpiMode::Mode0, SpiMode::Mode1, SpiMode::Mode2, SpiMode::Mode3};
    
    for (auto mode : modes) {
        SpiConfig config;
        config.clockHz = 1000000;
        config.mode = mode;
        config.bitOrder = SpiBitOrder::MsbFirst;
        
        if (spi->init(config)) {
            TEST_ASSERT_TRUE(spi->isOpen());
            
            // Quick transfer test
            uint8_t result = spi->transfer(0xFF);
            (void)result;
            
            spi->close();
            TEST_ASSERT_FALSE(spi->isOpen());
        }
    }
    
    TEST_PASS();
}

void test_spi_bit_order() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    // Test MSB first
    SpiConfig config;
    config.clockHz = 500000;
    config.mode = SpiMode::Mode0;
    config.bitOrder = SpiBitOrder::MsbFirst;
    
    if (spi->init(config)) {
        spi->close();
    }
    
    // Test LSB first (may not be supported by all hardware)
    config.bitOrder = SpiBitOrder::LsbFirst;
    if (spi->init(config)) {
        spi->close();
    }
    
    TEST_PASS();
}

void test_spi_speed_configurations() {
    LinuxPlatform platform;
    auto* spi = platform.spi();
    
    // Test various clock speeds
    uint32_t speeds[] = {100000, 500000, 1000000, 2000000, 4000000, 8000000};
    
    for (auto speed : speeds) {
        SpiConfig config;
        config.clockHz = speed;
        config.mode = SpiMode::Mode0;
        
        if (spi->init(config)) {
            TEST_ASSERT_TRUE(spi->isOpen());
            spi->close();
        }
    }
    
    TEST_PASS();
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    UNITY_BEGIN();
    
    // UART API tests
    RUN_TEST(test_uart_api_lifecycle);
    RUN_TEST(test_uart_config_validation);
    RUN_TEST(test_uart_read_write_api);
    RUN_TEST(test_uart_callback_api);
    RUN_TEST(test_uart_multiple_configs);
    
    // SPI API tests
    RUN_TEST(test_spi_api_lifecycle);
    RUN_TEST(test_spi_transfer_api);
    RUN_TEST(test_spi_transaction_api);
    RUN_TEST(test_spi_different_modes);
    RUN_TEST(test_spi_bit_order);
    RUN_TEST(test_spi_speed_configurations);
    
    return UNITY_END();
}
