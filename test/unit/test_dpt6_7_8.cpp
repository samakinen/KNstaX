// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_dpt6_7_8.cpp
 * @brief Unit tests for DPT 6 (8-bit signed), DPT 7 (16-bit unsigned), and DPT 8 (16-bit signed)
 */

#include "../../include/knx/application/dpt.hpp"
#include "dpt_test_helpers.hpp"
#include "../unity_mock/unity.h"
#include <array>
#include <span>
#include <vector>

using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

// ============================
// DPT 6 - 8-bit Signed
// ============================

void test_Dpt6_EncodePositiveValues(void) {
    // Zero
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt6::encode(0, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    
    // Small positive
    data = encodePayload([](std::span<uint8_t> out) { return Dpt6::encode(10, out); });
    TEST_ASSERT_EQUAL_HEX8(0x0A, data[0]);
    
    // Maximum positive
    data = encodePayload([](std::span<uint8_t> out) { return Dpt6::encode(127, out); });
    TEST_ASSERT_EQUAL_HEX8(0x7F, data[0]);
}

void test_Dpt6_EncodeNegativeValues(void) {
    // Small negative
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt6::encode(-1, out); });
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[0]);
    
    data = encodePayload([](std::span<uint8_t> out) { return Dpt6::encode(-10, out); });
    TEST_ASSERT_EQUAL_HEX8(0xF6, data[0]);
    
    // Minimum negative
    data = encodePayload([](std::span<uint8_t> out) { return Dpt6::encode(-128, out); });
    TEST_ASSERT_EQUAL_HEX8(0x80, data[0]);
}

void test_Dpt6_DecodePositiveValues(void) {
    int8_t value;
    
    TEST_ASSERT_TRUE(Dpt6::decode(std::to_array<uint8_t>({0x00}), value).isOk());
    TEST_ASSERT_EQUAL_INT8(0, value);
    
    TEST_ASSERT_TRUE(Dpt6::decode(std::to_array<uint8_t>({0x64}), value).isOk());
    TEST_ASSERT_EQUAL_INT8(100, value);
    
    TEST_ASSERT_TRUE(Dpt6::decode(std::to_array<uint8_t>({0x7F}), value).isOk());
    TEST_ASSERT_EQUAL_INT8(127, value);
}

void test_Dpt6_DecodeNegativeValues(void) {
    int8_t value;
    
    TEST_ASSERT_TRUE(Dpt6::decode(std::to_array<uint8_t>({0xFF}), value).isOk());
    TEST_ASSERT_EQUAL_INT8(-1, value);
    
    TEST_ASSERT_TRUE(Dpt6::decode(std::to_array<uint8_t>({0xEC}), value).isOk());
    TEST_ASSERT_EQUAL_INT8(-20, value);
    
    TEST_ASSERT_TRUE(Dpt6::decode(std::to_array<uint8_t>({0x80}), value).isOk());
    TEST_ASSERT_EQUAL_INT8(-128, value);
}

void test_Dpt6_DecodeEmptyData(void) {
    int8_t value;
    TEST_ASSERT_TRUE(Dpt6::decode(std::span<const uint8_t>{}, value).isError());
}

void test_Dpt6_RoundTrip(void) {
    int8_t original = -42;
    int8_t decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt6::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt6::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_INT8(original, decoded);
}

// ============================
// DPT 7 - 16-bit Unsigned
// ============================

void test_Dpt7_EncodeSmallValues(void) {
    // Zero
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt7::encode(0, out); });
    TEST_ASSERT_EQUAL_UINT(2, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    
    // 100
    data = encodePayload([](std::span<uint8_t> out) { return Dpt7::encode(100, out); });
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x64, data[1]);
    
    // 255
    data = encodePayload([](std::span<uint8_t> out) { return Dpt7::encode(255, out); });
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[1]);
}

void test_Dpt7_EncodeLargeValues(void) {
    // 256
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt7::encode(256, out); });
    TEST_ASSERT_EQUAL_HEX8(0x01, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    
    // 1000
    data = encodePayload([](std::span<uint8_t> out) { return Dpt7::encode(1000, out); });
    TEST_ASSERT_EQUAL_HEX8(0x03, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xE8, data[1]);
    
    // Maximum
    data = encodePayload([](std::span<uint8_t> out) { return Dpt7::encode(65535, out); });
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[1]);
}

void test_Dpt7_DecodeSmallValues(void) {
    uint16_t value;
    
    TEST_ASSERT_TRUE(Dpt7::decode(std::to_array<uint8_t>({0x00, 0x00}), value).isOk());
    TEST_ASSERT_EQUAL_UINT16(0, value);
    
    TEST_ASSERT_TRUE(Dpt7::decode(std::to_array<uint8_t>({0x00, 0x64}), value).isOk());
    TEST_ASSERT_EQUAL_UINT16(100, value);
}

void test_Dpt7_DecodeLargeValues(void) {
    uint16_t value;
    
    TEST_ASSERT_TRUE(Dpt7::decode(std::to_array<uint8_t>({0x04, 0xD2}), value).isOk());
    TEST_ASSERT_EQUAL_UINT16(1234, value);
    
    TEST_ASSERT_TRUE(Dpt7::decode(std::to_array<uint8_t>({0xFF, 0xFF}), value).isOk());
    TEST_ASSERT_EQUAL_UINT16(65535, value);
}

void test_Dpt7_DecodeInsufficientData(void) {
    uint16_t value;
    
    TEST_ASSERT_TRUE(Dpt7::decode(std::span<const uint8_t>{}, value).isError());
    TEST_ASSERT_TRUE(Dpt7::decode(std::to_array<uint8_t>({0x12}), value).isError());
}

void test_Dpt7_RoundTrip(void) {
    uint16_t original = 30000;
    uint16_t decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt7::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt7::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT16(original, decoded);
}

void test_Dpt7_BigEndianByteOrder(void) {
    // Verify big-endian encoding (MSB first)
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt7::encode(0x1234, out); });
    TEST_ASSERT_EQUAL_HEX8(0x12, data[0]);  // MSB
    TEST_ASSERT_EQUAL_HEX8(0x34, data[1]);  // LSB
}

// ============================
// DPT 8 - 16-bit Signed
// ============================

void test_Dpt8_EncodePositiveValues(void) {
    // Zero
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt8::encode(0, out); });
    TEST_ASSERT_EQUAL_UINT(2, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    
    // 1000
    data = encodePayload([](std::span<uint8_t> out) { return Dpt8::encode(1000, out); });
    TEST_ASSERT_EQUAL_HEX8(0x03, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xE8, data[1]);
    
    // Maximum positive
    data = encodePayload([](std::span<uint8_t> out) { return Dpt8::encode(32767, out); });
    TEST_ASSERT_EQUAL_HEX8(0x7F, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[1]);
}

void test_Dpt8_EncodeNegativeValues(void) {
    // -1
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt8::encode(-1, out); });
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[1]);
    
    // -1000
    data = encodePayload([](std::span<uint8_t> out) { return Dpt8::encode(-1000, out); });
    TEST_ASSERT_EQUAL_HEX8(0xFC, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x18, data[1]);
    
    // Minimum negative
    data = encodePayload([](std::span<uint8_t> out) { return Dpt8::encode(-32768, out); });
    TEST_ASSERT_EQUAL_HEX8(0x80, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
}

void test_Dpt8_DecodePositiveValues(void) {
    int16_t value;
    
    TEST_ASSERT_TRUE(Dpt8::decode(std::to_array<uint8_t>({0x00, 0x00}), value).isOk());
    TEST_ASSERT_EQUAL_INT16(0, value);
    
    TEST_ASSERT_TRUE(Dpt8::decode(std::to_array<uint8_t>({0x01, 0xF4}), value).isOk());
    TEST_ASSERT_EQUAL_INT16(500, value);
    
    TEST_ASSERT_TRUE(Dpt8::decode(std::to_array<uint8_t>({0x7F, 0xFF}), value).isOk());
    TEST_ASSERT_EQUAL_INT16(32767, value);
}

void test_Dpt8_DecodeNegativeValues(void) {
    int16_t value;
    
    TEST_ASSERT_TRUE(Dpt8::decode(std::to_array<uint8_t>({0xFF, 0xFF}), value).isOk());
    TEST_ASSERT_EQUAL_INT16(-1, value);
    
    TEST_ASSERT_TRUE(Dpt8::decode(std::to_array<uint8_t>({0xFE, 0x0C}), value).isOk());
    TEST_ASSERT_EQUAL_INT16(-500, value);
    
    TEST_ASSERT_TRUE(Dpt8::decode(std::to_array<uint8_t>({0x80, 0x00}), value).isOk());
    TEST_ASSERT_EQUAL_INT16(-32768, value);
}

void test_Dpt8_DecodeInsufficientData(void) {
    int16_t value;
    
    TEST_ASSERT_TRUE(Dpt8::decode(std::span<const uint8_t>{}, value).isError());
    TEST_ASSERT_TRUE(Dpt8::decode(std::to_array<uint8_t>({0x12}), value).isError());
}

void test_Dpt8_RoundTrip(void) {
    int16_t original = -12345;
    int16_t decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt8::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt8::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_INT16(original, decoded);
}

void test_Dpt8_BigEndianByteOrder(void) {
    // Verify big-endian encoding for positive value
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt8::encode(0x1234, out); });
    TEST_ASSERT_EQUAL_HEX8(0x12, data[0]);  // MSB
    TEST_ASSERT_EQUAL_HEX8(0x34, data[1]);  // LSB
    
    // Verify big-endian encoding for negative value (-1000 = 0xFC18)
    data = encodePayload([](std::span<uint8_t> out) { return Dpt8::encode(-1000, out); });
    TEST_ASSERT_EQUAL_HEX8(0xFC, data[0]);  // MSB
    TEST_ASSERT_EQUAL_HEX8(0x18, data[1]);  // LSB
}

int main(void) {
    UNITY_BEGIN();
    
    // DPT 6 tests
    RUN_TEST(test_Dpt6_EncodePositiveValues);
    RUN_TEST(test_Dpt6_EncodeNegativeValues);
    RUN_TEST(test_Dpt6_DecodePositiveValues);
    RUN_TEST(test_Dpt6_DecodeNegativeValues);
    RUN_TEST(test_Dpt6_DecodeEmptyData);
    RUN_TEST(test_Dpt6_RoundTrip);
    
    // DPT 7 tests
    RUN_TEST(test_Dpt7_EncodeSmallValues);
    RUN_TEST(test_Dpt7_EncodeLargeValues);
    RUN_TEST(test_Dpt7_DecodeSmallValues);
    RUN_TEST(test_Dpt7_DecodeLargeValues);
    RUN_TEST(test_Dpt7_DecodeInsufficientData);
    RUN_TEST(test_Dpt7_RoundTrip);
    RUN_TEST(test_Dpt7_BigEndianByteOrder);
    
    // DPT 8 tests
    RUN_TEST(test_Dpt8_EncodePositiveValues);
    RUN_TEST(test_Dpt8_EncodeNegativeValues);
    RUN_TEST(test_Dpt8_DecodePositiveValues);
    RUN_TEST(test_Dpt8_DecodeNegativeValues);
    RUN_TEST(test_Dpt8_DecodeInsufficientData);
    RUN_TEST(test_Dpt8_RoundTrip);
    RUN_TEST(test_Dpt8_BigEndianByteOrder);
    
    return UNITY_END();
}
