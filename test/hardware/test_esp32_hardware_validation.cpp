/**
 * @file test_esp32_hardware_validation.cpp
 * @brief ESP32 hardware validation test suite for KNX TP1 bit-bang
 *
 * This test suite is designed to run on real ESP32 hardware connected to
 * a KNX TP1 bus via a transceiver (e.g., NCN5120, TPUART).
 *
 * Test Setup:
 * - ESP32 development board
 * - KNX TP1 transceiver (NCN5120 or compatible)
 * - Connection to KNX bus (29V DC, bus powered or external)
 * - Logic analyzer (optional, for timing verification)
 * - Second KNX device (for interoperability testing)
 *
 * Wiring:
 * - GPIO4 (TX) → Transceiver TX
 * - GPIO5 (RX) ← Transceiver RX
 * - GND → Common ground
 * - 3.3V → Transceiver VCC (if not bus-powered)
 *
 * @note Only compiles on ESP32 platform
 * @note Requires real KNX hardware for testing
 * 
 * Copyright (c) 2026 KNstaX Project
 * SPDX-License-Identifier: MIT
 */

#ifdef ESP_PLATFORM

#include "unity.h"
#include "knx/physical/bitbang_driver_timer_isr_espidf.hpp"
#include "knx/physical/physical_factory.hpp"
#include "knx/physical/manchester_codec.hpp"
#include "knx/physical/tp1_frame_codec.hpp"
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

using namespace knx::physical;
using namespace knx;

// Test configuration
static constexpr gpio_num_t TEST_TX_PIN = GPIO_NUM_4;
static constexpr gpio_num_t TEST_RX_PIN = GPIO_NUM_5;

// Global test driver and physical layer
static BitBangDriverTimerIsrEspIdf* g_driver = nullptr;
static std::unique_ptr<PhysicalLayer> g_physical = nullptr;

// RX callback tracking
static volatile bool g_rxFrameReceived = false;
static uint8_t g_rxFrameData[256];
static size_t g_rxFrameLength = 0;

// Timing measurement
static volatile int64_t g_txStartTime = 0;
static volatile int64_t g_txEndTime = 0;

void rxCallback(void* context, const uint8_t* data, size_t length) {
    if (length <= sizeof(g_rxFrameData)) {
        memcpy(g_rxFrameData, data, length);
        g_rxFrameLength = length;
        g_rxFrameReceived = true;
    }
}

void setUp(void) {
    // Create driver
    g_driver = new BitBangDriverTimerIsrEspIdf();
    
    // Create physical layer
    g_physical = createBorrowedTp1BitbangPhysical(*g_driver, *g_driver);
    
    // Set RX callback
    g_driver->setRxCallback(rxCallback, nullptr);
    
    // Reset flags
    g_rxFrameReceived = false;
    g_rxFrameLength = 0;
}

void tearDown(void) {
    if (g_physical) {
        g_physical->close();
        g_physical.reset();
    }
    
    if (g_driver) {
        delete g_driver;
        g_driver = nullptr;
    }
}

// ============================================================================
// Test 1: Hardware Initialization
// ============================================================================

void test_hardware_init(void) {
    TEST_ASSERT_TRUE_MESSAGE(g_physical->init(), 
        "Physical layer initialization failed - check hardware connections");
    
    TEST_ASSERT_TRUE_MESSAGE(g_physical->isOpen(),
        "Physical layer not open after init");
    
    // Allow hardware to settle
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ============================================================================
// Test 2: Bus Voltage Detection
// ============================================================================

void test_bus_voltage_detection(void) {
    TEST_ASSERT_TRUE(g_physical->init());
    
    // Check if medium is idle (bus should be HIGH when idle)
    bool isIdle = g_driver->isMediumIdle();
    
    TEST_ASSERT_TRUE_MESSAGE(isIdle,
        "Bus not idle - check KNX bus power (should be ~29V DC)");
    
    printf("[INFO] Bus detected as idle - KNX bus powered correctly\n");
}

// ============================================================================
// Test 3: Manchester Encoding Timing
// ============================================================================

void test_manchester_timing(void) {
    TEST_ASSERT_TRUE(g_physical->init());
    
    // Create test frame with known pattern (0xAA = 10101010)
    Tp1Frame frame;
    frame.control = 0xBC;
        frame.source = IndividualAddress(0x1101);
        frame.destination = GroupAddress(0x0314);
        frame.destinationType = AddressType::Group;
    frame.length = 1;
    frame.data[0] = 0xAA;  // Alternating pattern for timing test
    
    // Encode frame
    uint8_t frameBuffer[256];
    size_t frameSize = Tp1FrameCodec::encode(frame, frameBuffer);
    
    printf("[INFO] Transmitting test pattern 0xAA for timing analysis\n");
    printf("[INFO] Connect logic analyzer to GPIO%d to verify:\n", TEST_TX_PIN);
    printf("[INFO] - Bit cell: 52.083 µs\n");
    printf("[INFO] - Half-bit: 26.042 µs\n");
    printf("[INFO] - Pattern: 0xAA should show alternating transitions\n");
    
    // Measure transmission time
    g_txStartTime = esp_timer_get_time();
    bool result = g_physical->send(frameBuffer, frameSize);
    
    // Wait for transmission to complete
    vTaskDelay(pdMS_TO_TICKS(50));
    g_txEndTime = esp_timer_get_time();
    
    TEST_ASSERT_TRUE_MESSAGE(result, "Frame transmission failed");
    
    // Calculate transmission time
    int64_t txDuration = g_txEndTime - g_txStartTime;
    int64_t expectedDuration = frameSize * 8 * 52;  // bits * 52µs per bit
    
    printf("[INFO] Transmission time: %lld µs (expected: ~%lld µs)\n", 
           txDuration, expectedDuration);
    
    // Allow ±10% tolerance for timing
    TEST_ASSERT_INT64_WITHIN_MESSAGE(expectedDuration / 10, expectedDuration, txDuration,
        "Transmission timing outside tolerance - check RMT configuration");
}

// ============================================================================
// Test 4: Frame Transmission
// ============================================================================

void test_frame_transmission(void) {
    TEST_ASSERT_TRUE(g_physical->init());
    
    // Create valid KNX frame
    Tp1Frame frame;
    frame.control = 0xBC;      // Standard frame
        frame.source = IndividualAddress(0x1101); // 1.1.1
        frame.destination = GroupAddress(0x0314);   // 3/1/4 (group address)
        frame.destinationType = AddressType::Group;
    frame.length = 2;
    frame.data[0] = 0x00;      // APCI
    frame.data[1] = 0x80;      // Data: ON
    
    // Encode to bytes
    uint8_t frameBuffer[256];
    size_t frameSize = Tp1FrameCodec::encode(frame, frameBuffer);
    
    printf("[INFO] Transmitting KNX frame:\n");
    printf("[INFO]   Source: 1.1.1\n");
    printf("[INFO]   Dest: 3/1/4\n");
    printf("[INFO]   Data: 0x00 0x80 (Group Write ON)\n");
    
    // Transmit
    bool result = g_physical->send(frameBuffer, frameSize);
    
    TEST_ASSERT_TRUE_MESSAGE(result, 
        "Frame transmission failed - check medium idle and retry logic");
    
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ============================================================================
// Test 5: Frame Reception
// ============================================================================

void test_frame_reception(void) {
    TEST_ASSERT_TRUE(g_physical->init());
    
    printf("[INFO] Waiting for frame reception...\n");
    printf("[INFO] Please trigger a KNX frame on the bus\n");
    printf("[INFO] (e.g., press a button on a KNX switch)\n");
    
    // Wait up to 30 seconds for a frame
    g_rxFrameReceived = false;
    
    for (int i = 0; i < 300; i++) {
        if (g_rxFrameReceived) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    TEST_ASSERT_TRUE_MESSAGE(g_rxFrameReceived,
        "No frame received - check RX wiring and bus activity");
    
    printf("[INFO] Frame received! Length: %d bytes\n", g_rxFrameLength);
    printf("[INFO] Raw data: ");
    for (size_t i = 0; i < g_rxFrameLength; i++) {
        printf("%02X ", g_rxFrameData[i]);
    }
    printf("\n");
    
    // Try to decode
    Tp1Frame decoded;
    if (Tp1FrameCodec::decode(std::span<const uint8_t>(g_rxFrameData, g_rxFrameLength), decoded).isOk()) {
        printf("[INFO] Decoded frame:\n");
        printf("[INFO]   Control: 0x%02X\n", decoded.control);
            printf("[INFO]   Source: %d.%d.%d\n", 
                   (decoded.source.raw >> 12) & 0xF,
                   (decoded.source.raw >> 8) & 0xF,
                   decoded.source.raw & 0xFF);
            printf("[INFO]   Dest: 0x%04X\n", decoded.destination.raw);
    }
}

// ============================================================================
// Test 6: Loopback Test (TX → RX)
// ============================================================================

void test_loopback(void) {
    TEST_ASSERT_TRUE(g_physical->init());
    
    // Create test frame
    Tp1Frame frame;
    frame.control = 0xBC;
        frame.source = IndividualAddress(0x1101);
        frame.destination = GroupAddress(0x0314);
        frame.destinationType = AddressType::Group;
    frame.length = 1;
    frame.data[0] = 0x42;  // Test pattern
    
    // Encode
    uint8_t frameBuffer[256];
    size_t frameSize = Tp1FrameCodec::encode(frame, frameBuffer);
    
    printf("[INFO] Loopback test: TX → RX on same device\n");
    printf("[INFO] Note: This requires external loopback or bus echo\n");
    
    g_rxFrameReceived = false;
    
    // Transmit
    TEST_ASSERT_TRUE(g_physical->send(frameBuffer, frameSize));
    
    // Wait for echo (up to 1 second)
    for (int i = 0; i < 100; i++) {
        if (g_rxFrameReceived) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    if (g_rxFrameReceived) {
        printf("[INFO] Loopback successful - frame echoed\n");
        
        // Verify data
        TEST_ASSERT_EQUAL_MESSAGE(frameSize, g_rxFrameLength,
            "Received frame size mismatch");
        
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(frameBuffer, g_rxFrameData, frameSize,
            "Received frame data mismatch");
    } else {
        printf("[WARN] No loopback - this is normal if bus doesn't echo\n");
    }
}

// ============================================================================
// Test 7: Collision Detection
// ============================================================================

void test_collision_detection(void) {
    TEST_ASSERT_TRUE(g_physical->init());
    
    printf("[INFO] Collision detection test\n");
    printf("[INFO] To test: trigger another device during transmission\n");
    
    // Create frame
    Tp1Frame frame;
    frame.control = 0xBC;
    frame.source = IndividualAddress(0x1101);
    frame.destination = GroupAddress(0x0314);
    frame.destinationType = AddressType::Group;
    frame.length = 1;
    frame.data[0] = 0x55;
    
    uint8_t frameBuffer[256];
    size_t frameSize = Tp1FrameCodec::encode(frame, frameBuffer);
    
    // Attempt transmission
    bool result = g_physical->send(frameBuffer, frameSize);
    
    // Check collision flag
    bool collision = g_driver->isCollisionDetected();
    
    if (collision) {
        printf("[INFO] Collision detected during transmission!\n");
        TEST_ASSERT_FALSE_MESSAGE(result,
            "Transmission should fail when collision detected");
    } else {
        printf("[INFO] No collision detected (normal if bus idle)\n");
        TEST_ASSERT_TRUE(result);
    }
}

// ============================================================================
// Test 8: Bus Monitor Mode
// ============================================================================

void test_bus_monitor_mode(void) {
    TEST_ASSERT_TRUE(g_physical->init());
    
    // Enable bus monitor
    auto result1 = g_physical->setBusMonitorMode(knx::Toggle::Enable);
    TEST_ASSERT_TRUE(result1.isOk());
    
    printf("[INFO] Bus monitor mode enabled\n");
    printf("[INFO] Device will receive ALL frames (no filtering)\n");
    printf("[INFO] Waiting 10 seconds for bus activity...\n");
    
    g_rxFrameReceived = false;
    int frameCount = 0;
    
    for (int i = 0; i < 100; i++) {
        if (g_rxFrameReceived) {
            frameCount++;
            printf("[INFO] Frame %d received in monitor mode\n", frameCount);
            g_rxFrameReceived = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    printf("[INFO] Total frames in 10 seconds: %d\n", frameCount);
    
    // Disable bus monitor
    auto result2 = g_physical->setBusMonitorMode(knx::Toggle::Disable);
    TEST_ASSERT_TRUE(result2.isOk());
}

// ============================================================================
// Test 9: Stress Test (Continuous Transmission)
// ============================================================================

void test_stress_continuous_tx(void) {
    TEST_ASSERT_TRUE(g_physical->init());
    
    printf("[INFO] Stress test: 100 consecutive transmissions\n");
    
    int successCount = 0;
    int failCount = 0;
    
    for (int i = 0; i < 100; i++) {
        Tp1Frame frame;
        frame.control = 0xBC;
        frame.source = IndividualAddress(0x1101);
        frame.destination = GroupAddress(0x0314);
        frame.destinationType = AddressType::Group;
        frame.length = 1;
        frame.data[0] = i & 0xFF;  // Counter
        
        uint8_t frameBuffer[256];
        size_t frameSize = Tp1FrameCodec::encode(frame, frameBuffer);
        
        if (g_physical->send(frameBuffer, frameSize)) {
            successCount++;
        } else {
            failCount++;
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));  // 10ms between frames
    }
    
    printf("[INFO] Stress test complete:\n");
    printf("[INFO]   Success: %d/100\n", successCount);
    printf("[INFO]   Failed: %d/100\n", failCount);
    
    // At least 95% success rate
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(95, successCount,
        "Stress test failed - success rate below 95%");
}

// ============================================================================
// Test 10: Long-term Stability (1 hour test)
// ============================================================================

void test_long_term_stability(void) {
    TEST_ASSERT_TRUE(g_physical->init());
    
    printf("[INFO] Long-term stability test: 1 hour\n");
    printf("[INFO] This test will run for 60 minutes\n");
    printf("[INFO] Press any key to skip...\n");
    
    int totalFrames = 0;
    int successFrames = 0;
    int64_t startTime = esp_timer_get_time();
    
    // Run for 1 hour (3600 seconds)
    for (int minute = 0; minute < 60; minute++) {
        printf("[INFO] Minute %d/60...\n", minute + 1);
        
        // Send 10 frames per minute
        for (int i = 0; i < 10; i++) {
            Tp1Frame frame;
            frame.control = 0xBC;
            frame.source = IndividualAddress(0x1101);
            frame.destination = GroupAddress(0x0314);
            frame.destinationType = AddressType::Group;
            frame.length = 2;
            frame.data[0] = minute & 0xFF;
            frame.data[1] = i & 0xFF;
            
            uint8_t frameBuffer[256];
            size_t frameSize = Tp1FrameCodec::encode(frame, frameBuffer);
            
            totalFrames++;
            if (g_physical->send(frameBuffer, frameSize)) {
                successFrames++;
            }
            
            vTaskDelay(pdMS_TO_TICKS(6000));  // 6 seconds between frames
        }
    }
    
    int64_t endTime = esp_timer_get_time();
    int64_t duration = (endTime - startTime) / 1000000;  // Convert to seconds
    
    printf("[INFO] Long-term test complete:\n");
    printf("[INFO]   Duration: %lld seconds\n", duration);
    printf("[INFO]   Total frames: %d\n", totalFrames);
    printf("[INFO]   Success: %d\n", successFrames);
    printf("[INFO]   Success rate: %.2f%%\n", 
           (successFrames * 100.0) / totalFrames);
    
    // Require 99% success for long-term stability
    float successRate = (successFrames * 100.0) / totalFrames;
    TEST_ASSERT_GREATER_OR_EQUAL_MESSAGE(99.0, successRate,
        "Long-term stability failed - success rate below 99%");
}

// ============================================================================
// Main Test Runner
// ============================================================================

extern "C" void app_main(void) {
    printf("\n");
    printf("==============================================\n");
    printf("ESP32 KNX TP1 Hardware Validation Test Suite\n");
    printf("==============================================\n");
    printf("\n");
    printf("Hardware Setup:\n");
    printf("  TX Pin: GPIO%d\n", TEST_TX_PIN);
    printf("  RX Pin: GPIO%d\n", TEST_RX_PIN);
    printf("  RMT TX Channel: %d\n", TEST_TX_CHANNEL);
    printf("  RMT RX Channel: %d\n", TEST_RX_CHANNEL);
    printf("\n");
    printf("Please ensure KNX transceiver is connected!\n");
    printf("\n");
    
    vTaskDelay(pdMS_TO_TICKS(2000));  // Wait for serial monitor
    
    UNITY_BEGIN();
    
    RUN_TEST(test_hardware_init);
    RUN_TEST(test_bus_voltage_detection);
    RUN_TEST(test_manchester_timing);
    RUN_TEST(test_frame_transmission);
    RUN_TEST(test_frame_reception);
    RUN_TEST(test_loopback);
    RUN_TEST(test_collision_detection);
    RUN_TEST(test_bus_monitor_mode);
    RUN_TEST(test_stress_continuous_tx);
    
    // Uncomment for long-term test (1 hour)
    // RUN_TEST(test_long_term_stability);
    
    UNITY_END();
}

#else
#error "This test suite requires ESP32 platform"
#endif // ESP_PLATFORM
