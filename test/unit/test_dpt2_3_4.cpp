// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_dpt2_3_4.cpp
 * @brief Unit tests for DPT 2 (1-bit controlled), DPT 3 (3-bit controlled), and DPT 4 (Character)
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
// DPT 2 - 1-bit Controlled
// ============================

void test_Dpt2_EncodeControlOff_ValueOff(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt2::encode(false, false, out); });
    
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
}

void test_Dpt2_EncodeControlOff_ValueOn(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt2::encode(false, true, out); });
    
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x01, data[0]);
}

void test_Dpt2_EncodeControlOn_ValueOff(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt2::encode(true, false, out); });
    
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x02, data[0]);
}

void test_Dpt2_EncodeControlOn_ValueOn(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt2::encode(true, true, out); });
    
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, data[0]);
}

void test_Dpt2_EncodeWithStruct(void) {
    Dpt2::Value val{true, false};
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt2::encode(val, out); });
    
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x02, data[0]);
}

void test_Dpt2_DecodeAllCombinations(void) {
    std::vector<uint8_t> data;
    Dpt2::Value val;
    
    // 0x00: control=0, value=0
    data = {0x00};
    TEST_ASSERT_TRUE(Dpt2::decode(data, val).isOk());
    TEST_ASSERT_FALSE(val.control);
    TEST_ASSERT_FALSE(val.value);
    
    // 0x01: control=0, value=1
    data = {0x01};
    TEST_ASSERT_TRUE(Dpt2::decode(data, val).isOk());
    TEST_ASSERT_FALSE(val.control);
    TEST_ASSERT_TRUE(val.value);
    
    // 0x02: control=1, value=0
    data = {0x02};
    TEST_ASSERT_TRUE(Dpt2::decode(data, val).isOk());
    TEST_ASSERT_TRUE(val.control);
    TEST_ASSERT_FALSE(val.value);
    
    // 0x03: control=1, value=1
    data = {0x03};
    TEST_ASSERT_TRUE(Dpt2::decode(data, val).isOk());
    TEST_ASSERT_TRUE(val.control);
    TEST_ASSERT_TRUE(val.value);
}

void test_Dpt2_DecodeEmptyData(void) {
    std::vector<uint8_t> data;
    Dpt2::Value val;
    
    TEST_ASSERT_TRUE(Dpt2::decode(data, val).isError());
}

void test_Dpt2_RoundTrip(void) {
    Dpt2::Value original{true, true};
    Dpt2::Value decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt2::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt2::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL(original.control, decoded.control);
    TEST_ASSERT_EQUAL(original.value, decoded.value);
}

// ============================
// DPT 3 - 3-bit Controlled
// ============================

void test_Dpt3_EncodeValidStepCodes(void) {
    // Step code 0 (break)
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt3::encode(false, 0, out); });
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    
    // Step code 1 (minimum increase)
    data = encodePayload([](std::span<uint8_t> out) { return Dpt3::encode(true, 1, out); });
    TEST_ASSERT_EQUAL_HEX8(0x09, data[0]);  // control=1, step=1 -> 0b1001
    
    // Step code 7 (maximum)
    data = encodePayload([](std::span<uint8_t> out) { return Dpt3::encode(false, 7, out); });
    TEST_ASSERT_EQUAL_HEX8(0x07, data[0]);
}

void test_Dpt3_EncodeInvalidStepCode(void) {
    // Step code > 7 should fail
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt3::encode(false, 8, out); }).isError());
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt3::encode(true, 15, out); }).isError());
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt3::encode(false, 255, out); }).isError());
}

void test_Dpt3_EncodeWithStruct(void) {
    Dpt3::Value val{true, 3};
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt3::encode(val, out); });
    TEST_ASSERT_EQUAL_HEX8(0x0B, data[0]);  // 0b1011
}

void test_Dpt3_DecodeVariousCombinations(void) {
    std::vector<uint8_t> data;
    Dpt3::Value val;
    
    // Decrease dimming, step 3
    data = {0x03};
    TEST_ASSERT_TRUE(Dpt3::decode(data, val).isOk());
    TEST_ASSERT_FALSE(val.control);
    TEST_ASSERT_EQUAL_UINT8(3, val.stepCode);
    
    // Increase dimming, step 5
    data = {0x0D};  // 0b1101
    TEST_ASSERT_TRUE(Dpt3::decode(data, val).isOk());
    TEST_ASSERT_TRUE(val.control);
    TEST_ASSERT_EQUAL_UINT8(5, val.stepCode);
}

void test_Dpt3_DecodeEmptyData(void) {
    std::vector<uint8_t> data;
    Dpt3::Value val;
    
    TEST_ASSERT_TRUE(Dpt3::decode(data, val).isError());
}

void test_Dpt3_RoundTrip(void) {
    Dpt3::Value original{false, 4};
    Dpt3::Value decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt3::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt3::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL(original.control, decoded.control);
    TEST_ASSERT_EQUAL_UINT8(original.stepCode, decoded.stepCode);
}

// ============================
// DPT 4 - Character
// ============================

void test_Dpt4_EncodeBasicASCII(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt4::encode('A', out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x41, data[0]);
    
    data = encodePayload([](std::span<uint8_t> out) { return Dpt4::encode('Z', out); });
    TEST_ASSERT_EQUAL_HEX8(0x5A, data[0]);
    
    data = encodePayload([](std::span<uint8_t> out) { return Dpt4::encode('0', out); });
    TEST_ASSERT_EQUAL_HEX8(0x30, data[0]);
}

void test_Dpt4_EncodeExtendedCharacters(void) {
    // Space
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt4::encode(' ', out); });
    TEST_ASSERT_EQUAL_HEX8(0x20, data[0]);
    
    // Newline
    data = encodePayload([](std::span<uint8_t> out) { return Dpt4::encode('\n', out); });
    TEST_ASSERT_EQUAL_HEX8(0x0A, data[0]);
    
    // Null
    data = encodePayload([](std::span<uint8_t> out) { return Dpt4::encode('\0', out); });
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
}

void test_Dpt4_DecodeBasicASCII(void) {
    char value;
    
    TEST_ASSERT_TRUE(Dpt4::decode(std::to_array<uint8_t>({0x41}), value).isOk());
    TEST_ASSERT_EQUAL_INT8('A', value);
    
    TEST_ASSERT_TRUE(Dpt4::decode(std::to_array<uint8_t>({0x7A}), value).isOk());
    TEST_ASSERT_EQUAL_INT8('z', value);
}

void test_Dpt4_DecodeEmptyData(void) {
    char value;
    TEST_ASSERT_TRUE(Dpt4::decode(std::span<const uint8_t>{}, value).isError());
}

void test_Dpt4_RoundTrip(void) {
    char original = 'X';
    char decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt4::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt4::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_INT8(original, decoded);
}

void test_Dpt4_ISO88591Characters(void) {
    char value;
    
    // Test extended ASCII/ISO-8859-1 (values > 127)
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt4::encode(static_cast<char>(0xA0), out); });  // Non-breaking space
    TEST_ASSERT_EQUAL_HEX8(0xA0, data[0]);
    
    TEST_ASSERT_TRUE(Dpt4::decode(std::to_array<uint8_t>({0xE4}), value).isOk());  // ä
    TEST_ASSERT_EQUAL_HEX8(0xE4, static_cast<uint8_t>(value));
}

int main(void) {
    UNITY_BEGIN();
    
    // DPT 2 tests
    RUN_TEST(test_Dpt2_EncodeControlOff_ValueOff);
    RUN_TEST(test_Dpt2_EncodeControlOff_ValueOn);
    RUN_TEST(test_Dpt2_EncodeControlOn_ValueOff);
    RUN_TEST(test_Dpt2_EncodeControlOn_ValueOn);
    RUN_TEST(test_Dpt2_EncodeWithStruct);
    RUN_TEST(test_Dpt2_DecodeAllCombinations);
    RUN_TEST(test_Dpt2_DecodeEmptyData);
    RUN_TEST(test_Dpt2_RoundTrip);
    
    // DPT 3 tests
    RUN_TEST(test_Dpt3_EncodeValidStepCodes);
    RUN_TEST(test_Dpt3_EncodeInvalidStepCode);
    RUN_TEST(test_Dpt3_EncodeWithStruct);
    RUN_TEST(test_Dpt3_DecodeVariousCombinations);
    RUN_TEST(test_Dpt3_DecodeEmptyData);
    RUN_TEST(test_Dpt3_RoundTrip);
    
    // DPT 4 tests
    RUN_TEST(test_Dpt4_EncodeBasicASCII);
    RUN_TEST(test_Dpt4_EncodeExtendedCharacters);
    RUN_TEST(test_Dpt4_DecodeBasicASCII);
    RUN_TEST(test_Dpt4_DecodeEmptyData);
    RUN_TEST(test_Dpt4_RoundTrip);
    RUN_TEST(test_Dpt4_ISO88591Characters);
    
    return UNITY_END();
}
