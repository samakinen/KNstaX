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
 *       - DPT 243: Colour transition xyY
 *       - DPT 244: Converter status
 */

#include "unity.h"
#include "knx/application/dpt.hpp"
#include "dpt_test_helpers.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <vector>

using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

// ============================
// DPT 15.000 - DPT_Access_Data, U4U4U4U4U4U4B4N4 (03_07_02 §3.16)
// [D6 D5] [D4 D3] [D2 D1] [E P D C Index]
// ============================

void test_dpt15_example6_golden_bytes(void) {
    // EXAMPLE 6: code "123456", no error, permission accepted, left to right,
    // no encryption, index 13 -> 0x12 0x34 0x56 0x4D.
    Dpt15::Value access{};
    access.accessCode = 123456;
    access.permission = true;
    access.index = 13;
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt15::encode(access, out); });
    const std::array<uint8_t, 4> expected{0x12, 0x34, 0x56, 0x4D};
    TEST_ASSERT_EQUAL(4, data.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), data.data(), 4);

    Dpt15::Value decoded{};
    TEST_ASSERT_TRUE(Dpt15::decode(expected, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT32(123456, decoded.accessCode);
    TEST_ASSERT_TRUE(decoded.permission);
    TEST_ASSERT_FALSE(decoded.error);
    TEST_ASSERT_FALSE(decoded.readDirection);
    TEST_ASSERT_FALSE(decoded.encrypted);
    TEST_ASSERT_EQUAL_UINT8(13, decoded.index);
}

void test_dpt15_example7_golden_bytes(void) {
    // EXAMPLE 7: code "6789", permission not accepted, index 14
    // -> 0x00 0x67 0x89 0x0E (unused leading digits are zero).
    Dpt15::Value access{};
    access.accessCode = 6789;
    access.index = 14;
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt15::encode(access, out); });
    const std::array<uint8_t, 4> expected{0x00, 0x67, 0x89, 0x0E};
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), data.data(), 4);

    Dpt15::Value decoded{};
    TEST_ASSERT_TRUE(Dpt15::decode(expected, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT32(6789, decoded.accessCode);
    TEST_ASSERT_FALSE(decoded.permission);
    TEST_ASSERT_EQUAL_UINT8(14, decoded.index);
}

void test_dpt15_flag_bits_and_ranges(void) {
    Dpt15::Value access{};
    access.error = true;
    access.readDirection = true;
    access.encrypted = true;
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt15::encode(access, out); });
    TEST_ASSERT_EQUAL_HEX8(0xB0, data[3]);  // E . D C

    access.accessCode = 1000000;  // seven digits
    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt15::encode(access, out); }).isError());
    access.accessCode = 0;
    access.index = 16;
    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt15::encode(access, out); }).isError());

    // A digit nibble above 9 is not a decimal digit.
    Dpt15::Value decoded{};
    TEST_ASSERT_TRUE(Dpt15::decode(std::to_array<uint8_t>({0x0A, 0x00, 0x00, 0x00}), decoded).isError());
    TEST_ASSERT_TRUE(Dpt15::decode(std::to_array<uint8_t>({0x00, 0x00, 0xF0, 0x00}), decoded).isError());
    TEST_ASSERT_TRUE(Dpt15::decode(std::to_array<uint8_t>({0x00, 0x00, 0x00}), decoded).isError());
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
// DPT 242.600 - DPT_Colour_xyY, U16U16U8r6B2 (03_07_02 §6.9)
// ============================

void test_dpt242_encode_color_point(void) {
    Dpt242::Value color{};
    color.x = 32768;
    color.y = 0x1234;
    color.brightness = 128;
    color.colourValid = true;
    color.brightnessValid = true;

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt242::encode(color, out); });
    const std::array<uint8_t, 6> expected{0x80, 0x00, 0x12, 0x34, 0x80, 0x03};
    TEST_ASSERT_EQUAL(6, data.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), data.data(), 6);
}

void test_dpt242_validity_bits_are_separate(void) {
    // C (bit 1) = x/y valid, B (bit 0) = brightness valid.
    Dpt242::Value color{};
    color.colourValid = true;
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt242::encode(color, out); });
    TEST_ASSERT_EQUAL_HEX8(0x02, data[5]);
    color.colourValid = false;
    color.brightnessValid = true;
    data = encodePayload([&](std::span<uint8_t> out) { return Dpt242::encode(color, out); });
    TEST_ASSERT_EQUAL_HEX8(0x01, data[5]);

    Dpt242::Value decoded{};
    TEST_ASSERT_TRUE(Dpt242::decode(std::to_array<uint8_t>({0, 0, 0, 0, 0, 0xFE}), decoded).isOk());
    TEST_ASSERT_TRUE(decoded.colourValid);
    TEST_ASSERT_FALSE(decoded.brightnessValid);
}

// ============================
// DPT 243.600 - DPT_Colour_Transition_xyY, U16U16U16U8r6B2 (03_07_02 §6.10)
// Field order: Time Period, x-axis, y-axis, Brightness, [r6 C B].
// ============================

void test_dpt243_colour_transition_layout(void) {
    Dpt243::Value transition{};
    transition.timePeriod = 50;  // 5.0 s
    transition.x = 0xAABB;
    transition.y = 0xCCDD;
    transition.brightness = 0xEE;
    transition.colourValid = true;
    transition.brightnessValid = false;

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt243::encode(transition, out); });
    const std::array<uint8_t, 8> expected{0x00, 0x32, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
    TEST_ASSERT_EQUAL(8, data.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), data.data(), 8);

    Dpt243::Value decoded{};
    TEST_ASSERT_TRUE(Dpt243::decode(expected, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT16(50, decoded.timePeriod);
    TEST_ASSERT_EQUAL_UINT16(0xAABB, decoded.x);
    TEST_ASSERT_EQUAL_UINT16(0xCCDD, decoded.y);
    TEST_ASSERT_EQUAL_UINT8(0xEE, decoded.brightness);
    TEST_ASSERT_TRUE(decoded.colourValid);
    TEST_ASSERT_FALSE(decoded.brightnessValid);
    TEST_ASSERT_TRUE(Dpt243::decode(std::to_array<uint8_t>({0, 0, 0, 0, 0, 0, 0}), decoded).isError());
}

// ============================
// DPT 244.600 - DPT_Converter_Status, N4B4N2N2N2N2 (03_07_02 §6.11)
// [CM(4) 0 0 HS1 HS0] [FP(2) DP(2) PP(2) CF(2)]
// ============================

void test_dpt244_converter_status_layout(void) {
    Dpt244::Value status{};
    status.mode = 5;                      // emergency mode active
    status.hardwiredInhibitActive = true; // HS bit 0
    status.hardwiredSwitchOn = false;
    status.functionTestPending = 2;       // test pending
    status.durationTestPending = 1;       // no test pending
    status.partialDurationTestPending = 0;
    status.failure = 1;                   // no failure detected

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt244::encode(status, out); });
    TEST_ASSERT_EQUAL(2, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x51, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x91, data[1]);  // 10 01 00 01

    Dpt244::Value decoded{};
    TEST_ASSERT_TRUE(Dpt244::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT8(5, decoded.mode);
    TEST_ASSERT_TRUE(decoded.hardwiredInhibitActive);
    TEST_ASSERT_FALSE(decoded.hardwiredSwitchOn);
    TEST_ASSERT_EQUAL_UINT8(2, decoded.functionTestPending);
    TEST_ASSERT_EQUAL_UINT8(1, decoded.durationTestPending);
    TEST_ASSERT_EQUAL_UINT8(0, decoded.partialDurationTestPending);
    TEST_ASSERT_EQUAL_UINT8(1, decoded.failure);
}

void test_dpt244_reserved_values_rejected(void) {
    Dpt244::Value status{};
    status.mode = 10;  // 10..15 reserved
    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt244::encode(status, out); }).isError());
    status.mode = 0;
    status.failure = 3;  // reserved
    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt244::encode(status, out); }).isError());

    Dpt244::Value decoded{};
    TEST_ASSERT_TRUE(Dpt244::decode(std::to_array<uint8_t>({0xA0, 0x00}), decoded).isError());  // CM 10
    TEST_ASSERT_TRUE(Dpt244::decode(std::to_array<uint8_t>({0x04, 0x00}), decoded).isError());  // HS bit 2
    TEST_ASSERT_TRUE(Dpt244::decode(std::to_array<uint8_t>({0x00, 0xC0}), decoded).isError());  // FP 3
    TEST_ASSERT_TRUE(Dpt244::decode(std::to_array<uint8_t>({0x00, 0x03}), decoded).isError());  // CF 3
}

// ============================
// DPT 9 - 2-octet float (03_07_02 §3.10)
// ============================

namespace {
uint16_t dpt9Raw(float value)
{
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(value, out); });
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}
} // namespace

void test_dpt9_invalid_code_is_nan_both_ways(void) {
    // "the encoded value 7FFFh shall always be used to denote invalid data"
    float decoded = 0.0f;
    TEST_ASSERT_TRUE(Dpt9::decode(std::to_array<uint8_t>({0x7F, 0xFF}), decoded).isOk());
    TEST_ASSERT_TRUE(std::isnan(decoded));
    TEST_ASSERT_EQUAL_HEX16(0x7FFF, dpt9Raw(std::numeric_limits<float>::quiet_NaN()));
}

void test_dpt9_range_ends_below_the_invalid_code(void) {
    // Range [-671 088,64 … 670 433,28]: the top is 7FFEh, not 7FFFh.
    TEST_ASSERT_EQUAL_HEX16(0x7FFE, dpt9Raw(670433.28f));
    TEST_ASSERT_EQUAL_HEX16(0xF800, dpt9Raw(-671088.64f));
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt9::encode(670760.96f, out); }).isError());
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt9::encode(700000.0f, out); }).isError());
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) {
        return Dpt9::encode(std::numeric_limits<float>::infinity(), out);
    }).isError());
}

void test_dpt9_rounds_once_to_the_nearest_code(void) {
    // 40.976 = 4097.6 hundredths: E=2 gives M = round(1024.4) = 1024 -> 0x1400.
    // Rounding to 4098 first and then halving twice would give 1025 (0x1401).
    TEST_ASSERT_EQUAL_HEX16(0x1400, dpt9Raw(40.976f));
    // Smallest exponent that fits: 20.47 stays at E=0, 20.48 needs E=1.
    TEST_ASSERT_EQUAL_HEX16(0x07FF, dpt9Raw(20.47f));
    TEST_ASSERT_EQUAL_HEX16(0x0C00, dpt9Raw(20.48f));
    // 21.0 -> 2100 hundredths, E=1, M=1050 = 0x41A -> 0x0C1A (commonly cited value 0x0C1A).
    TEST_ASSERT_EQUAL_HEX16(0x0C1A, dpt9Raw(21.0f));
    // -30.0 -> -3000, E=1, M=-1500 = 0xA24 (12-bit) -> 0x8A24.
    TEST_ASSERT_EQUAL_HEX16(0x8A24, dpt9Raw(-30.0f));
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

    // DPT 15 tests (Access Data)
    RUN_TEST(test_dpt15_example6_golden_bytes);
    RUN_TEST(test_dpt15_example7_golden_bytes);
    RUN_TEST(test_dpt15_flag_bits_and_ranges);

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

    // DPT 242/243/244
    RUN_TEST(test_dpt242_encode_color_point);
    RUN_TEST(test_dpt242_validity_bits_are_separate);
    RUN_TEST(test_dpt243_colour_transition_layout);
    RUN_TEST(test_dpt244_converter_status_layout);
    RUN_TEST(test_dpt244_reserved_values_rejected);

    // DPT 9
    RUN_TEST(test_dpt9_invalid_code_is_nan_both_ways);
    RUN_TEST(test_dpt9_range_ends_below_the_invalid_code);
    RUN_TEST(test_dpt9_rounds_once_to_the_nearest_code);

    // Error handling
    RUN_TEST(test_dpt_invalid_data_handling);

    return UNITY_END();
}

int main() {
    return run_all_dpt_extended_tests();
}
