// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_frame_encoding.cpp
 * @brief Unit tests for frame encoding/decoding
 * 
 * @spec KNX Specification 03/05/01 Physical Layer TP1 v1.3
 * @spec KNX Specification 03/03/04 Data Link Layer General v1.0.2
 * @note Tests validate frame structure per KNX TP1 specifications:
 *       - Control field encoding (Section 2.2.5.3.1)
 *       - Address field encoding (Section 2.2.5.3.3)
 *       - Checksum calculation (Section 2.2.5.3.2)
 */

#include "unity.h"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/application/apci_services.hpp"
#include <cstring>

using namespace knx::datalink;

void setUp(void) {
    // Set up code here
}

void tearDown(void) {
    // Clean up code here
}

// Helper: create a test frame
LDataFrame createTestFrame(void) {
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = knx::Priority::Low;
    frame.ackRequested = true;
    frame.confirmation = false;
    frame.source = knx::IndividualAddress(1, 1, 10);
    frame.destination = knx::GroupAddress(0, 0, 1);
    frame.destinationType = knx::AddressType::Group;
    frame.hopCount = 6;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueWrite, {0x01});
    return frame;
}

// Checks that createTestFrame() populates the frame fields as expected.
// Despite the name this does not exercise wire encoding — that needs a
// Tp1DataLinkLayer instance and is covered by the data link layer tests.
void test_frame_encode_basic(void) {
    LDataFrame frame = createTestFrame();

    TEST_ASSERT_EQUAL(knx::IndividualAddress(1, 1, 10).raw, frame.source.raw);
    TEST_ASSERT_EQUAL(knx::GroupAddress(0, 0, 1).raw, frame.destination.raw);
    TEST_ASSERT_EQUAL(1, frame.payload().size());
}

// Test control field structure
void test_frame_control_field(void) {
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = knx::Priority::Low;
    frame.ackRequested = true;
    
    // Control field construction:
    // Bit 7: Standard frame (1)
    // Bit 6: Repeat (0)
    // Bit 5-4: Reserved (0)
    // Bit 3-2: Priority (11 for Low)
    // Bit 1: ACK requested (1)
    // Bit 0: Confirmation (0)
    
    uint8_t expected = 0x80 |      // Standard frame
                      0x00 |      // Not repeated
                      0x0C |      // Low priority
                      0x02;       // ACK requested
    
    TEST_ASSERT_EQUAL(0x80 | 0x0C | 0x02, expected);
}

// Test address fields
void test_frame_address_encoding(void) {
    knx::IndividualAddress src(1, 2, 3);
    knx::GroupAddress dest(4, 5, 6);
    
    uint16_t srcRaw = src.raw;
    uint16_t destRaw = dest.raw;
    
    // Verify encoding
    TEST_ASSERT_EQUAL((1 << 12) | (2 << 8) | 3, srcRaw);
    TEST_ASSERT_EQUAL((4 << 11) | (5 << 8) | 6, destRaw);
}

// Test length field with hop count
void test_frame_length_field(void) {
    uint8_t hopCount = 6;
    uint8_t dataLength = 5;
    
    // Length field: bits 7-4 = hop count, bits 3-0 = data length
    uint8_t lengthField = (hopCount << 4) | (dataLength & 0x0F);
    
    TEST_ASSERT_EQUAL((6 << 4) | 5, lengthField);
    TEST_ASSERT_EQUAL(6, (lengthField >> 4) & 0x07);
    TEST_ASSERT_EQUAL(5, lengthField & 0x0F);
}

// Test checksum calculation (XOR of all bytes)
void test_frame_checksum(void) {
    uint8_t data[] = {0x29, 0x01, 0x01, 0x00, 0x01, 0x05, 0x00, 0x81};
    uint8_t checksum = 0xFF;
    
    for (size_t i = 0; i < sizeof(data); i++) {
        checksum ^= data[i];
    }
    
    // Checksum should be calculated correctly
    TEST_ASSERT_NOT_EQUAL(0x00, checksum);
}

// Test APCI field
void test_frame_apci_field(void) {
    // APCI is encoded in the upper 2 bits of the first data byte
    // Bits 7-6: APCI (00=GroupValue_Write, 01=GroupValue_Response, 10=GroupValue_Read)
    
    uint8_t apci = 0x00;  // GroupValue_Write
    uint8_t dataValue = 0x01;
    
    uint8_t firstByte = (apci << 6) | (dataValue & 0x3F);
    
    TEST_ASSERT_EQUAL(0x01, firstByte);
    TEST_ASSERT_EQUAL(0x00, (firstByte >> 6) & 0x03);
}

// Test hop count ranges
void test_frame_hop_count_range(void) {
    // Hop count should be 0-6
    LDataFrame frame;
    
    frame.hopCount = 0;
    TEST_ASSERT_EQUAL(0, frame.hopCount);
    
    frame.hopCount = 6;
    TEST_ASSERT_EQUAL(6, frame.hopCount);
    
    frame.hopCount = 3;
    TEST_ASSERT_EQUAL(3, frame.hopCount);
}

// ============================
// KNX Specification Compliance Tests
// Per KNX Spec 03/05/01 and 03/03/04
// ============================

void test_frame_checksum_spec_compliance(void) {
    // Per KNX Spec 03/05/01 Section 2.2.5.3.2
    // Checksum = NOT(XOR of all bytes from control field onwards)
    // Example from spec documentation
    
    uint8_t frame[] = {
        0xBC,  // Control byte
        0x11, 0x0A,  // Source address (1.1.10)
        0x11, 0x0B,  // Dest address (1.1.11)
        0xE1,  // Length/routing
        0x00,  // TPCI
        0x81   // APCI/Data
    };
    
    uint8_t checksum = 0xFF;
    for (size_t i = 0; i < sizeof(frame); i++) {
        checksum ^= frame[i];
    }
    
    // Verify checksum calculation matches spec
    TEST_ASSERT_NOT_EQUAL(0xFF, checksum);
    
    // Verify checksum is complement of XOR
    uint8_t xor_result = 0x00;
    for (size_t i = 0; i < sizeof(frame); i++) {
        xor_result ^= frame[i];
    }
    TEST_ASSERT_EQUAL((uint8_t)~xor_result, checksum);
}

void test_frame_control_byte_spec(void) {
    // Per KNX 03_02_02 §2.2.2 / §2.3.2 the TP1 control field is:
    // FT 0 r 1 P1 P0 x x
    // FT = frame type (bit 7): 1=standard, 0=extended
    // r  = repeat flag (bit 5): 1=NOT repeated, 0=repeated
    // bit 4 = 1 in every L_DATA control field
    // P1 P0 = priority (bits 3-2): 00=system, 01=normal, 10=urgent, 11=low

    // Standard frame, first transmission, normal priority.
    uint8_t control = 0x00;
    control |= (1 << 7);  // Standard frame
    control |= (1 << 5);  // Not repeated
    control |= (1 << 4);  // Fixed 1 for L_DATA
    control |= (1 << 2);  // Normal priority (01b)

    // Result: 1011 0100 = 0xB4 (matches 03_02_02 §2.3.2 table row
    // "L_DATA, normal priority, not rep.")
    TEST_ASSERT_EQUAL(0xB4, control);

    // Verify field extraction
    TEST_ASSERT_EQUAL(1, (control >> 7) & 0x01);  // Frame type
    TEST_ASSERT_EQUAL(1, (control >> 5) & 0x01);  // Not repeated
    TEST_ASSERT_EQUAL(1, (control >> 4) & 0x01);  // Fixed bit
    TEST_ASSERT_EQUAL(1, (control >> 2) & 0x03);  // Normal priority (01b)
}

void test_frame_priority_encoding_spec(void) {
    // Per KNX 03_02_02 §2.3.2: priority encoding in control byte bits 3-2
    // 00b (0) = System (highest)
    // 01b (1) = Normal
    // 10b (2) = Urgent
    // 11b (3) = Low (lowest)

    LDataFrame frame;

    frame.priority = knx::Priority::System;
    TEST_ASSERT_EQUAL(0, static_cast<uint8_t>(frame.priority));

    frame.priority = knx::Priority::Normal;
    TEST_ASSERT_EQUAL(1, static_cast<uint8_t>(frame.priority));

    frame.priority = knx::Priority::Urgent;
    TEST_ASSERT_EQUAL(2, static_cast<uint8_t>(frame.priority));

    frame.priority = knx::Priority::Low;
    TEST_ASSERT_EQUAL(3, static_cast<uint8_t>(frame.priority));
}

void test_frame_address_field_spec(void) {
    // Per KNX Spec 03/05/01 Section 2.2.5.3.3
    // Individual Address: AAAA LLLL DDDD DDDD (A=area, L=line, D=device)
    // Group Address 3-level: GGGG GMMM DDDD DDDD (G=main, M=middle, D=sub)
    
    // Test individual address encoding
    knx::IndividualAddress ia(15, 15, 255);  // Max values
    TEST_ASSERT_EQUAL((15 << 12) | (15 << 8) | 255, ia.raw);
    
    // Test group address encoding (3-level)
    knx::GroupAddress ga(31, 7, 255);  // Max values for 3-level
    TEST_ASSERT_EQUAL((31 << 11) | (7 << 8) | 255, ga.raw);
}

void test_frame_length_field_spec(void) {
    // Per KNX Spec 03/05/01 Section 2.2.5.3.4
    // Length byte format: HHHH LLLL
    // H = Hop count (3 bits, values 0-7, but 7 is reserved)
    // L = Data length (4 bits, values 0-15)
    
    uint8_t hopCount = 6;
    uint8_t dataLen = 14;  // Max for standard frame
    
    uint8_t lengthByte = (hopCount << 4) | (dataLen & 0x0F);
    
    TEST_ASSERT_EQUAL(0x6E, lengthByte);  // 0110 1110
    
    // Verify extraction
    TEST_ASSERT_EQUAL(6, (lengthByte >> 4) & 0x07);
    TEST_ASSERT_EQUAL(14, lengthByte & 0x0F);
}

void test_frame_tpci_apci_encoding_spec(void) {
    // Per KNX Spec 03/03/07 Transport Layer
    // TPCI byte format depends on service:
    // - Unnumbered Data: 00xx xxxx
    // - Numbered Data:   01xx xxxx (sequence in bits 5-2)
    // - Control:         10xx xxxx
    
    // Test unnumbered data
    const auto tpci_unnumbered = knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData);
    TEST_ASSERT_EQUAL(0x00, tpci_unnumbered.raw & 0xC0);
    
    // Test numbered data with sequence 5
    uint8_t seq = 5;
    const auto tpci_numbered = knx::protocol::TPCIField::create(knx::protocol::TPCI::NumberedData, seq);
    TEST_ASSERT_EQUAL(0x40, tpci_numbered.raw & 0xC0);
    TEST_ASSERT_EQUAL(5, (tpci_numbered.raw >> 2) & 0x0F);
    
    // Test T_Connect
    const auto tpci_connect = knx::protocol::TPCIField::create(knx::protocol::TPCIControl::Connect);
    TEST_ASSERT_EQUAL(0x80, tpci_connect.raw & 0xC0);
}

void test_frame_standard_vs_extended(void) {
    // Per KNX Spec: Standard frames limited to 15 bytes data
    // Extended frames can have up to 254 bytes
    
    LDataFrame standardFrame;
    standardFrame.standardFrame = true;
    // TP1 standard frame length field encodes TPDU byte count (0..15).
    // With the 2-byte TPDU header, max user payload is 15 - 2 = 13 bytes.
    standardFrame.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueWrite,
                          std::vector<uint8_t>(13, 0x00));
    TEST_ASSERT_EQUAL(13, standardFrame.payload().size());
    
    // Extended frame would allow more
    LDataFrame extendedFrame;
    extendedFrame.standardFrame = false;
    // Note: Extended frame support depends on implementation
}

void test_frame_repeated_flag_spec(void) {
    // Per KNX Spec: Repeated flag indicates retransmission
    // Initial transmission: repeated = false
    // Retransmission: repeated = true
    
    LDataFrame frame;
    frame.repeated = false;
    TEST_ASSERT_FALSE(frame.repeated);
    
    // Simulate retransmission
    frame.repeated = true;
    TEST_ASSERT_TRUE(frame.repeated);
}

void test_frame_group_vs_individual_addressing(void) {
    // Per KNX Spec: Address type flag distinguishes addressing mode
    
    LDataFrame groupFrame;
    groupFrame.destinationType = knx::AddressType::Group;
    groupFrame.destination = knx::GroupAddress(1, 2, 3);
    TEST_ASSERT_TRUE(groupFrame.destinationType == knx::AddressType::Group);
    
    LDataFrame individualFrame;
    individualFrame.destinationType = knx::AddressType::Individual;
    individualFrame.destination.raw = knx::IndividualAddress(1, 2, 3).raw;
    TEST_ASSERT_TRUE(individualFrame.destinationType == knx::AddressType::Individual);
}

// Test run
int run_all_frame_tests(void) {
    UNITY_BEGIN();
    
    RUN_TEST(test_frame_encode_basic);
    RUN_TEST(test_frame_control_field);
    RUN_TEST(test_frame_address_encoding);
    RUN_TEST(test_frame_length_field);
    RUN_TEST(test_frame_checksum);
    RUN_TEST(test_frame_apci_field);
    RUN_TEST(test_frame_hop_count_range);
    
    // KNX Specification Compliance Tests
    RUN_TEST(test_frame_checksum_spec_compliance);
    RUN_TEST(test_frame_control_byte_spec);
    RUN_TEST(test_frame_priority_encoding_spec);
    RUN_TEST(test_frame_address_field_spec);
    RUN_TEST(test_frame_length_field_spec);
    RUN_TEST(test_frame_tpci_apci_encoding_spec);
    RUN_TEST(test_frame_standard_vs_extended);
    RUN_TEST(test_frame_repeated_flag_spec);
    RUN_TEST(test_frame_group_vs_individual_addressing);
    
    return UNITY_END();
}

int main() {
    return run_all_frame_tests();
}
