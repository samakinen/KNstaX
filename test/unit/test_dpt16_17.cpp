// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_dpt16_17.cpp
 * @brief Unit tests for DPT 16 (String) and DPT 17 (Scene Number)
 */

#include "../../include/knx/application/dpt.hpp"
#include "dpt_test_helpers.hpp"
#include "../unity_mock/unity.h"
#include <array>
#include <span>
#include <vector>
#include <string>

using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

// ============================
// DPT 16 - String (ASCII)
// ============================

void test_Dpt16_EncodeEmptyString(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt16::encode("", out); });
    TEST_ASSERT_EQUAL_UINT(14, data.size());
    
    // All bytes should be NULL
    for (size_t i = 0; i < 14; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x00, data[i]);
    }
}

void test_Dpt16_EncodeShortString(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt16::encode("Hello", out); });
    TEST_ASSERT_EQUAL_UINT(14, data.size());
    
    TEST_ASSERT_EQUAL_HEX8('H', data[0]);
    TEST_ASSERT_EQUAL_HEX8('e', data[1]);
    TEST_ASSERT_EQUAL_HEX8('l', data[2]);
    TEST_ASSERT_EQUAL_HEX8('l', data[3]);
    TEST_ASSERT_EQUAL_HEX8('o', data[4]);
    
    // Remaining bytes should be NULL
    for (size_t i = 5; i < 14; ++i) {
        TEST_ASSERT_EQUAL_HEX8(0x00, data[i]);
    }
}

void test_Dpt16_EncodeMaxLengthString(void) {
    std::string maxString = "1234567890ABC";  // 13 characters
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt16::encode(maxString, out); });
    TEST_ASSERT_EQUAL_UINT(14, data.size());
    
    for (size_t i = 0; i < 13; ++i) {
        TEST_ASSERT_EQUAL_HEX8(maxString[i], data[i]);
    }
    TEST_ASSERT_EQUAL_HEX8(0x00, data[13]);  // NULL terminator
}

void test_Dpt16_EncodeTooLongString(void) {
    std::string tooLong = "12345678901234";  // 14 characters (too long)
    
    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt16::encode(tooLong, out); }).isError());
}

void test_Dpt16_EncodeStringWithSpaces(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt16::encode("Room 101", out); });
    TEST_ASSERT_EQUAL_UINT(14, data.size());
    
    TEST_ASSERT_EQUAL_HEX8('R', data[0]);
    TEST_ASSERT_EQUAL_HEX8('o', data[1]);
    TEST_ASSERT_EQUAL_HEX8('o', data[2]);
    TEST_ASSERT_EQUAL_HEX8('m', data[3]);
    TEST_ASSERT_EQUAL_HEX8(' ', data[4]);
    TEST_ASSERT_EQUAL_HEX8('1', data[5]);
    TEST_ASSERT_EQUAL_HEX8('0', data[6]);
    TEST_ASSERT_EQUAL_HEX8('1', data[7]);
}

void test_Dpt16_DecodeEmptyString(void) {
    std::string value;
    std::vector<uint8_t> data(14, 0x00);
    
    TEST_ASSERT_TRUE(Dpt16::decode(data, value).isOk());
    TEST_ASSERT_EQUAL_STRING("", value.c_str());
}

void test_Dpt16_DecodeShortString(void) {
    std::string value;
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o', 0, 0, 0, 0, 0, 0, 0, 0, 0};
    
    TEST_ASSERT_TRUE(Dpt16::decode(data, value).isOk());
    TEST_ASSERT_EQUAL_STRING("Hello", value.c_str());
}

void test_Dpt16_DecodeMaxLengthString(void) {
    std::string value;
    std::vector<uint8_t> data = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 'A', 'B', 'C', 0};
    
    TEST_ASSERT_TRUE(Dpt16::decode(data, value).isOk());
    TEST_ASSERT_EQUAL_STRING("1234567890ABC", value.c_str());
}

void test_Dpt16_DecodeStringNoPadding(void) {
    std::string value;
    // String fills all 14 bytes (no explicit NULL)
    std::vector<uint8_t> data = {'F', 'u', 'l', 'l', 'S', 't', 'r', 'i', 'n', 'g', '1', '2', '3', '4'};
    
    TEST_ASSERT_TRUE(Dpt16::decode(data, value).isOk());
    // Should decode all 14 characters since no NULL found
    TEST_ASSERT_EQUAL_UINT(14, value.length());
}

void test_Dpt16_DecodeInsufficientData(void) {
    std::string value;
    
    TEST_ASSERT_TRUE(Dpt16::decode(std::span<const uint8_t>{}, value).isError());
    TEST_ASSERT_TRUE(Dpt16::decode(std::to_array<uint8_t>({static_cast<uint8_t>('H'), static_cast<uint8_t>('i')}), value).isError());
    TEST_ASSERT_TRUE(Dpt16::decode(std::vector<uint8_t>(13, 'A'), value).isError());
}

void test_Dpt16_RoundTrip(void) {
    std::string original = "KNX System";
    std::string decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt16::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt16::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_STRING(original.c_str(), decoded.c_str());
}

// ============================
// DPT 17 - Scene Number
// ============================

void test_Dpt17_EncodeValidSceneZero(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt17::encode(0, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);  // Scene 0, valid
}

void test_Dpt17_EncodeValidSceneMax(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt17::encode(63, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x3F, data[0]);  // Scene 63, valid
}

void test_Dpt17_EncodeValidSceneMid(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt17::encode(32, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x20, data[0]);  // Scene 32, valid
}

void test_Dpt17_EncodeInvalidSceneNumber(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt17::encode(64, out); }).isError());
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt17::encode(100, out); }).isError());
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt17::encode(255, out); }).isError());
}

void test_Dpt17_EncodeStructValid(void) {
    Dpt17::Value scene{42, true};  // Scene 42, valid
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt17::encode(scene, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x2A, data[0]);  // 42 = 0x2A, bit 7 = 0 (valid)
}

void test_Dpt17_EncodeStructInvalid(void) {
    Dpt17::Value scene{10, false};  // Scene 10, invalid
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt17::encode(scene, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x8A, data[0]);  // 10 = 0x0A, bit 7 = 1 (invalid)
}

void test_Dpt17_EncodeStructInvalidSceneNumber(void) {
    Dpt17::Value scene{64, true};  // Scene number too high
    
    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt17::encode(scene, out); }).isError());
}

void test_Dpt17_DecodeValidScene(void) {
    Dpt17::Value scene;
    
    TEST_ASSERT_TRUE(Dpt17::decode(std::to_array<uint8_t>({0x15}), scene).isOk());  // Scene 21, valid
    TEST_ASSERT_EQUAL_UINT8(21, scene.sceneNumber);
    TEST_ASSERT_TRUE(scene.valid);
}

void test_Dpt17_DecodeInvalidScene(void) {
    Dpt17::Value scene;
    
    TEST_ASSERT_TRUE(Dpt17::decode(std::to_array<uint8_t>({0x95}), scene).isOk());  // Scene 21, invalid (bit 7 set)
    TEST_ASSERT_EQUAL_UINT8(21, scene.sceneNumber);
    TEST_ASSERT_FALSE(scene.valid);
}

void test_Dpt17_DecodeSceneZero(void) {
    Dpt17::Value scene;
    
    TEST_ASSERT_TRUE(Dpt17::decode(std::to_array<uint8_t>({0x00}), scene).isOk());
    TEST_ASSERT_EQUAL_UINT8(0, scene.sceneNumber);
    TEST_ASSERT_TRUE(scene.valid);
}

void test_Dpt17_DecodeSceneMax(void) {
    Dpt17::Value scene;
    
    TEST_ASSERT_TRUE(Dpt17::decode(std::to_array<uint8_t>({0x3F}), scene).isOk());
    TEST_ASSERT_EQUAL_UINT8(63, scene.sceneNumber);
    TEST_ASSERT_TRUE(scene.valid);
}

void test_Dpt17_DecodeSceneMaxInvalid(void) {
    Dpt17::Value scene;
    
    TEST_ASSERT_TRUE(Dpt17::decode(std::to_array<uint8_t>({0xBF}), scene).isOk());  // Scene 63, invalid
    TEST_ASSERT_EQUAL_UINT8(63, scene.sceneNumber);
    TEST_ASSERT_FALSE(scene.valid);
}

void test_Dpt17_DecodeEmptyData(void) {
    Dpt17::Value scene;
    
    TEST_ASSERT_TRUE(Dpt17::decode(std::span<const uint8_t>{}, scene).isError());
}

void test_Dpt17_RoundTripValid(void) {
    Dpt17::Value original{50, true};
    Dpt17::Value decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt17::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt17::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT8(original.sceneNumber, decoded.sceneNumber);
    TEST_ASSERT_EQUAL(original.valid, decoded.valid);
}

void test_Dpt17_RoundTripInvalid(void) {
    Dpt17::Value original{25, false};
    Dpt17::Value decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt17::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt17::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT8(original.sceneNumber, decoded.sceneNumber);
    TEST_ASSERT_EQUAL(original.valid, decoded.valid);
}

int main(void) {
    UNITY_BEGIN();
    
    // DPT 16 tests
    RUN_TEST(test_Dpt16_EncodeEmptyString);
    RUN_TEST(test_Dpt16_EncodeShortString);
    RUN_TEST(test_Dpt16_EncodeMaxLengthString);
    RUN_TEST(test_Dpt16_EncodeTooLongString);
    RUN_TEST(test_Dpt16_EncodeStringWithSpaces);
    RUN_TEST(test_Dpt16_DecodeEmptyString);
    RUN_TEST(test_Dpt16_DecodeShortString);
    RUN_TEST(test_Dpt16_DecodeMaxLengthString);
    RUN_TEST(test_Dpt16_DecodeStringNoPadding);
    RUN_TEST(test_Dpt16_DecodeInsufficientData);
    RUN_TEST(test_Dpt16_RoundTrip);
    
    // DPT 17 tests
    RUN_TEST(test_Dpt17_EncodeValidSceneZero);
    RUN_TEST(test_Dpt17_EncodeValidSceneMax);
    RUN_TEST(test_Dpt17_EncodeValidSceneMid);
    RUN_TEST(test_Dpt17_EncodeInvalidSceneNumber);
    RUN_TEST(test_Dpt17_EncodeStructValid);
    RUN_TEST(test_Dpt17_EncodeStructInvalid);
    RUN_TEST(test_Dpt17_EncodeStructInvalidSceneNumber);
    RUN_TEST(test_Dpt17_DecodeValidScene);
    RUN_TEST(test_Dpt17_DecodeInvalidScene);
    RUN_TEST(test_Dpt17_DecodeSceneZero);
    RUN_TEST(test_Dpt17_DecodeSceneMax);
    RUN_TEST(test_Dpt17_DecodeSceneMaxInvalid);
    RUN_TEST(test_Dpt17_DecodeEmptyData);
    RUN_TEST(test_Dpt17_RoundTripValid);
    RUN_TEST(test_Dpt17_RoundTripInvalid);
    
    return UNITY_END();
}
