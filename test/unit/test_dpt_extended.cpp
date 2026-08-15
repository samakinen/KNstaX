// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_dpt_extended.cpp
 * @brief Unit tests for extended/specialized Data Point Types
 * 
 * @spec KNX Specification 03/07/02 Datapoint Types v2.1
 * @note Tests for DPT types commonly used but previously untested:
 *       - DPT 15: Access Control
 *       - DPT 18: Scene Control
 *       - DPT 20: HVAC Mode
 *       - DPT 232: RGB Color
 *       - DPT 242: xyY Color
 *       - DPT 243: HSV Color
 *       - DPT 244: Color Transition
 */

#include "unity.h"
#include "knx/application/dpt.hpp"
#include "dpt_test_helpers.hpp"
#include <vector>

using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

// ============================
// DPT 15 - Access Control (4 bytes)
// Format: D7..D4=digit, D3=error, D2=permission, D1=read, D0=encrypt
// ============================

void test_dpt15_encode_basic_access(void) {
    Dpt15::Value access{1, 2, 3, 4, false, true, false, false};
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt15::encode(access, out); });
    TEST_ASSERT_EQUAL(4, data.size());
    
    // Each byte has digit in upper nibble, control bits in lower
    // Control bits: P=1 (permission granted) = 0x04
    TEST_ASSERT_EQUAL(0x44, data[0]);  // digit3=4, control=0x04
    TEST_ASSERT_EQUAL(0x34, data[1]);  // digit2=3, control=0x04
    TEST_ASSERT_EQUAL(0x24, data[2]);  // digit1=2, control=0x04
    TEST_ASSERT_EQUAL(0x14, data[3]);  // digit0=1, control=0x04
}

void test_dpt15_decode_access_denied(void) {
    // Control bits: E=1 (error), P=0 (permission denied), R=1 (read), C=0 (not encrypted)
    // Control nibble = 1010 = 0x0A
    std::vector<uint8_t> data{0x5A, 0x6A, 0x7A, 0x8A};  // digits 5,6,7,8 with control=0x0A
    Dpt15::Value access;
    TEST_ASSERT_TRUE(Dpt15::decode(data, access).isOk());
    
    TEST_ASSERT_EQUAL(8, access.digit0);
    TEST_ASSERT_EQUAL(7, access.digit1);
    TEST_ASSERT_EQUAL(6, access.digit2);
    TEST_ASSERT_EQUAL(5, access.digit3);
    TEST_ASSERT_FALSE(access.permission);  // Permission denied (P bit = 0)
    TEST_ASSERT_TRUE(access.error);        // Error set (E bit = 1)
}

// ============================
// DPT 18 - Scene Control (1 byte)
// Format: bit 7=activate/learn, bits 5-0=scene number (0-63)
// ============================

void test_dpt18_encode_activate_scene(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt18::encode(5, false, out); });  // Activate scene 5
    TEST_ASSERT_EQUAL(1, data.size());
    TEST_ASSERT_EQUAL(0x05, data[0]);
}

void test_dpt18_encode_learn_scene(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt18::encode(10, true, out); });  // Learn scene 10
    TEST_ASSERT_EQUAL(1, data.size());
    TEST_ASSERT_EQUAL(0x8A, data[0]);  // bit 7 set for learn
}

void test_dpt18_invalid_scene_number(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt18::encode(64, false, out); }).isError());   // Must be 0-63
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt18::encode(255, false, out); }).isError());  // Must be 0-63
}

// ============================
// DPT 20 - HVAC Mode (1 byte)
// Values: Auto=0, Comfort=1, Standby=2, Economy=3, Building Protection=4
// ============================

void test_dpt20_encode_hvac_modes(void) {
    // Auto mode
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt20::encode(Dpt20::Mode::Auto, out); });
    TEST_ASSERT_EQUAL(0x00, data[0]);
    
    // Comfort mode
    data = encodePayload([](std::span<uint8_t> out) { return Dpt20::encode(Dpt20::Mode::Comfort, out); });
    TEST_ASSERT_EQUAL(0x01, data[0]);
    
    // Building Protection
    data = encodePayload([](std::span<uint8_t> out) { return Dpt20::encode(Dpt20::Mode::BuildingProtection, out); });
    TEST_ASSERT_EQUAL(0x04, data[0]);
}

void test_dpt20_decode_hvac_modes(void) {
    std::vector<uint8_t> data{0x02};
    Dpt20::Mode mode;
    TEST_ASSERT_TRUE(Dpt20::decode(data, mode).isOk());
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(Dpt20::Mode::Standby), static_cast<uint8_t>(mode));
}

// ============================
// DPT 232 - RGB Color (3 bytes)
// Format: R, G, B (each 0-255)
// ============================

void test_dpt232_encode_primary_colors(void) {
    // Red
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt232::encode(255, 0, 0, out); });
    TEST_ASSERT_EQUAL(3, data.size());
    TEST_ASSERT_EQUAL(0xFF, data[0]);
    TEST_ASSERT_EQUAL(0x00, data[1]);
    TEST_ASSERT_EQUAL(0x00, data[2]);
    
    // Green
    data = encodePayload([](std::span<uint8_t> out) { return Dpt232::encode(0, 255, 0, out); });
    TEST_ASSERT_EQUAL(0x00, data[0]);
    TEST_ASSERT_EQUAL(0xFF, data[1]);
    TEST_ASSERT_EQUAL(0x00, data[2]);
    
    // Blue
    data = encodePayload([](std::span<uint8_t> out) { return Dpt232::encode(0, 0, 255, out); });
    TEST_ASSERT_EQUAL(0x00, data[0]);
    TEST_ASSERT_EQUAL(0x00, data[1]);
    TEST_ASSERT_EQUAL(0xFF, data[2]);
}

void test_dpt232_encode_white_and_black(void) {
    // White
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt232::encode(255, 255, 255, out); });
    TEST_ASSERT_EQUAL(3, data.size());
    TEST_ASSERT_EQUAL(0xFF, data[0]);
    TEST_ASSERT_EQUAL(0xFF, data[1]);
    TEST_ASSERT_EQUAL(0xFF, data[2]);
    
    // Black
    data = encodePayload([](std::span<uint8_t> out) { return Dpt232::encode(0, 0, 0, out); });
    TEST_ASSERT_EQUAL(0x00, data[0]);
    TEST_ASSERT_EQUAL(0x00, data[1]);
    TEST_ASSERT_EQUAL(0x00, data[2]);
}

void test_dpt232_roundtrip(void) {
    uint8_t r_out, g_out, b_out;
    
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt232::encode(128, 64, 192, out); });
    TEST_ASSERT_TRUE(Dpt232::decode(data, r_out, g_out, b_out).isOk());
    
    TEST_ASSERT_EQUAL(128, r_out);
    TEST_ASSERT_EQUAL(64, g_out);
    TEST_ASSERT_EQUAL(192, b_out);
}

// ============================
// DPT 242 - xyY Color (6 bytes)
// Format: x (uint16), y (uint16), Y/Brightness (uint8), valid flag (uint8)
// ============================

void test_dpt242_encode_color_point(void) {
    Dpt242::Value color{32768, 32768, 128, true};  // Mid-point, 50% brightness, valid
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt242::encode(color, out); });
    TEST_ASSERT_EQUAL(6, data.size());
    TEST_ASSERT_EQUAL(0x80, data[0]);  // x high byte
    TEST_ASSERT_EQUAL(0x00, data[1]);  // x low byte
    TEST_ASSERT_EQUAL(0x80, data[2]);  // y high byte
    TEST_ASSERT_EQUAL(0x00, data[3]);  // y low byte
    TEST_ASSERT_EQUAL(128, data[4]);   // brightness
    TEST_ASSERT_EQUAL(0x01, data[5]);  // valid flag
}

// ============================
// DPT 243 - HSV Color (3 bytes)
// Format: H (uint8 0-360°), S (uint8 0-100%), V (uint8 0-100%)
// ============================

void test_dpt243_encode_hsv_values(void) {
    // Red: H=0°, S=100%, V=100%
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt243::encode(0, 100, 100, out); });
    TEST_ASSERT_EQUAL(3, data.size());
    TEST_ASSERT_EQUAL(0x00, data[0]);  // Hue 0°
    TEST_ASSERT_EQUAL(0xFF, data[1]);  // Sat 100%
    TEST_ASSERT_EQUAL(0xFF, data[2]);  // Val 100%
    
    // Green: H=120°, S=100%, V=100%
    data = encodePayload([](std::span<uint8_t> out) { return Dpt243::encode(120, 100, 100, out); });
    TEST_ASSERT_EQUAL(85, data[0]);    // Hue 120° (120*255/360 = 85)
    TEST_ASSERT_EQUAL(0xFF, data[1]);  // Sat 100%
    TEST_ASSERT_EQUAL(0xFF, data[2]);  // Val 100%
}

// ============================
// DPT 244 - Color Transition (6 bytes)
// ============================

void test_dpt244_encode_transition(void) {
    Dpt244::Value transition{255, 128, 64, 1000, 0};
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt244::encode(transition, out); });
    TEST_ASSERT_EQUAL(6, data.size());
    TEST_ASSERT_EQUAL(255, data[0]);  // Red
    TEST_ASSERT_EQUAL(128, data[1]);  // Green
    TEST_ASSERT_EQUAL(64, data[2]);   // Blue
    TEST_ASSERT_EQUAL(0x03, data[3]); // Fade time high byte (1000 = 0x03E8)
    TEST_ASSERT_EQUAL(0xE8, data[4]); // Fade time low byte
    TEST_ASSERT_EQUAL(0x00, data[5]); // Reserved
}

// ============================
// Error Handling Tests
// ============================

void test_dpt_invalid_data_handling(void) {
    // Test that all DPT decoders properly handle invalid input
    // This is a general test for robustness
    
    float float_val;
    uint8_t uint8_val;
    bool bool_val;
    
    // Empty data should fail for all types
    TEST_ASSERT_TRUE(Dpt1::decode(std::span<const uint8_t>{}, bool_val).isError());
    TEST_ASSERT_TRUE(Dpt5::decode(std::span<const uint8_t>{}, uint8_val).isError());
    TEST_ASSERT_TRUE(Dpt9::decode(std::span<const uint8_t>{}, float_val).isError());
    
    // Insufficient data should fail
    TEST_ASSERT_TRUE(Dpt9::decode(std::array<uint8_t, 1>{0x00}, float_val).isError());  // Need 2 bytes
}

// Test runner
int run_all_dpt_extended_tests(void) {
    UNITY_BEGIN();
    
    // DPT 15 tests (Access Control)
    RUN_TEST(test_dpt15_encode_basic_access);
    RUN_TEST(test_dpt15_decode_access_denied);
    
    // DPT 18 tests (Scene Control)
    RUN_TEST(test_dpt18_encode_activate_scene);
    RUN_TEST(test_dpt18_encode_learn_scene);
    RUN_TEST(test_dpt18_invalid_scene_number);
    
    // DPT 20 tests (HVAC Mode)
    RUN_TEST(test_dpt20_encode_hvac_modes);
    RUN_TEST(test_dpt20_decode_hvac_modes);
    
    // DPT 232 tests (RGB Color)
    RUN_TEST(test_dpt232_encode_primary_colors);
    RUN_TEST(test_dpt232_encode_white_and_black);
    RUN_TEST(test_dpt232_roundtrip);
    
    // DPT 242 tests (xyY Color)
    RUN_TEST(test_dpt242_encode_color_point);
    
    // DPT 243 tests (HSV Color)
    RUN_TEST(test_dpt243_encode_hsv_values);
    
    // DPT 244 tests (Color Transition)
    RUN_TEST(test_dpt244_encode_transition);
    
    // Error handling
    RUN_TEST(test_dpt_invalid_data_handling);
    
    return UNITY_END();
}

int main() {
    return run_all_dpt_extended_tests();
}
