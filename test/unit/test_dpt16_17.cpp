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
    // 03_07_02 §3.17: the length is fixed to 14 octets; only shorter strings
    // are NUL-padded, so 14 characters fill the field with no terminator.
    std::string maxString = "1234567890ABCD";  // 14 characters

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt16::encode(maxString, out); });
    TEST_ASSERT_EQUAL_UINT(14, data.size());

    for (size_t i = 0; i < 14; ++i) {
        TEST_ASSERT_EQUAL_HEX8(maxString[i], data[i]);
    }
}

void test_Dpt16_EncodeTooLongString(void) {
    std::string tooLong = "123456789012345";  // 15 characters (too long)

    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt16::encode(tooLong, out); }).isError());
}

void test_Dpt16_GoldenBytesExample8(void) {
    // §3.17 EXAMPLE 8: 'KNX is OK' = 4B 4E 58 20 69 73 20 4F 4B 00 00 00 00 00
    const std::array<uint8_t, 14> expected{0x4B, 0x4E, 0x58, 0x20, 0x69, 0x73, 0x20,
                                           0x4F, 0x4B, 0x00, 0x00, 0x00, 0x00, 0x00};
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt16::encode("KNX is OK", out); });
    TEST_ASSERT_EQUAL_UINT(14, data.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), data.data(), 14);

    std::string decoded;
    TEST_ASSERT_TRUE(Dpt16::decode(expected, decoded).isOk());
    TEST_ASSERT_EQUAL_STRING("KNX is OK", decoded.c_str());
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
// DPT 17.001 - Scene Number, r2U6 (03_07_02 §3.18)
// ============================

void test_Dpt17_EncodeValidSceneZero(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt17::encode(0, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
}

void test_Dpt17_EncodeValidSceneMax(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt17::encode(63, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x3F, data[0]);
}

void test_Dpt17_EncodeValidSceneMid(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt17::encode(32, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x20, data[0]);
}

void test_Dpt17_EncodeInvalidSceneNumber(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt17::encode(64, out); }).isError());
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt17::encode(100, out); }).isError());
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt17::encode(255, out); }).isError());
}

void test_Dpt17_EncodeStruct(void) {
    Dpt17::Value scene{42};

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt17::encode(scene, out); });
    TEST_ASSERT_EQUAL_UINT(1, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x2A, data[0]);  // reserved bits 0
}

void test_Dpt17_EncodeStructInvalidSceneNumber(void) {
    Dpt17::Value scene{64};

    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt17::encode(scene, out); }).isError());
}

void test_Dpt17_DecodeScene(void) {
    Dpt17::Value scene;

    TEST_ASSERT_TRUE(Dpt17::decode(std::to_array<uint8_t>({0x15}), scene).isOk());
    TEST_ASSERT_EQUAL_UINT8(21, scene.sceneNumber);
}

void test_Dpt17_DecodeIgnoresReservedBits(void) {
    // The two high bits are reserved (r2), not a validity flag.
    Dpt17::Value scene;

    TEST_ASSERT_TRUE(Dpt17::decode(std::to_array<uint8_t>({0x95}), scene).isOk());
    TEST_ASSERT_EQUAL_UINT8(21, scene.sceneNumber);
    TEST_ASSERT_TRUE(Dpt17::decode(std::to_array<uint8_t>({0xFF}), scene).isOk());
    TEST_ASSERT_EQUAL_UINT8(63, scene.sceneNumber);
}

void test_Dpt17_DecodeSceneZero(void) {
    Dpt17::Value scene;

    TEST_ASSERT_TRUE(Dpt17::decode(std::to_array<uint8_t>({0x00}), scene).isOk());
    TEST_ASSERT_EQUAL_UINT8(0, scene.sceneNumber);
}

void test_Dpt17_DecodeEmptyData(void) {
    Dpt17::Value scene;

    TEST_ASSERT_TRUE(Dpt17::decode(std::span<const uint8_t>{}, scene).isError());
}

void test_Dpt17_RoundTrip(void) {
    Dpt17::Value original{50};
    Dpt17::Value decoded;

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt17::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt17::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT8(original.sceneNumber, decoded.sceneNumber);
}

int main(void) {
    UNITY_BEGIN();

    // DPT 16 tests
    RUN_TEST(test_Dpt16_EncodeEmptyString);
    RUN_TEST(test_Dpt16_EncodeShortString);
    RUN_TEST(test_Dpt16_EncodeMaxLengthString);
    RUN_TEST(test_Dpt16_EncodeTooLongString);
    RUN_TEST(test_Dpt16_GoldenBytesExample8);
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
    RUN_TEST(test_Dpt17_EncodeStruct);
    RUN_TEST(test_Dpt17_EncodeStructInvalidSceneNumber);
    RUN_TEST(test_Dpt17_DecodeScene);
    RUN_TEST(test_Dpt17_DecodeIgnoresReservedBits);
    RUN_TEST(test_Dpt17_DecodeSceneZero);
    RUN_TEST(test_Dpt17_DecodeEmptyData);
    RUN_TEST(test_Dpt17_RoundTrip);

    return UNITY_END();
}
