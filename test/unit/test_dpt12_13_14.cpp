// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_dpt12_13_14.cpp
 * @brief Unit tests for DPT 12 (32-bit unsigned), DPT 13 (32-bit signed), and DPT 14 (32-bit float)
 */

#include "../../include/knx/application/dpt.hpp"
#include "dpt_test_helpers.hpp"
#include "../unity_mock/unity.h"
#include <array>
#include <span>
#include <vector>
#include <cmath>

using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

// ============================
// DPT 12 - 32-bit Unsigned
// ============================

void test_Dpt12_EncodeZero(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt12::encode(0, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[3]);
}

void test_Dpt12_EncodeSmallValue(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt12::encode(1000, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0xE8, data[3]);
}

void test_Dpt12_EncodeLargeValue(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt12::encode(0x12345678, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x12, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x34, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x56, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x78, data[3]);
}

void test_Dpt12_EncodeMaxValue(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt12::encode(0xFFFFFFFF, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[3]);
}

void test_Dpt12_DecodeZero(void) {
    uint32_t value;
    
    TEST_ASSERT_TRUE(Dpt12::decode(std::to_array<uint8_t>({0x00, 0x00, 0x00, 0x00}), value).isOk());
    TEST_ASSERT_EQUAL_UINT32(0, value);
}

void test_Dpt12_DecodeSmallValue(void) {
    uint32_t value;
    
    TEST_ASSERT_TRUE(Dpt12::decode(std::to_array<uint8_t>({0x00, 0x00, 0x03, 0xE8}), value).isOk());
    TEST_ASSERT_EQUAL_UINT32(1000, value);
}

void test_Dpt12_DecodeLargeValue(void) {
    uint32_t value;
    
    TEST_ASSERT_TRUE(Dpt12::decode(std::to_array<uint8_t>({0x12, 0x34, 0x56, 0x78}), value).isOk());
    TEST_ASSERT_EQUAL_UINT32(0x12345678, value);
}

void test_Dpt12_DecodeMaxValue(void) {
    uint32_t value;
    
    TEST_ASSERT_TRUE(Dpt12::decode(std::to_array<uint8_t>({0xFF, 0xFF, 0xFF, 0xFF}), value).isOk());
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFF, value);
}

void test_Dpt12_DecodeInsufficientData(void) {
    uint32_t value;
    
    TEST_ASSERT_TRUE(Dpt12::decode(std::span<const uint8_t>{}, value).isError());
    TEST_ASSERT_TRUE(Dpt12::decode(std::to_array<uint8_t>({0x00}), value).isError());
    TEST_ASSERT_TRUE(Dpt12::decode(std::to_array<uint8_t>({0x00, 0x00}), value).isError());
    TEST_ASSERT_TRUE(Dpt12::decode(std::to_array<uint8_t>({0x00, 0x00, 0x00}), value).isError());
}

void test_Dpt12_RoundTrip(void) {
    uint32_t original = 987654321;
    uint32_t decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt12::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt12::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT32(original, decoded);
}

void test_Dpt12_BigEndianByteOrder(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt12::encode(0x01020304, out); });
    TEST_ASSERT_EQUAL_HEX8(0x01, data[0]);  // MSB
    TEST_ASSERT_EQUAL_HEX8(0x02, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x04, data[3]);  // LSB
}

// ============================
// DPT 13 - 32-bit Signed
// ============================

void test_Dpt13_EncodeZero(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt13::encode(0, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[3]);
}

void test_Dpt13_EncodePositiveValue(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt13::encode(1000000, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0F, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x42, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x40, data[3]);
}

void test_Dpt13_EncodeNegativeValue(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt13::encode(-1000000, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xF0, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBD, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0xC0, data[3]);
}

void test_Dpt13_EncodeMaxPositive(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt13::encode(2147483647, out); });  // INT32_MAX
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x7F, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, data[3]);
}

void test_Dpt13_EncodeMinNegative(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt13::encode(-2147483648LL, out); });  // INT32_MIN
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x80, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[3]);
}

void test_Dpt13_DecodePositiveValue(void) {
    int32_t value;
    
    TEST_ASSERT_TRUE(Dpt13::decode(std::to_array<uint8_t>({0x00, 0x0F, 0x42, 0x40}), value).isOk());
    TEST_ASSERT_EQUAL_INT32(1000000, value);
}

void test_Dpt13_DecodeNegativeValue(void) {
    int32_t value;
    
    TEST_ASSERT_TRUE(Dpt13::decode(std::to_array<uint8_t>({0xFF, 0xF0, 0xBD, 0xC0}), value).isOk());
    TEST_ASSERT_EQUAL_INT32(-1000000, value);
}

void test_Dpt13_DecodeInsufficientData(void) {
    int32_t value;
    
    TEST_ASSERT_TRUE(Dpt13::decode(std::span<const uint8_t>{}, value).isError());
    TEST_ASSERT_TRUE(Dpt13::decode(std::to_array<uint8_t>({0x00, 0x00, 0x00}), value).isError());
}

void test_Dpt13_RoundTrip(void) {
    int32_t original = -123456789;
    int32_t decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt13::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt13::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_INT32(original, decoded);
}

// ============================
// DPT 14 - 32-bit Float
// ============================

void test_Dpt14_EncodeZero(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt14::encode(0.0f, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[3]);
}

void test_Dpt14_EncodePositiveValue(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt14::encode(1.0f, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    // IEEE 754: 1.0f = 0x3F800000
    TEST_ASSERT_EQUAL_HEX8(0x3F, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[3]);
}

void test_Dpt14_EncodeNegativeValue(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt14::encode(-1.0f, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    // IEEE 754: -1.0f = 0xBF800000
    TEST_ASSERT_EQUAL_HEX8(0xBF, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[3]);
}

void test_Dpt14_EncodeFractionalValue(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt14::encode(3.14159f, out); });
    TEST_ASSERT_EQUAL_UINT(4, data.size());
    // Verify it's non-zero and reasonable
    TEST_ASSERT_NOT_EQUAL(0x00, data[0]);
}

void test_Dpt14_EncodeInvalidNaN(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt14::encode(NAN, out); }).isError());
}

void test_Dpt14_EncodeInvalidInfinity(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt14::encode(INFINITY, out); }).isError());
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt14::encode(-INFINITY, out); }).isError());
}

void test_Dpt14_DecodeZero(void) {
    float value;
    
    TEST_ASSERT_TRUE(Dpt14::decode(std::to_array<uint8_t>({0x00, 0x00, 0x00, 0x00}), value).isOk());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, value);
}

void test_Dpt14_DecodePositiveValue(void) {
    float value;
    
    TEST_ASSERT_TRUE(Dpt14::decode(std::to_array<uint8_t>({0x3F, 0x80, 0x00, 0x00}), value).isOk());
    TEST_ASSERT_EQUAL_FLOAT(1.0f, value);
}

void test_Dpt14_DecodeNegativeValue(void) {
    float value;
    
    TEST_ASSERT_TRUE(Dpt14::decode(std::to_array<uint8_t>({0xBF, 0x80, 0x00, 0x00}), value).isOk());
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, value);
}

void test_Dpt14_DecodeInsufficientData(void) {
    float value;
    
    TEST_ASSERT_TRUE(Dpt14::decode(std::span<const uint8_t>{}, value).isError());
    TEST_ASSERT_TRUE(Dpt14::decode(std::to_array<uint8_t>({0x3F, 0x80, 0x00}), value).isError());
}

void test_Dpt14_RoundTrip(void) {
    float original = 123.456f;
    float decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt14::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt14::decode(data, decoded).isOk());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, original, decoded);
}

void test_Dpt14_RoundTripNegative(void) {
    float original = -273.15f;
    float decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt14::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt14::decode(data, decoded).isOk());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, original, decoded);
}

int main(void) {
    UNITY_BEGIN();
    
    // DPT 12 tests
    RUN_TEST(test_Dpt12_EncodeZero);
    RUN_TEST(test_Dpt12_EncodeSmallValue);
    RUN_TEST(test_Dpt12_EncodeLargeValue);
    RUN_TEST(test_Dpt12_EncodeMaxValue);
    RUN_TEST(test_Dpt12_DecodeZero);
    RUN_TEST(test_Dpt12_DecodeSmallValue);
    RUN_TEST(test_Dpt12_DecodeLargeValue);
    RUN_TEST(test_Dpt12_DecodeMaxValue);
    RUN_TEST(test_Dpt12_DecodeInsufficientData);
    RUN_TEST(test_Dpt12_RoundTrip);
    RUN_TEST(test_Dpt12_BigEndianByteOrder);
    
    // DPT 13 tests
    RUN_TEST(test_Dpt13_EncodeZero);
    RUN_TEST(test_Dpt13_EncodePositiveValue);
    RUN_TEST(test_Dpt13_EncodeNegativeValue);
    RUN_TEST(test_Dpt13_EncodeMaxPositive);
    RUN_TEST(test_Dpt13_EncodeMinNegative);
    RUN_TEST(test_Dpt13_DecodePositiveValue);
    RUN_TEST(test_Dpt13_DecodeNegativeValue);
    RUN_TEST(test_Dpt13_DecodeInsufficientData);
    RUN_TEST(test_Dpt13_RoundTrip);
    
    // DPT 14 tests
    RUN_TEST(test_Dpt14_EncodeZero);
    RUN_TEST(test_Dpt14_EncodePositiveValue);
    RUN_TEST(test_Dpt14_EncodeNegativeValue);
    RUN_TEST(test_Dpt14_EncodeFractionalValue);
    RUN_TEST(test_Dpt14_EncodeInvalidNaN);
    RUN_TEST(test_Dpt14_EncodeInvalidInfinity);
    RUN_TEST(test_Dpt14_DecodeZero);
    RUN_TEST(test_Dpt14_DecodePositiveValue);
    RUN_TEST(test_Dpt14_DecodeNegativeValue);
    RUN_TEST(test_Dpt14_DecodeInsufficientData);
    RUN_TEST(test_Dpt14_RoundTrip);
    RUN_TEST(test_Dpt14_RoundTripNegative);
    
    return UNITY_END();
}
