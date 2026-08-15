// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tp1_frame_codec.cpp
 * @brief Unit tests for KNX TP1 frame encoding/decoding
 */

#include "knx/physical/tp1_frame_codec.hpp"
#include "unity.h"
#include <cstring>
#include <span>

using namespace knx::physical;
using namespace knx;

void setUp(void) {
    // Setup before each test
}

void tearDown(void) {
    // Cleanup after each test
}

// ============================================================================
// Frame Structure Constants
// ============================================================================

void test_frame_constants() {
    TEST_ASSERT_EQUAL(7, Tp1Frame::MIN_FRAME_SIZE);
    TEST_ASSERT_EQUAL(23, Tp1Frame::MAX_FRAME_SIZE);
    TEST_ASSERT_EQUAL(16, Tp1Frame::MAX_DATA_SIZE);
}

void test_get_frame_size() {
    TEST_ASSERT_EQUAL(7, Tp1FrameCodec::getFrameSize(0));   // No data
    TEST_ASSERT_EQUAL(8, Tp1FrameCodec::getFrameSize(1));   // 1 byte data
    TEST_ASSERT_EQUAL(15, Tp1FrameCodec::getFrameSize(8));  // 8 bytes data
    TEST_ASSERT_EQUAL(23, Tp1FrameCodec::getFrameSize(16)); // Max data
}

// ============================================================================
// Checksum Tests
// ============================================================================

void test_checksum_all_zeros() {
    Tp1Frame frame;
    frame.control = 0x00;
    frame.source = IndividualAddress(0x0000);
    frame.destination = GroupAddress(0x0000);
    frame.destinationType = AddressType::Individual;
    frame.length = 0;
    
    uint8_t checksum = Tp1FrameCodec::calculateChecksum(frame);
    TEST_ASSERT_EQUAL(0xFF, checksum);  // XOR of all zeros = 0xFF
}

void test_checksum_known_pattern() {
    Tp1Frame frame;
    frame.control = 0xBC;
    frame.source = IndividualAddress(0x1234);
    frame.destination = GroupAddress(0x5678);
    frame.destinationType = AddressType::Individual;
    frame.length = 1;
    frame.data[0] = 0x42;
    
    uint8_t checksum = Tp1FrameCodec::calculateChecksum(frame);
    
    // Calculate manually: 0xFF ^ 0xBC ^ 0x12 ^ 0x34 ^ 0x56 ^ 0x78 ^ 0x01 ^ 0x42
    uint8_t expected = 0xFF ^ 0xBC ^ 0x12 ^ 0x34 ^ 0x56 ^ 0x78 ^ 0x01 ^ 0x42;
    TEST_ASSERT_EQUAL(expected, checksum);
}

void test_checksum_from_bytes() {
    uint8_t data[] = {0x12, 0x34, 0x56, 0x78, 0x9A};
    uint8_t checksum = Tp1FrameCodec::calculateChecksum(std::span<const uint8_t>(data, 5));
    
    uint8_t expected = 0xFF ^ 0x12 ^ 0x34 ^ 0x56 ^ 0x78 ^ 0x9A;
    TEST_ASSERT_EQUAL(expected, checksum);
}

void test_checksum_empty_data() {
    uint8_t checksum = Tp1FrameCodec::calculateChecksum(std::span<const uint8_t>{});
    TEST_ASSERT_EQUAL(0xFF, checksum);
}

// ============================================================================
// Frame Encoding Tests
// ============================================================================

void test_encode_minimal_frame() {
    Tp1Frame frame;
    frame.control = 0xBC;
    frame.source = IndividualAddress(0x1234);
    frame.destination = GroupAddress(0x5678);
    frame.destinationType = AddressType::Individual;
    frame.length = 0;
    
    uint8_t output[32];
    size_t len = Tp1FrameCodec::encode(frame, std::span<uint8_t>(output));
    
    TEST_ASSERT_EQUAL(7, len);  // Minimal frame
    TEST_ASSERT_EQUAL(0xBC, output[0]);
    TEST_ASSERT_EQUAL(0x12, output[1]);
    TEST_ASSERT_EQUAL(0x34, output[2]);
    TEST_ASSERT_EQUAL(0x56, output[3]);
    TEST_ASSERT_EQUAL(0x78, output[4]);
    TEST_ASSERT_EQUAL(0x00, output[5]);
    
    // Verify checksum
    uint8_t expectedChecksum = Tp1FrameCodec::calculateChecksum(std::span<const uint8_t>(output, 6));
    TEST_ASSERT_EQUAL(expectedChecksum, output[6]);
}

void test_encode_frame_with_data() {
    Tp1Frame frame;
    frame.control = 0xBC;
    frame.source = IndividualAddress(0x1100);
    frame.destination = GroupAddress(0x0A01);
    frame.destinationType = AddressType::Individual;
    frame.length = 3;
    frame.data[0] = 0x00;
    frame.data[1] = 0x80;
    frame.data[2] = 0x42;
    
    uint8_t output[32];
    size_t len = Tp1FrameCodec::encode(frame, std::span<uint8_t>(output));
    
    TEST_ASSERT_EQUAL(10, len);  // 7 + 3 data bytes
    
    // Verify structure
    TEST_ASSERT_EQUAL(0xBC, output[0]);
    TEST_ASSERT_EQUAL(0x11, output[1]);
    TEST_ASSERT_EQUAL(0x00, output[2]);
    TEST_ASSERT_EQUAL(0x0A, output[3]);
    TEST_ASSERT_EQUAL(0x01, output[4]);
    TEST_ASSERT_EQUAL(0x03, output[5]);
    TEST_ASSERT_EQUAL(0x00, output[6]);
    TEST_ASSERT_EQUAL(0x80, output[7]);
    TEST_ASSERT_EQUAL(0x42, output[8]);
    
    // Verify checksum
    uint8_t expectedChecksum = Tp1FrameCodec::calculateChecksum(std::span<const uint8_t>(output, 9));
    TEST_ASSERT_EQUAL(expectedChecksum, output[9]);
}

void test_encode_max_data_frame() {
    Tp1Frame frame;
    frame.control = 0xBC;
    frame.source = IndividualAddress(0xFFFF);
    frame.destination = GroupAddress(0xFFFF);
    frame.destinationType = AddressType::Individual;
    frame.length = 16;
    for (size_t i = 0; i < 16; ++i) {
        frame.data[i] = static_cast<uint8_t>(i);
    }
    
    uint8_t output[32];
    size_t len = Tp1FrameCodec::encode(frame, std::span<uint8_t>(output));
    
    TEST_ASSERT_EQUAL(23, len);  // Maximum frame
    
    // Verify data
    for (size_t i = 0; i < 16; ++i) {
        TEST_ASSERT_EQUAL(i, output[6 + i]);
    }
}

void test_encode_invalid_length() {
    Tp1Frame frame;
    frame.control = 0xBC;
    frame.length = 20;  // Invalid: > MAX_DATA_SIZE
    
    uint8_t output[32];
    size_t len = Tp1FrameCodec::encode(frame, std::span<uint8_t>(output));
    
    TEST_ASSERT_EQUAL(0, len);  // Should fail
}

void test_encode_null_output() {
    Tp1Frame frame;
    size_t len = Tp1FrameCodec::encode(frame, std::span<uint8_t>(static_cast<uint8_t*>(nullptr), Tp1Frame::MAX_FRAME_SIZE));
    TEST_ASSERT_EQUAL(0, len);
}

// ============================================================================
// Frame Decoding Tests
// ============================================================================

void test_decode_minimal_frame() {
    uint8_t input[] = {0xBC, 0x12, 0x34, 0x56, 0x78, 0x00};
    
    // Calculate checksum
    uint8_t checksum = Tp1FrameCodec::calculateChecksum(std::span<const uint8_t>(input, 6));
    uint8_t fullFrame[7];
    memcpy(fullFrame, input, 6);
    fullFrame[6] = checksum;
    
    Tp1Frame frame;
    auto result = Tp1FrameCodec::decode(std::span<const uint8_t>(fullFrame, 7), frame);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(0xBC, frame.control);
    TEST_ASSERT_EQUAL(0x1234, frame.source.raw);
    TEST_ASSERT_EQUAL(AddressType::Individual, frame.destinationType);
    TEST_ASSERT_EQUAL(0x5678, frame.destination.raw);
    TEST_ASSERT_EQUAL(0, frame.length);
}

void test_decode_frame_with_data() {
    uint8_t input[] = {0xBC, 0x11, 0x00, 0x0A, 0x01, 0x03, 0x00, 0x80, 0x42};
    uint8_t checksum = Tp1FrameCodec::calculateChecksum(std::span<const uint8_t>(input, 9));
    
    uint8_t fullFrame[10];
    memcpy(fullFrame, input, 9);
    fullFrame[9] = checksum;
    
    Tp1Frame frame;
    auto result = Tp1FrameCodec::decode(std::span<const uint8_t>(fullFrame, 10), frame);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(0xBC, frame.control);
    TEST_ASSERT_EQUAL(0x1100, frame.source.raw);
    TEST_ASSERT_EQUAL(AddressType::Individual, frame.destinationType);
    TEST_ASSERT_EQUAL(0x0A01, frame.destination.raw);
    TEST_ASSERT_EQUAL(3, frame.length);
    TEST_ASSERT_EQUAL(0x00, frame.data[0]);
    TEST_ASSERT_EQUAL(0x80, frame.data[1]);
    TEST_ASSERT_EQUAL(0x42, frame.data[2]);
}

void test_decode_roundtrip() {
    Tp1Frame original;
    original.control = 0xBC;
    original.source = IndividualAddress(0x1234);
    original.destination = GroupAddress(0x5678);
    original.destinationType = AddressType::Individual;
    original.length = 5;
    for (size_t i = 0; i < 5; ++i) {
        original.data[i] = static_cast<uint8_t>(i * 10);
    }
    
    uint8_t encoded[32];
    size_t len = Tp1FrameCodec::encode(original, std::span<uint8_t>(encoded));
    TEST_ASSERT_GREATER_THAN(0, len);
    
    Tp1Frame decoded;
    auto result = Tp1FrameCodec::decode(std::span<const uint8_t>(encoded, len), decoded);
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(original.control, decoded.control);
    TEST_ASSERT_EQUAL(original.source.raw, decoded.source.raw);
    TEST_ASSERT_EQUAL(original.destinationType, decoded.destinationType);
    TEST_ASSERT_EQUAL(original.destination.raw, decoded.destination.raw);
    TEST_ASSERT_EQUAL(original.length, decoded.length);
    TEST_ASSERT_EQUAL_MEMORY(original.data, decoded.data, original.length);
}

void test_roundtrip_preserves_individual_broadcast_destination() {
    Tp1Frame original;
    original.control = 0xBC;
    original.source = IndividualAddress(0x1234);
    original.destination = GroupAddress(initialIndividualAddress().raw);
    original.destinationType = AddressType::Individual;
    original.length = 1;
    original.data[0] = 0x42;

    uint8_t encoded[32];
    size_t len = Tp1FrameCodec::encode(original, std::span<uint8_t>(encoded));
    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL_HEX8(0xFF, encoded[3]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, encoded[4]);
    TEST_ASSERT_EQUAL_HEX8(0x01, encoded[5]);

    Tp1Frame decoded;
    auto result = Tp1FrameCodec::decode(std::span<const uint8_t>(encoded, len), decoded);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(decoded.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL_HEX16(initialIndividualAddress().raw, decoded.destination.raw);
    TEST_ASSERT_EQUAL_UINT8(1, decoded.length);
    TEST_ASSERT_EQUAL_HEX8(0x42, decoded.data[0]);
}

void test_decode_invalid_checksum() {
    uint8_t input[] = {0xBC, 0x12, 0x34, 0x56, 0x78, 0x00, 0xFF};  // Wrong checksum
    
    Tp1Frame frame;
    auto result = Tp1FrameCodec::decode(std::span<const uint8_t>(input, 7), frame);
    
    TEST_ASSERT_TRUE(result.isError());  // Should fail due to bad checksum
}

void test_decode_insufficient_data() {
    uint8_t input[] = {0xBC, 0x12, 0x34};  // Too short
    
    Tp1Frame frame;
    auto result = Tp1FrameCodec::decode(std::span<const uint8_t>(input, 3), frame);
    
    TEST_ASSERT_TRUE(result.isError());
}

void test_decode_invalid_length_field() {
    uint8_t input[] = {0xBC, 0x12, 0x34, 0x56, 0x78, 0x20};  // length=32 > MAX
    uint8_t checksum = Tp1FrameCodec::calculateChecksum(std::span<const uint8_t>(input, 6));
    
    uint8_t fullFrame[7];
    memcpy(fullFrame, input, 6);
    fullFrame[6] = checksum;
    
    Tp1Frame frame;
    auto result = Tp1FrameCodec::decode(std::span<const uint8_t>(fullFrame, 7), frame);
    
    TEST_ASSERT_TRUE(result.isError());
}

void test_decode_null_input() {
    Tp1Frame frame;
    auto result = Tp1FrameCodec::decode(std::span<const uint8_t>(static_cast<const uint8_t*>(nullptr), 7), frame);
    TEST_ASSERT_TRUE(result.isError());
}

// ============================================================================
// Frame Validation Tests
// ============================================================================

void test_validate_frame_structure() {
    Tp1Frame frame;
    frame.control = 0xBC;
    frame.source = IndividualAddress(0x1234);
    frame.destination = GroupAddress(0x5678);
    frame.destinationType = AddressType::Individual;
    frame.length = 3;
    frame.data[0] = 0x00;
    frame.data[1] = 0x80;
    frame.data[2] = 0x42;
    frame.checksum = Tp1FrameCodec::calculateChecksum(frame);
    
    auto valid = Tp1FrameCodec::validate(frame);
    TEST_ASSERT_TRUE(valid.isOk());
}

void test_validate_frame_invalid_checksum() {
    Tp1Frame frame;
    frame.control = 0xBC;
    frame.length = 0;
    frame.checksum = 0x00;  // Wrong checksum
    
    auto valid = Tp1FrameCodec::validate(frame);
    TEST_ASSERT_TRUE(valid.isError());
}

void test_validate_frame_invalid_length() {
    Tp1Frame frame;
    frame.length = 20;  // > MAX_DATA_SIZE
    
    auto valid = Tp1FrameCodec::validate(frame);
    TEST_ASSERT_TRUE(valid.isError());
}

void test_validate_bytes_proper_frame() {
    uint8_t data[] = {0xBC, 0x12, 0x34, 0x56, 0x78, 0x00};
    uint8_t checksum = Tp1FrameCodec::calculateChecksum(std::span<const uint8_t>(data, 6));
    
    uint8_t frame[7];
    memcpy(frame, data, 6);
    frame[6] = checksum;
    
    auto valid = Tp1FrameCodec::validate(std::span<const uint8_t>(frame, 7));
    TEST_ASSERT_TRUE(valid.isOk());
}

void test_validate_bytes_size_mismatch() {
    uint8_t data[] = {0xBC, 0x12, 0x34, 0x56, 0x78, 0x03};  // length=3
    uint8_t checksum = Tp1FrameCodec::calculateChecksum(std::span<const uint8_t>(data, 6));
    
    uint8_t frame[7];  // But only 7 bytes total (should be 10)
    memcpy(frame, data, 6);
    frame[6] = checksum;
    
    auto valid = Tp1FrameCodec::validate(std::span<const uint8_t>(frame, 7));
    TEST_ASSERT_TRUE(valid.isError());
}

// ============================================================================
// Extract Length Tests
// ============================================================================

void test_extract_length() {
    uint8_t data[] = {0xBC, 0x12, 0x34, 0x56, 0x78, 0x05};
    
    uint8_t length = Tp1FrameCodec::extractLength(std::span<const uint8_t>(data));
    TEST_ASSERT_EQUAL(5, length);
}

void test_extract_length_masks_group_address_type_bit() {
    uint8_t data[] = {0xBC, 0x12, 0x34, 0x56, 0x78, 0x83};

    uint8_t length = Tp1FrameCodec::extractLength(std::span<const uint8_t>(data));
    TEST_ASSERT_EQUAL(3, length);
}

void test_extract_length_zero() {
    uint8_t data[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    uint8_t length = Tp1FrameCodec::extractLength(std::span<const uint8_t>(data));
    TEST_ASSERT_EQUAL(0, length);
}

void test_extract_length_null() {
    uint8_t length = Tp1FrameCodec::extractLength(std::span<const uint8_t>{});
    TEST_ASSERT_EQUAL(0, length);
}

// ============================================================================
// Test Runner
// ============================================================================

void runTp1FrameCodecTests() {
    RUN_TEST(test_frame_constants);
    RUN_TEST(test_get_frame_size);
    
    RUN_TEST(test_checksum_all_zeros);
    RUN_TEST(test_checksum_known_pattern);
    RUN_TEST(test_checksum_from_bytes);
    RUN_TEST(test_checksum_empty_data);
    
    RUN_TEST(test_encode_minimal_frame);
    RUN_TEST(test_encode_frame_with_data);
    RUN_TEST(test_encode_max_data_frame);
    RUN_TEST(test_encode_invalid_length);
    RUN_TEST(test_encode_null_output);
    
    RUN_TEST(test_decode_minimal_frame);
    RUN_TEST(test_decode_frame_with_data);
    RUN_TEST(test_decode_roundtrip);
    RUN_TEST(test_roundtrip_preserves_individual_broadcast_destination);
    RUN_TEST(test_decode_invalid_checksum);
    RUN_TEST(test_decode_insufficient_data);
    RUN_TEST(test_decode_invalid_length_field);
    RUN_TEST(test_decode_null_input);
    
    RUN_TEST(test_validate_frame_structure);
    RUN_TEST(test_validate_frame_invalid_checksum);
    RUN_TEST(test_validate_frame_invalid_length);
    RUN_TEST(test_validate_bytes_proper_frame);
    RUN_TEST(test_validate_bytes_size_mismatch);
    
    RUN_TEST(test_extract_length);
    RUN_TEST(test_extract_length_masks_group_address_type_bit);
    RUN_TEST(test_extract_length_zero);
    RUN_TEST(test_extract_length_null);
}

#ifndef UNITY_MAIN
int main(int argc, char** argv) {
    UNITY_BEGIN();
    runTp1FrameCodecTests();
    return UNITY_END();
}
#endif
