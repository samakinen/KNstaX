// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_config_validation.cpp
 * @brief Unit tests for configuration validation
 */

#include "unity.h"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/physical/bitbang_board_profile.hpp"
#include "knx/physical/bitbang_driver_interface.hpp"

using namespace knx;

void setUp(void) {
    // Set up test fixtures
}

void tearDown(void) {
    // Clean up test fixtures
}

// ============================================================================
// Tp1DataLinkConfig Validation Tests
// ============================================================================

void test_tp1_config_defaults_valid() {
    datalink::Tp1DataLinkConfig config = datalink::Tp1DataLinkConfig::defaults();
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isOk());
}

void test_tp1_config_invalid_stack_size_too_small() {
    datalink::Tp1DataLinkConfig config;
    config.rxTaskStackSize = 256;  // Too small
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::InvalidParameter, result.error());
}

void test_tp1_config_invalid_stack_size_too_large() {
    datalink::Tp1DataLinkConfig config;
    config.rxTaskStackSize = 100000;  // Too large
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
}

void test_tp1_config_valid_stack_sizes() {
    uint32_t validSizes[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    
    for (uint32_t size : validSizes) {
        datalink::Tp1DataLinkConfig config;
        config.rxTaskStackSize = size;
        auto result = config.validate();
        TEST_ASSERT_TRUE(result.isOk());
    }
}

void test_tp1_config_invalid_priority_too_high() {
    datalink::Tp1DataLinkConfig config;
    config.rxTaskPriority = 30;  // Too high
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
}

void test_tp1_config_valid_priorities() {
    for (uint32_t prio = 0; prio <= 24; prio++) {
        datalink::Tp1DataLinkConfig config;
        config.rxTaskPriority = prio;
        auto result = config.validate();
        TEST_ASSERT_TRUE(result.isOk());
    }
}

void test_tp1_config_invalid_mutex_timeout_too_large() {
    datalink::Tp1DataLinkConfig config;
    config.txMutexTimeout = 70000;  // > 60 seconds
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
}

void test_tp1_config_valid_mutex_timeouts() {
    uint32_t validTimeouts[] = {0, 100, 500, 1000, 5000, 10000, 60000};
    
    for (uint32_t timeout : validTimeouts) {
        datalink::Tp1DataLinkConfig config;
        config.txMutexTimeout = timeout;
        auto result = config.validate();
        TEST_ASSERT_TRUE(result.isOk());
    }
}

// ============================================================================
// BitBangConfig Validation Tests
// ============================================================================

void test_bitbang_config_defaults_valid() {
    physical::BitBangConfig config;
    config.txPin = 1;  // Change from default to avoid same pin
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isOk());
}

void test_bitbang_config_invalid_tx_pin_too_high() {
    physical::BitBangConfig config;
    config.txPin = 70;  // Too high
    config.rxPin = 1;
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::InvalidParameter, result.error());
}

void test_bitbang_config_invalid_rx_pin_too_high() {
    physical::BitBangConfig config;
    config.txPin = 1;
    config.rxPin = 70;  // Too high
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
}

void test_bitbang_config_invalid_same_pins() {
    physical::BitBangConfig config;
    config.txPin = 5;
    config.rxPin = 5;  // Same as TX
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
}

void test_bitbang_config_valid_pins() {
    uint8_t validPins[][2] = {{0, 1}, {2, 3}, {10, 11}, {20, 21}, {30, 31}, {50, 60}};
    
    for (auto& pins : validPins) {
        physical::BitBangConfig config;
        config.txPin = pins[0];
        config.rxPin = pins[1];
        auto result = config.validate();
        TEST_ASSERT_TRUE(result.isOk());
    }
}

void test_bitbang_config_invalid_baud_rate_too_low() {
    physical::BitBangConfig config;
    config.txPin = 1;
    config.rxPin = 2;
    config.baudRate = 100;  // Too low
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
}

void test_bitbang_config_invalid_baud_rate_too_high() {
    physical::BitBangConfig config;
    config.txPin = 1;
    config.rxPin = 2;
    config.baudRate = 200000;  // Too high
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
}

void test_bitbang_config_valid_baud_rates() {
    // KNX TP1 only supports 9600 baud with Manchester encoding (19200 bit rate)
    physical::BitBangConfig config;
    config.txPin = 1;
    config.rxPin = 2;
    config.baudRate = 9600;   // TP1 requires exactly 9600
    config.bitRate = 19200;    // Manchester doubles the bit rate
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isOk());
}

void test_bitbang_config_invalid_zero_timing_sum() {
    physical::BitBangConfig config;
    config.txPin = 1;
    config.rxPin = 2;
    config.zeroActiveTimeUs = 34;
    config.zeroEqualizationTimeUs = 69;
    auto result = config.validate();
    TEST_ASSERT_TRUE(result.isError());
}

void test_apply_bitbang_board_profile_applies_transceiver_traits() {
    physical::BitBangBoardProfile profile;
    profile.txPin = 6;
    profile.rxPin = 7;
    profile.enablePullup = false;
    profile.txDominantHigh = false;
    profile.rxDominantHigh = false;
    profile.serialBitTimeUs = 108;
    profile.zeroActiveTimeUs = 40;
    profile.zeroEqualizationTimeUs = 68;
    profile.bitSamplingOffsetUs = 81;

    physical::BitBangConfig config;
    physical::applyBitBangBoardProfile(profile, config);

    TEST_ASSERT_EQUAL_UINT8(6, config.txPin);
    TEST_ASSERT_EQUAL_UINT8(7, config.rxPin);
    TEST_ASSERT_FALSE(config.enablePullup);
    TEST_ASSERT_FALSE(config.txDominantHigh);
    TEST_ASSERT_FALSE(config.rxDominantHigh);
    TEST_ASSERT_EQUAL_UINT32(108, config.serialBitTimeUs);
    TEST_ASSERT_EQUAL_UINT32(40, config.zeroActiveTimeUs);
    TEST_ASSERT_EQUAL_UINT32(68, config.zeroEqualizationTimeUs);
    TEST_ASSERT_EQUAL_UINT32(81, config.bitSamplingOffsetUs);
    TEST_ASSERT_TRUE(config.validate().isOk());
}

// ============================================================================
// Main Test Runner
// ============================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();
    
    // Tp1DataLinkConfig tests
    RUN_TEST(test_tp1_config_defaults_valid);
    RUN_TEST(test_tp1_config_invalid_stack_size_too_small);
    RUN_TEST(test_tp1_config_invalid_stack_size_too_large);
    RUN_TEST(test_tp1_config_valid_stack_sizes);
    RUN_TEST(test_tp1_config_invalid_priority_too_high);
    RUN_TEST(test_tp1_config_valid_priorities);
    RUN_TEST(test_tp1_config_invalid_mutex_timeout_too_large);
    RUN_TEST(test_tp1_config_valid_mutex_timeouts);
    
    // BitBangConfig tests
    RUN_TEST(test_bitbang_config_defaults_valid);
    RUN_TEST(test_bitbang_config_invalid_tx_pin_too_high);
    RUN_TEST(test_bitbang_config_invalid_rx_pin_too_high);
    RUN_TEST(test_bitbang_config_invalid_same_pins);
    RUN_TEST(test_bitbang_config_valid_pins);
    RUN_TEST(test_bitbang_config_invalid_baud_rate_too_low);
    RUN_TEST(test_bitbang_config_invalid_baud_rate_too_high);
    RUN_TEST(test_bitbang_config_valid_baud_rates);
    RUN_TEST(test_bitbang_config_invalid_zero_timing_sum);
    RUN_TEST(test_apply_bitbang_board_profile_applies_transceiver_traits);
    
    return UNITY_END();
}
