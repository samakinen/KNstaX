// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_manchester_codec.cpp
 * @brief Unit tests for Manchester encoding/decoding
 */

#include "knx/physical/manchester_codec.hpp"
#include "knx/util/result.hpp"
#include "unity.h"
#include <cstring>
#include <span>

using namespace knx::physical;

void setUp(void) {
    // Setup before each test
}

void tearDown(void) {
    // Cleanup after each test
}

// ============================================================================
// Timing Constants Tests
// ============================================================================

void test_timing_constants() {
    TEST_ASSERT_EQUAL(9600, ManchesterCodec::BAUD_RATE);
    TEST_ASSERT_EQUAL(19200, ManchesterCodec::BIT_RATE);
    TEST_ASSERT_EQUAL(52, ManchesterCodec::BIT_CELL_US);
    TEST_ASSERT_EQUAL(26, ManchesterCodec::HALF_BIT_US);
    TEST_ASSERT_EQUAL(52083, ManchesterCodec::BIT_CELL_NS);
    TEST_ASSERT_EQUAL(26042, ManchesterCodec::HALF_BIT_NS);
}

void test_timing_accuracy() {
    // Verify timing accuracy ±2% per KNX spec
    uint32_t expectedBitCellUs = 1000000 / ManchesterCodec::BIT_RATE;
    TEST_ASSERT_EQUAL(52, expectedBitCellUs);  // 52.083 rounded
    
    // Check tolerance
    uint32_t minUs = expectedBitCellUs * 98 / 100;
    uint32_t maxUs = expectedBitCellUs * 102 / 100;
    
    TEST_ASSERT_GREATER_OR_EQUAL(minUs, ManchesterCodec::BIT_CELL_US);
    TEST_ASSERT_LESS_OR_EQUAL(maxUs, ManchesterCodec::BIT_CELL_US);
}

// ============================================================================
// Parity Calculation Tests
// ============================================================================

void test_parity_all_zeros() {
    // 0x00 has 0 set bits, even parity = true
    TEST_ASSERT_TRUE(ManchesterCodec::calculateParity(0x00));
}

void test_parity_all_ones() {
    // 0xFF has 8 set bits (even), parity = true
    TEST_ASSERT_TRUE(ManchesterCodec::calculateParity(0xFF));
}

void test_parity_one_bit() {
    // 0x01 has 1 set bit (odd), parity = false
    TEST_ASSERT_FALSE(ManchesterCodec::calculateParity(0x01));
}

void test_parity_two_bits() {
    // 0x03 has 2 set bits (even), parity = true
    TEST_ASSERT_TRUE(ManchesterCodec::calculateParity(0x03));
}

void test_parity_alternating() {
    // 0xAA = 10101010 has 4 set bits (even), parity = true
    TEST_ASSERT_TRUE(ManchesterCodec::calculateParity(0xAA));
    
    // 0x55 = 01010101 has 4 set bits (even), parity = true
    TEST_ASSERT_TRUE(ManchesterCodec::calculateParity(0x55));
}

// ============================================================================
// Byte Encoding Tests
// ============================================================================

void test_encode_byte_all_zeros() {
    uint8_t output[16];
    size_t len = ManchesterCodec::encodeByte(0x00, std::span<uint8_t>(output));
    
    TEST_ASSERT_EQUAL(16, len);  // 8 bits * 2 half-bits
    
    // With differential Manchester, 0x00 should have transitions at start
    // Verify that mid-bit transitions exist (Manchester rule)
    for (size_t i = 0; i < 16; i += 2) {
        TEST_ASSERT_NOT_EQUAL(output[i], output[i + 1]);  // Mid-bit transition
    }
}

void test_encode_byte_all_ones() {
    uint8_t output[16];
    size_t len = ManchesterCodec::encodeByte(0xFF, std::span<uint8_t>(output));
    
    TEST_ASSERT_EQUAL(16, len);
    
    // Verify mid-bit transitions
    for (size_t i = 0; i < 16; i += 2) {
        TEST_ASSERT_NOT_EQUAL(output[i], output[i + 1]);
    }
}

void test_encode_byte_alternating_10() {
    uint8_t output[16];
    size_t len = ManchesterCodec::encodeByte(0xAA, std::span<uint8_t>(output));  // 10101010
    
    TEST_ASSERT_EQUAL(16, len);
    
    // Verify structure
    for (size_t i = 0; i < 16; i += 2) {
        TEST_ASSERT_NOT_EQUAL(output[i], output[i + 1]);
    }
}

void test_encode_byte_alternating_01() {
    uint8_t output[16];
    size_t len = ManchesterCodec::encodeByte(0x55, std::span<uint8_t>(output));  // 01010101
    
    TEST_ASSERT_EQUAL(16, len);
    
    for (size_t i = 0; i < 16; i += 2) {
        TEST_ASSERT_NOT_EQUAL(output[i], output[i + 1]);
    }
}

void test_encode_byte_known_pattern() {
    uint8_t output[16];
    size_t len = ManchesterCodec::encodeByte(0x42, std::span<uint8_t>(output));  // 01000010
    
    TEST_ASSERT_EQUAL(16, len);
    
    // Each bit cell must have a transition
    for (size_t i = 0; i < 16; i += 2) {
        TEST_ASSERT_NOT_EQUAL(output[i], output[i + 1]);
    }
}

// ============================================================================
// Byte Decoding Tests
// ============================================================================

void test_decode_byte_roundtrip_zeros() {
    uint8_t encoded[16];
    ManchesterCodec::encodeByte(0x00, std::span<uint8_t>(encoded));
    
    uint8_t decoded;
    auto result = ManchesterCodec::decodeByte(std::span<const uint8_t>(encoded, 16), decoded);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(0x00, decoded);
}

void test_decode_byte_roundtrip_ones() {
    uint8_t encoded[16];
    ManchesterCodec::encodeByte(0xFF, std::span<uint8_t>(encoded));
    
    uint8_t decoded;
    auto result = ManchesterCodec::decodeByte(std::span<const uint8_t>(encoded, 16), decoded);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(0xFF, decoded);
}

void test_decode_byte_roundtrip_pattern() {
    uint8_t testValues[] = {0x00, 0xFF, 0xAA, 0x55, 0x42, 0xBD, 0x01, 0x80};
    
    for (size_t i = 0; i < sizeof(testValues); ++i) {
        uint8_t encoded[16];
        ManchesterCodec::encodeByte(testValues[i], std::span<uint8_t>(encoded));
        
        uint8_t decoded;
        auto result = ManchesterCodec::decodeByte(std::span<const uint8_t>(encoded, 16), decoded);

        TEST_ASSERT_TRUE(result.isOk());
        TEST_ASSERT_EQUAL(testValues[i], decoded);
    }
}

void test_decode_byte_insufficient_data() {
    uint8_t encoded[10] = {0};
    uint8_t decoded;
    
    auto result = ManchesterCodec::decodeByte(std::span<const uint8_t>(encoded, 10), decoded);
    TEST_ASSERT_TRUE(result.isError());  // Not enough data
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::InvalidFrameSize, result.error());
}

void test_decode_byte_null_pointer() {
    uint8_t decoded;
    auto result = ManchesterCodec::decodeByte(std::span<const uint8_t>(static_cast<const uint8_t*>(nullptr), 16), decoded);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::InvalidParameter, result.error());
}

// ============================================================================
// Multi-Byte Encoding/Decoding Tests
// ============================================================================

void test_encode_bytes_multiple() {
    uint8_t data[] = {0x12, 0x34, 0x56};
    uint8_t output[48];  // 3 bytes * 16 half-bits
    
    size_t len = ManchesterCodec::encodeBytes(std::span<const uint8_t>(data, 3), std::span<uint8_t>(output));
    TEST_ASSERT_EQUAL(48, len);
}

void test_decode_bytes_roundtrip() {
    uint8_t data[] = {0x12, 0x34, 0x56, 0x78, 0x9A};
    uint8_t encoded[80];  // 5 * 16
    
    size_t encLen = ManchesterCodec::encodeBytes(std::span<const uint8_t>(data, 5), std::span<uint8_t>(encoded));
    TEST_ASSERT_EQUAL(80, encLen);
    
    uint8_t decoded[5];
    auto result = ManchesterCodec::decodeBytes(std::span<const uint8_t>(encoded, 80), std::span<uint8_t>(decoded));

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(5, result.value());
    TEST_ASSERT_EQUAL_MEMORY(data, decoded, 5);
}

// ============================================================================
// Frame Encoding Tests
// ============================================================================

void test_encode_frame_structure() {
    uint8_t output[24];  // 11 bits * 2 half-bits + margin
    
    size_t len = ManchesterCodec::encodeFrame(0x42, std::span<uint8_t>(output));
    
    // 11 bits (start + 8 data + parity + stop) * 2 half-bits = 22
    TEST_ASSERT_EQUAL(22, len);
    
    // Verify all bit cells have mid-bit transitions
    for (size_t i = 0; i < len; i += 2) {
        TEST_ASSERT_NOT_EQUAL(output[i], output[i + 1]);
    }
}

void test_encode_frame_different_values() {
    uint8_t testData[] = {0x00, 0xFF, 0xAA, 0x55, 0x42};
    
    for (size_t i = 0; i < sizeof(testData); ++i) {
        uint8_t output[24];
        size_t len = ManchesterCodec::encodeFrame(testData[i], std::span<uint8_t>(output));
        
        TEST_ASSERT_EQUAL(22, len);
        
        // Verify valid Manchester encoding
        auto result = ManchesterCodec::validate(std::span<const uint8_t>(output, len));
        TEST_ASSERT_TRUE(result.isOk());
    }
}

// ============================================================================
// Validation Tests
// ============================================================================

void test_validate_proper_encoding() {
    uint8_t data[] = {0x42, 0xAA, 0x55};
    uint8_t encoded[48];
    
    size_t len = ManchesterCodec::encodeBytes(std::span<const uint8_t>(data, 3), std::span<uint8_t>(encoded));
    
    auto result = ManchesterCodec::validate(std::span<const uint8_t>(encoded, len));
    TEST_ASSERT_TRUE(result.isOk());
}

void test_validate_invalid_no_transition() {
    // Create invalid Manchester: no mid-bit transition
    uint8_t invalid[] = {0, 0, 1, 0, 1, 1, 0, 1};  // Missing transitions
    
    auto result = ManchesterCodec::validate(std::span<const uint8_t>(invalid, 8));
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::ChecksumError, result.error());
}

void test_validate_insufficient_data() {
    uint8_t data[] = {0};
    auto result = ManchesterCodec::validate(std::span<const uint8_t>(data, 1));
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::InvalidFrameSize, result.error());
}

void test_validate_null_pointer() {
    auto result = ManchesterCodec::validate(std::span<const uint8_t>(static_cast<const uint8_t*>(nullptr), 10));
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(knx::util::ErrorCode::InvalidParameter, result.error());
}

// ============================================================================
// Test Runner
// ============================================================================

void runManchesterCodecTests() {
    RUN_TEST(test_timing_constants);
    RUN_TEST(test_timing_accuracy);
    
    RUN_TEST(test_parity_all_zeros);
    RUN_TEST(test_parity_all_ones);
    RUN_TEST(test_parity_one_bit);
    RUN_TEST(test_parity_two_bits);
    RUN_TEST(test_parity_alternating);
    
    RUN_TEST(test_encode_byte_all_zeros);
    RUN_TEST(test_encode_byte_all_ones);
    RUN_TEST(test_encode_byte_alternating_10);
    RUN_TEST(test_encode_byte_alternating_01);
    RUN_TEST(test_encode_byte_known_pattern);
    
    RUN_TEST(test_decode_byte_roundtrip_zeros);
    RUN_TEST(test_decode_byte_roundtrip_ones);
    RUN_TEST(test_decode_byte_roundtrip_pattern);
    RUN_TEST(test_decode_byte_insufficient_data);
    RUN_TEST(test_decode_byte_null_pointer);
    
    RUN_TEST(test_encode_bytes_multiple);
    RUN_TEST(test_decode_bytes_roundtrip);
    
    RUN_TEST(test_encode_frame_structure);
    RUN_TEST(test_encode_frame_different_values);
    
    RUN_TEST(test_validate_proper_encoding);
    RUN_TEST(test_validate_invalid_no_transition);
    RUN_TEST(test_validate_insufficient_data);
    RUN_TEST(test_validate_null_pointer);
}

#ifndef UNITY_MAIN
int main(int argc, char** argv) {
    UNITY_BEGIN();
    runManchesterCodecTests();
    return UNITY_END();
}
#endif
