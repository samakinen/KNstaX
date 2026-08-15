// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_frame_codec_errors.cpp
 * @brief Unit tests for FrameCodec Result<T> error handling
 * 
 * Tests all error paths and Result<T> pattern usage in frame encoding/decoding.
 */

#include "unity.h"
#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/util/result.hpp"
#include <array>
#include <cstring>
#include <span>

using namespace knx;
using namespace knx::datalink;
using namespace knx::util;

void setUp(void) {}
void tearDown(void) {}

// Helper: create a valid test frame
static LDataFrame createValidFrame() {
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = false;
    frame.confirmation = false;
    frame.source = IndividualAddress(1, 1, 10);
    frame.destination = GroupAddress(0, 0, 1);
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
    return frame;
}

// Test: Successful encoding returns Ok with byte count
void test_encode_success_returns_size(void) {
    LDataFrame frame = createValidFrame();
    uint8_t buffer[32];
    size_t maxLen = sizeof(buffer);
    
    auto result = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer, maxLen));
    
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_FALSE(result.isError());
    TEST_ASSERT_TRUE(result.value() > 0);
    TEST_ASSERT_TRUE(result.value() <= maxLen);
}

// Test: Encoding with null buffer returns InvalidParameter
void test_encode_null_buffer_returns_error(void) {
    LDataFrame frame = createValidFrame();
    size_t maxLen = 32;
    
    auto result = FrameCodec::encodeFrame(frame, std::span<uint8_t>(static_cast<uint8_t*>(nullptr), maxLen));
    
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_FALSE(result.isOk());
    TEST_ASSERT_EQUAL(ErrorCode::InvalidParameter, result.error());
}

// Test: Encoding with zero length returns BufferTooSmall
void test_encode_zero_length_returns_error(void) {
    LDataFrame frame = createValidFrame();
    uint8_t buffer[32];
    
    auto result = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer, 0));
    
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(ErrorCode::BufferTooSmall, result.error());
}

// Test: Encoding with buffer too small returns BufferTooSmall
void test_encode_buffer_too_small_returns_error(void) {
    LDataFrame frame = createValidFrame();
    uint8_t buffer[5];  // Too small for a valid frame
    
    auto result = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(ErrorCode::BufferTooSmall, result.error());
}

// Test: a TPDU too large for a standard frame auto-selects L_Data_Extended
// (CTRL bits 7..6 == 00) instead of failing; a too-small output buffer is
// still rejected.
void test_encode_excessive_data_returns_error(void) {
    LDataFrame frame = createValidFrame();
    // 20-byte payload + 2-byte TPCI/APCI header exceeds the 16-byte standard
    // frame TPDU limit, so the codec must emit an extended frame.
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, std::vector<uint8_t>(20, 0xFF));

    uint8_t buffer[64];
    auto result = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_HEX8(0x00, buffer[0] & 0xC0); // extended frame CTRL marker

    // The same frame must still be rejected when the output buffer can't fit it.
    uint8_t tinyBuffer[8];
    auto tinyResult = FrameCodec::encodeFrame(frame, std::span<uint8_t>(tinyBuffer));
    TEST_ASSERT_TRUE(tinyResult.isError());
}

void test_encode_length_field_is_tpdu_minus_one(void) {
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = false;
    frame.confirmation = false;
    frame.source = IndividualAddress(1, 1, 2);
    frame.destination = GroupAddress(1, 2, 10);
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x13, 0x20});

    uint8_t buffer[32] = {};
    auto result = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT8(0xE3, buffer[5]);
}

void test_decode_real_tp1_group_write_length_field(void) {
    const std::array<uint8_t, 11> wire = {
        0xBC,
        0x11, 0x02,
        0x0A, 0x0A,
        0xE3,
        0x00, 0x80, 0x13, 0x20,
        0x00,
    };

    std::array<uint8_t, 11> frame = wire;
    frame.back() = FrameCodec::calculateChecksum(std::span<const uint8_t>(frame).first(frame.size() - 1u));

    LDataFrame decodedFrame;
    auto decodeResult = FrameCodec::decodeFrame(std::span<const uint8_t>(frame), decodedFrame);

    TEST_ASSERT_TRUE(decodeResult.isOk());
    TEST_ASSERT_EQUAL_UINT16(0x1102, decodedFrame.source.raw);
    TEST_ASSERT_EQUAL_UINT16(0x0A0A, decodedFrame.destination.raw);
    TEST_ASSERT_EQUAL_UINT8(4u, static_cast<uint8_t>(decodedFrame.tpdu.size()));
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::GroupValueWrite),
                             static_cast<uint16_t>(decodedFrame.apci().service()));
}

// Test: Successful decoding returns Ok
void test_decode_success_returns_ok(void) {
    // First encode a valid frame
    LDataFrame originalFrame = createValidFrame();
    uint8_t buffer[32];
    size_t maxLen = sizeof(buffer);
    
    auto encodeResult = FrameCodec::encodeFrame(originalFrame, std::span<uint8_t>(buffer, maxLen));
    TEST_ASSERT_TRUE(encodeResult.isOk());
    size_t encodedLen = encodeResult.value();
    
    // Now decode it
    LDataFrame decodedFrame;
    auto decodeResult = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer, encodedLen), decodedFrame);
    
    TEST_ASSERT_TRUE(decodeResult.isOk());
    TEST_ASSERT_FALSE(decodeResult.isError());
    
    // Verify the frame was decoded correctly
    TEST_ASSERT_EQUAL(originalFrame.source.raw, decodedFrame.source.raw);
    TEST_ASSERT_EQUAL(originalFrame.destination.raw, decodedFrame.destination.raw);
    TEST_ASSERT_EQUAL(originalFrame.apci().raw, decodedFrame.apci().raw);
}

// Test: Decoding null buffer returns InvalidParameter
void test_decode_null_buffer_returns_error(void) {
    LDataFrame frame;
    
    auto result = FrameCodec::decodeFrame(std::span<const uint8_t>(static_cast<const uint8_t*>(nullptr), 10), frame);
    
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(ErrorCode::InvalidParameter, result.error());
}

// Test: Decoding zero length returns InvalidFrameSize
void test_decode_zero_length_returns_error(void) {
    uint8_t buffer[32];
    LDataFrame frame;
    
    auto result = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer, 0), frame);
    
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(ErrorCode::InvalidFrameSize, result.error());
}

// Test: Decoding frame too short returns InvalidFrameSize
void test_decode_too_short_returns_error(void) {
    uint8_t buffer[5] = {0x29, 0x01, 0x02, 0x03, 0x04};  // Too short
    LDataFrame frame;
    
    auto result = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer), frame);
    
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(ErrorCode::InvalidFrameSize, result.error());
}

// Test: Decoding with bad checksum returns ChecksumError
void test_decode_bad_checksum_returns_error(void) {
    // First encode a valid frame
    LDataFrame originalFrame = createValidFrame();
    uint8_t buffer[32];
    size_t maxLen = sizeof(buffer);
    
    auto encodeResult = FrameCodec::encodeFrame(originalFrame, std::span<uint8_t>(buffer, maxLen));
    TEST_ASSERT_TRUE(encodeResult.isOk());
    size_t encodedLen = encodeResult.value();
    
    // Corrupt the checksum (last byte)
    buffer[encodedLen - 1] ^= 0xFF;
    
    // Try to decode
    LDataFrame decodedFrame;
    auto decodeResult = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer, encodedLen), decodedFrame);
    
    TEST_ASSERT_TRUE(decodeResult.isError());
    TEST_ASSERT_EQUAL(ErrorCode::ChecksumError, decodeResult.error());
}

// Test: Result<T> can be used in conditional expressions
void test_result_conditional_usage(void) {
    LDataFrame frame = createValidFrame();
    uint8_t buffer[32];
    
    auto result = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    
    // Test boolean conversion
    if (result.isOk()) {
        size_t encodedSize = result.value();
        TEST_ASSERT_TRUE(encodedSize > 0);
    } else {
        TEST_FAIL_MESSAGE("Expected successful encoding");
    }
}

// Test: Result<T> error() method only valid when isError()
void test_result_error_access(void) {
    LDataFrame frame = createValidFrame();
    uint8_t buffer[5];  // Too small
    
    auto result = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    
    TEST_ASSERT_TRUE(result.isError());
    ErrorCode error = result.error();
    TEST_ASSERT_EQUAL(ErrorCode::BufferTooSmall, error);
}

// Test: Result<void> for decodeFrame works correctly
void test_result_void_decode(void) {
    LDataFrame originalFrame = createValidFrame();
    uint8_t buffer[32];
    
    auto encodeResult = FrameCodec::encodeFrame(originalFrame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(encodeResult.isOk());
    
    LDataFrame decodedFrame;
    auto decodeResult = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer, encodeResult.value()), decodedFrame);
    
    // Result<void> should only have isOk() and isError()
    TEST_ASSERT_TRUE(decodeResult.isOk());
    TEST_ASSERT_FALSE(decodeResult.isError());
    // No value() method for Result<void>
}

void test_encode_with_span_buffer(void) {
    LDataFrame frame = createValidFrame();
    std::array<uint8_t, 32> buffer{};

    auto result = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(result.value() > 0);
}

void test_decode_with_span_buffer(void) {
    LDataFrame originalFrame = createValidFrame();
    std::array<uint8_t, 32> buffer{};

    auto encodeResult = FrameCodec::encodeFrame(originalFrame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(encodeResult.isOk());

    LDataFrame decodedFrame;
    auto decodeResult = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer).first(encodeResult.value()), decodedFrame);

    TEST_ASSERT_TRUE(decodeResult.isOk());
    TEST_ASSERT_EQUAL(originalFrame.source.raw, decodedFrame.source.raw);
    TEST_ASSERT_EQUAL(originalFrame.destination.raw, decodedFrame.destination.raw);
}

void test_verify_checksum_with_span_buffer(void) {
    LDataFrame frame = createValidFrame();
    std::array<uint8_t, 32> buffer{};

    auto encodeResult = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(encodeResult.isOk());

    auto verifyResult = FrameCodec::verifyChecksum(std::span<const uint8_t>(buffer).first(encodeResult.value()));
    TEST_ASSERT_TRUE(verifyResult.isOk());
}

// Test: Round-trip encoding and decoding
void test_roundtrip_encoding_decoding(void) {
    LDataFrame originalFrame = createValidFrame();
    originalFrame.hopCount = 5;
    originalFrame.priority = Priority::System;
    originalFrame.ackRequested = true;
    originalFrame.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x02, 0xAB});
    
    uint8_t buffer[32];
    auto encodeResult = FrameCodec::encodeFrame(originalFrame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(encodeResult.isOk());
    
    LDataFrame decodedFrame;
    auto decodeResult = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer, encodeResult.value()), decodedFrame);
    TEST_ASSERT_TRUE(decodeResult.isOk());
    
    // Verify all fields match
    TEST_ASSERT_EQUAL(originalFrame.standardFrame, decodedFrame.standardFrame);
    TEST_ASSERT_EQUAL(originalFrame.repeated, decodedFrame.repeated);
    TEST_ASSERT_EQUAL(originalFrame.priority, decodedFrame.priority);
    TEST_ASSERT_EQUAL(originalFrame.ackRequested, decodedFrame.ackRequested);
    TEST_ASSERT_EQUAL(originalFrame.confirmation, decodedFrame.confirmation);
    TEST_ASSERT_EQUAL(originalFrame.source.raw, decodedFrame.source.raw);
    TEST_ASSERT_EQUAL(originalFrame.destination.raw, decodedFrame.destination.raw);
    TEST_ASSERT_EQUAL(originalFrame.destinationType, decodedFrame.destinationType);
    TEST_ASSERT_EQUAL(originalFrame.hopCount, decodedFrame.hopCount);
    TEST_ASSERT_EQUAL(originalFrame.tpci().raw, decodedFrame.tpci().raw);
    TEST_ASSERT_EQUAL(originalFrame.apci().raw, decodedFrame.apci().raw);
    auto inPayload = originalFrame.payload();
    auto outPayload = decodedFrame.payload();
    TEST_ASSERT_EQUAL(inPayload.size(), outPayload.size());
    for (size_t i = 0; i < inPayload.size(); i++) {
        TEST_ASSERT_EQUAL(inPayload[i], outPayload[i]);
    }
}

// Test: Multiple frames can be encoded/decoded independently
void test_multiple_independent_frames(void) {
    LDataFrame frame1 = createValidFrame();
    frame1.source = IndividualAddress(1, 0, 1);
    frame1.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x11});
    
    LDataFrame frame2 = createValidFrame();
    frame2.source = IndividualAddress(2, 0, 2);
    frame2.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x22, 0x33});
    
    uint8_t buffer1[32], buffer2[32];
    
    auto result1 = FrameCodec::encodeFrame(frame1, std::span<uint8_t>(buffer1));
    auto result2 = FrameCodec::encodeFrame(frame2, std::span<uint8_t>(buffer2));
    
    TEST_ASSERT_TRUE(result1.isOk());
    TEST_ASSERT_TRUE(result2.isOk());
    
    LDataFrame decoded1, decoded2;
    auto decode1 = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer1, result1.value()), decoded1);
    auto decode2 = FrameCodec::decodeFrame(std::span<const uint8_t>(buffer2, result2.value()), decoded2);
    
    TEST_ASSERT_TRUE(decode1.isOk());
    TEST_ASSERT_TRUE(decode2.isOk());
    
    TEST_ASSERT_EQUAL(frame1.source.raw, decoded1.source.raw);
    TEST_ASSERT_EQUAL(frame2.source.raw, decoded2.source.raw);
    TEST_ASSERT_EQUAL(1, decoded1.payload().size());
    TEST_ASSERT_EQUAL(2, decoded2.payload().size());
}

int main() {
    UNITY_BEGIN();
    
    // Success cases
    RUN_TEST(test_encode_success_returns_size);
    RUN_TEST(test_decode_success_returns_ok);
    
    // Encoding error cases
    RUN_TEST(test_encode_null_buffer_returns_error);
    RUN_TEST(test_encode_zero_length_returns_error);
    RUN_TEST(test_encode_buffer_too_small_returns_error);
    RUN_TEST(test_encode_excessive_data_returns_error);
    RUN_TEST(test_encode_length_field_is_tpdu_minus_one);
    
    // Decoding error cases
    RUN_TEST(test_decode_null_buffer_returns_error);
    RUN_TEST(test_decode_zero_length_returns_error);
    RUN_TEST(test_decode_too_short_returns_error);
    RUN_TEST(test_decode_bad_checksum_returns_error);
    
    // Result<T> usage patterns
    RUN_TEST(test_result_conditional_usage);
    RUN_TEST(test_result_error_access);
    RUN_TEST(test_result_void_decode);
    RUN_TEST(test_decode_real_tp1_group_write_length_field);
    RUN_TEST(test_encode_with_span_buffer);
    RUN_TEST(test_decode_with_span_buffer);
    RUN_TEST(test_verify_checksum_with_span_buffer);
    
    // Integration tests
    RUN_TEST(test_roundtrip_encoding_decoding);
    RUN_TEST(test_multiple_independent_frames);
    
    return UNITY_END();
}
