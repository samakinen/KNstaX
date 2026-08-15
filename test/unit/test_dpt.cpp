// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_dpt.cpp
 * @brief Unit tests for Data Point Types (DPT)
 * 
 * @spec KNX Specification 03/07/02 Datapoint Types v2.1
 * @spec KNX Interworking Standard v2.1 - System Specifications
 * @note Tests validate encoding/decoding per official KNX DPT specifications
 * @note Each DPT test should verify boundary conditions and spec-defined ranges
 */

#include "unity.h"
#include "knx/application/dpt.hpp"
#include "knx/constants.hpp"
#include <array>
#include <span>
#include <vector>
#include <cmath>

using namespace knx::application;

template <size_t Capacity = 32, typename EncoderFn>
static std::vector<uint8_t> encodePayload(EncoderFn&& encoder)
{
    std::array<uint8_t, Capacity> buffer{};
    auto result = encoder(std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(result.isOk());
    return std::vector<uint8_t>(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(result.value()));
}

void setUp(void) {
    // Set up code here
}

void tearDown(void) {
    // Clean up code here
}

// DPT 1 - Boolean
void test_dpt1_encode_true(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt1::encode(true, out); });
    TEST_ASSERT_EQUAL(1, data.size());
    TEST_ASSERT_EQUAL(0x01, data[0] & 0x01);
}

void test_dpt1_encode_false(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt1::encode(false, out); });
    TEST_ASSERT_EQUAL(1, data.size());
    TEST_ASSERT_EQUAL(0x00, data[0] & 0x01);
}

void test_dpt1_decode_true(void) {
    std::vector<uint8_t> data = {0x01};
    bool value = false;
    TEST_ASSERT_TRUE(Dpt1::decode(data, value).isOk());
    TEST_ASSERT_TRUE(value);
}

void test_dpt1_decode_false(void) {
    std::vector<uint8_t> data = {0x00};
    bool value = true;
    TEST_ASSERT_TRUE(Dpt1::decode(data, value).isOk());
    TEST_ASSERT_FALSE(value);
}

// DPT 5 - Unsigned integer
void test_dpt5_encode_0(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt5::encode(0, out); });
    TEST_ASSERT_EQUAL(1, data.size());
    TEST_ASSERT_EQUAL(0x00, data[0]);
}

void test_dpt5_encode_255(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt5::encode(255, out); });
    TEST_ASSERT_EQUAL(1, data.size());
    TEST_ASSERT_EQUAL(0xFF, data[0]);
}

void test_dpt5_encode_128(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt5::encode(128, out); });
    TEST_ASSERT_EQUAL(1, data.size());
    TEST_ASSERT_EQUAL(0x80, data[0]);
}

void test_dpt5_decode_0(void) {
    std::vector<uint8_t> data = {0x00};
    uint8_t value = 99;
    TEST_ASSERT_TRUE(Dpt5::decode(data, value).isOk());
    TEST_ASSERT_EQUAL(0, value);
}

void test_dpt5_decode_255(void) {
    std::vector<uint8_t> data = {0xFF};
    uint8_t value = 0;
    TEST_ASSERT_TRUE(Dpt5::decode(data, value).isOk());
    TEST_ASSERT_EQUAL(255, value);
}

void test_dpt5_roundtrip(void) {
    uint8_t original = 42;
    uint8_t decoded = 0;

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt5::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt5::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL(original, decoded);
}

// DPT 9 - Float
void test_dpt9_encode_zero(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt9::encode(0.0f, out); });
    TEST_ASSERT_EQUAL(2, data.size());
    TEST_ASSERT_EQUAL(0x00, data[0]);
    TEST_ASSERT_EQUAL(0x00, data[1]);
}

void test_dpt9_decode_zero(void) {
    std::vector<uint8_t> data = {0x00, 0x00};
    float value = 1.0f;
    TEST_ASSERT_TRUE(Dpt9::decode(data, value).isOk());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, value);
}

void test_dpt9_roundtrip_positive(void) {
    float original = 21.5f;
    float decoded = 0.0f;

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(original, out); });
    TEST_ASSERT_EQUAL(2, data.size());
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
    
    // Allow small floating point error
    TEST_ASSERT_FLOAT_WITHIN(0.1f, original, decoded);
}

void test_dpt9_roundtrip_negative(void) {
    float original = -15.3f;
    float decoded = 0.0f;

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
    
    // Allow small floating point error
    TEST_ASSERT_FLOAT_WITHIN(0.1f, original, decoded);
}

void test_dpt9_known_wire_vectors(void) {
    struct Dpt9Vector {
        float value;
        std::array<uint8_t, 2> raw;
    };

    const std::array<Dpt9Vector, 7> vectors{{
        {0.0f, {0x00, 0x00}},
        {0.01f, {0x00, 0x01}},
        {20.47f, {0x07, 0xFF}},
        {-0.01f, {0x87, 0xFF}},
        {-20.48f, {0x80, 0x00}},
        {knx::constants::dpt::DPT9_MIN_VALUE, {0xF8, 0x00}},
        {knx::constants::dpt::DPT9_MAX_VALUE, {0x7F, 0xFF}},
    }};

    for (const auto& vector : vectors) {
        const auto encoded = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(vector.value, out); });
        TEST_ASSERT_EQUAL_UINT32(vector.raw.size(), static_cast<uint32_t>(encoded.size()));
        TEST_ASSERT_EQUAL_UINT8(vector.raw[0], encoded[0]);
        TEST_ASSERT_EQUAL_UINT8(vector.raw[1], encoded[1]);

        float decoded = 0.0f;
        TEST_ASSERT_TRUE(Dpt9::decode(vector.raw, decoded).isOk());
        TEST_ASSERT_FLOAT_WITHIN(0.01f, vector.value, decoded);
    }
}

// DPT 9 - Boundary conditions per KNX Spec 03/07/02
// Format: SEEE EMMM MMMM MMMM (S=sign, E=4-bit exponent, M=signed 11-bit mantissa)
// Value = (0.01 * M) * 2^E
// Range: -671088.64 to 670760.96

void test_dpt9_minimum_value(void) {
    const float min_value = knx::constants::dpt::DPT9_MIN_VALUE;

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(min_value, out); });
    TEST_ASSERT_EQUAL(2, data.size());
    TEST_ASSERT_EQUAL_UINT8(0xF8, data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, data[1]);
    
    float decoded;
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
    
    // Allow 0.01% tolerance for floating point
    TEST_ASSERT_FLOAT_WITHIN(fabsf(min_value) * 0.0001f, min_value, decoded);
}

void test_dpt9_maximum_value(void) {
    const float max_value = knx::constants::dpt::DPT9_MAX_VALUE;

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(max_value, out); });
    TEST_ASSERT_EQUAL(2, data.size());
    TEST_ASSERT_EQUAL_UINT8(0x7F, data[0]);
    TEST_ASSERT_EQUAL_UINT8(0xFF, data[1]);
    
    float decoded;
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
    
    // Allow 0.01% tolerance for floating point
    TEST_ASSERT_FLOAT_WITHIN(max_value * 0.0001f, max_value, decoded);
}

void test_dpt9_resolution_at_different_ranges(void) {
    // Test resolution at different exponent values
    // Resolution = 0.01 * 2^E
    // At E=0: resolution is 0.01
    float value1 = 1.00f;
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(value1, out); });
    float decoded1;
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded1).isOk());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, value1, decoded1);
    
    // At E=5: resolution is 0.32
    float value2 = 100.0f;
    data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(value2, out); });
    float decoded2;
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded2).isOk());
    TEST_ASSERT_FLOAT_WITHIN(0.5f, value2, decoded2);
    
    // At E=10: resolution is 10.24
    float value3 = 10000.0f;
    data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(value3, out); });
    float decoded3;
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded3).isOk());
    TEST_ASSERT_FLOAT_WITHIN(20.0f, value3, decoded3);
}

void test_dpt9_small_positive_values(void) {
    // Test small positive values near zero
    float values[] = {0.01f, 0.1f, 1.0f, 10.0f};
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(values[i], out); });
        float decoded;
        TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
        TEST_ASSERT_FLOAT_WITHIN(values[i] * 0.02f, values[i], decoded);
    }
}

void test_dpt9_small_negative_values(void) {
    // Test small negative values near zero
    float values[] = {-0.01f, -0.1f, -1.0f, -10.0f};
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(values[i], out); });
        float decoded;
        TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
        TEST_ASSERT_FLOAT_WITHIN(fabsf(values[i]) * 0.02f, values[i], decoded);
    }
}

void test_dpt9_temperature_range(void) {
    // Common temperature sensor range: -50°C to +50°C
    float temps[] = {-50.0f, -20.0f, 0.0f, 20.0f, 50.0f};
    for (size_t i = 0; i < sizeof(temps)/sizeof(temps[0]); i++) {
        auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt9::encode(temps[i], out); });
        float decoded;
        TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
        // Temperature should be within 0.1°C
        TEST_ASSERT_FLOAT_WITHIN(0.1f, temps[i], decoded);
    }
}

void test_dpt9_invalid_data_size(void) {
    // Test decoding with invalid data sizes
    float value;
    
    TEST_ASSERT_TRUE(Dpt9::decode(std::span<const uint8_t>{}, value).isError());  // Empty
    TEST_ASSERT_TRUE(Dpt9::decode(std::array<uint8_t, 1>{0x00}, value).isError());  // Too short
    // Note: Too long should either fail or use first 2 bytes
}

void test_dpt9_mantissa_boundaries(void) {
    // Test mantissa boundaries
    // Format: SEEE EMMM MMMM MMMM (S=sign, E=exponent, M=signed 11-bit mantissa)
    std::vector<uint8_t> data;
    
    // Max positive mantissa (+2047) with exponent 0
    // 0000 0111 1111 1111 = 0x07FF
    data = {0x07, 0xFF};
    float decoded;
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 20.47f, decoded);
    
    // Min negative mantissa (-2048) with exponent 0
    // 1000 0000 0000 0000 = 0x8000
    data = {0x80, 0x00};
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -20.48f, decoded);

    // Highest negative mantissa before wrap (-1) with exponent 0
    // 1000 0111 1111 1111 = 0x87FF
    data = {0x87, 0xFF};
    TEST_ASSERT_TRUE(Dpt9::decode(data, decoded).isOk());
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -0.01f, decoded);
}

void test_dpt_lookup_prefers_exact_subtype_and_falls_back_to_main_type(void) {
    const auto* exact = lookupDptInfo(dptids::Switch);
    TEST_ASSERT_NOT_NULL(exact);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(DptMainType::Boolean), exact->id.mainNumber());
    TEST_ASSERT_EQUAL_UINT16(1u, exact->id.sub);

    const auto* fallback = lookupDptInfo(makeDptId(1, 99));
    TEST_ASSERT_NOT_NULL(fallback);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(DptMainType::Boolean), fallback->id.mainNumber());
    TEST_ASSERT_EQUAL_UINT16(0u, fallback->id.sub);
}

void test_dpt_traits_switch_encode_decode_and_dynamic_dispatch(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return DptTraits<dpttags::Switch>::encode(true, out); });
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(data.size()));

    auto decodedTyped = DptTraits<dpttags::Switch>::decode(data);
    TEST_ASSERT_TRUE(decodedTyped.isOk());
    TEST_ASSERT_TRUE(decodedTyped.value());

    data = encodePayload([](std::span<uint8_t> out) { return encodeDptValue(dptids::Switch, DptValue(true), out); });
    auto decodedDynamic = decodeDptValue(dptids::Switch, data);
    TEST_ASSERT_TRUE(decodedDynamic.isOk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DptValue::Type::Boolean), static_cast<int>(decodedDynamic.value().type()));
    TEST_ASSERT_TRUE(decodedDynamic.value().asBool());
}

void test_dpt_registry_exposes_full_runtime_catalog(void) {
    const DptInfo* entries = dptRegistryEntries();
    TEST_ASSERT_NOT_NULL(entries);

    uint32_t computed = 0u;
    for (size_t i = 0; i < dptRegistrySize(); ++i) {
        if (entries[i].runtimeSupported) {
            ++computed;
        }
    }

    // Require at least 25 datapoint types to be registered at runtime.
    TEST_ASSERT_TRUE(computed >= 25u);
    // Require that all DPTs supported at runtime are present in the registry
    TEST_ASSERT_TRUE(dptRegistrySize() == computed);
    
    TEST_ASSERT_TRUE(supportsDpt(dptids::TimeOfDay));
    TEST_ASSERT_TRUE(supportsDpt(dptids::String));
    TEST_ASSERT_TRUE(supportsDpt(dptids::RgbColor));
    TEST_ASSERT_TRUE(supportsDpt(dptids::Float4Byte));
}

void test_dynamic_dpt10_roundtrip(void) {
    Dpt10Value expected{};
    expected.dayOfWeek = 2;
    expected.hour = 14;
    expected.minute = 37;
    expected.second = 42;

    auto data = encodePayload([&](std::span<uint8_t> out) { return encodeDptValue(dptids::TimeOfDay, DptValue(expected), out); });

    auto decoded = decodeDptValue(dptids::TimeOfDay, data);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DptValue::Type::TimeOfDay), static_cast<int>(decoded.value().type()));

    const auto& actual = decoded.value().asTimeOfDay();
    TEST_ASSERT_EQUAL_UINT8(expected.dayOfWeek, actual.dayOfWeek);
    TEST_ASSERT_EQUAL_UINT8(expected.hour, actual.hour);
    TEST_ASSERT_EQUAL_UINT8(expected.minute, actual.minute);
    TEST_ASSERT_EQUAL_UINT8(expected.second, actual.second);
}

void test_dynamic_dpt16_roundtrip(void) {
    const std::string expected = "KNX test";
    auto data = encodePayload([&](std::span<uint8_t> out) { return encodeDptValue(dptids::String, DptValue(expected), out); });

    auto decoded = decodeDptValue(dptids::String, data);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DptValue::Type::String), static_cast<int>(decoded.value().type()));
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), decoded.value().asString().c_str());
}

void test_dynamic_dpt14_roundtrip_preserves_float4byte_type(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return encodeDptValue(dptids::Float4Byte, DptValue(42.5f, DptValue::Type::Float4Byte), out); });

    auto decoded = decodeDptValue(dptids::Float4Byte, data);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DptValue::Type::Float4Byte), static_cast<int>(decoded.value().type()));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 42.5f, decoded.value().asFloat());
}

void test_dynamic_dpt232_roundtrip(void) {
    Dpt232Value expected{};
    expected.red = 10;
    expected.green = 20;
    expected.blue = 30;

    auto data = encodePayload([&](std::span<uint8_t> out) { return encodeDptValue(dptids::RgbColor, DptValue(expected), out); });

    auto decoded = decodeDptValue(dptids::RgbColor, data);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(DptValue::Type::RgbColor), static_cast<int>(decoded.value().type()));

    const auto& actual = decoded.value().asRgbColor();
    TEST_ASSERT_EQUAL_UINT8(expected.red, actual.red);
    TEST_ASSERT_EQUAL_UINT8(expected.green, actual.green);
    TEST_ASSERT_EQUAL_UINT8(expected.blue, actual.blue);
}

// Test runner
int run_all_dpt_tests(void) {
    UNITY_BEGIN();
    
    // DPT 1 tests
    RUN_TEST(test_dpt1_encode_true);
    RUN_TEST(test_dpt1_encode_false);
    RUN_TEST(test_dpt1_decode_true);
    RUN_TEST(test_dpt1_decode_false);
    
    // DPT 5 tests
    RUN_TEST(test_dpt5_encode_0);
    RUN_TEST(test_dpt5_encode_255);
    RUN_TEST(test_dpt5_encode_128);
    RUN_TEST(test_dpt5_decode_0);
    RUN_TEST(test_dpt5_decode_255);
    RUN_TEST(test_dpt5_roundtrip);
    
    // DPT 9 tests
    RUN_TEST(test_dpt9_encode_zero);
    RUN_TEST(test_dpt9_decode_zero);
    RUN_TEST(test_dpt9_roundtrip_positive);
    RUN_TEST(test_dpt9_known_wire_vectors);
    RUN_TEST(test_dpt9_roundtrip_negative);
    
    // DPT 9 boundary tests (KNX Spec 03/07/02)
    RUN_TEST(test_dpt9_minimum_value);
    RUN_TEST(test_dpt9_maximum_value);
    RUN_TEST(test_dpt9_resolution_at_different_ranges);
    RUN_TEST(test_dpt9_small_positive_values);
    RUN_TEST(test_dpt9_small_negative_values);
    RUN_TEST(test_dpt9_temperature_range);
    RUN_TEST(test_dpt9_invalid_data_size);
    RUN_TEST(test_dpt9_mantissa_boundaries);
    RUN_TEST(test_dpt_lookup_prefers_exact_subtype_and_falls_back_to_main_type);
    RUN_TEST(test_dpt_traits_switch_encode_decode_and_dynamic_dispatch);
    RUN_TEST(test_dpt_registry_exposes_full_runtime_catalog);
    RUN_TEST(test_dynamic_dpt10_roundtrip);
    RUN_TEST(test_dynamic_dpt16_roundtrip);
    RUN_TEST(test_dynamic_dpt14_roundtrip_preserves_float4byte_type);
    RUN_TEST(test_dynamic_dpt232_roundtrip);
    
    return UNITY_END();
}

int main() {
    return run_all_dpt_tests();
}
