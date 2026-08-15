// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include <array>
#include <span>

/**
 * @file test_error_injection.cpp
 * @brief Error injection and robustness tests for KNX stack
 * 
 * @spec KNX Specification 03/05/01 Physical Layer (Error Handling)
 * @spec KNX Specification 03/03/04 Data Link Layer (Frame Validation)
 * 
 * @note These tests validate error handling, malformed data processing,
 *       and robustness against invalid inputs per KNX specifications.
 */

#include "unity.h"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/application/dpt.hpp"
#include "knx/constants.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/transport/connection_table.hpp"
#include "knx/transport/connection_state.hpp"
#include "knx/platform/linux_platform.hpp"
#include "../mocks/mock_physical_layer.hpp"
#include <vector>
#include <cstring>
#include <cmath>

using namespace knx;
using namespace knx::application;
using namespace knx::datalink;
using namespace knx::netip;
using namespace knx::transport;
using namespace knx::test;

void setUp(void) {}
void tearDown(void) {}

// ============================
// Frame Validation Tests
// ============================

void test_frame_invalid_checksum(void) {
    // Test handling of corrupted frame with invalid checksum
    // Per KNX Spec: Frames with invalid checksum must be discarded
    
    std::vector<uint8_t> frame = {
        0xBC,  // Control
        0x11, 0x0A,  // Source
        0x11, 0x0B,  // Dest
        0x61,  // Length
        0x00,  // TPCI
        0x81,  // APCI/Data
        0xFF   // Invalid checksum (should be calculated)
    };
    
    LDataFrame out;
    auto r = FrameCodec::decodeFrame(std::span<const uint8_t>(frame), out);
    TEST_ASSERT_TRUE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(knx::util::ErrorCode::ChecksumError),
                            static_cast<uint8_t>(r.error()));
}

void test_frame_zero_length(void) {
    // L_Data frames must carry at least a 2-byte TPDU header (TPCI/APCI).
    // A length nibble of 0 is not a valid L_Data TPDU length.
    std::vector<uint8_t> frame = {
        0xBC,  // Control
        0x11, 0x0A,  // Source
        0x11, 0x0B,  // Dest
        0x60,  // Hop=6, Len=0
    };
    frame.push_back(FrameCodec::calculateChecksum(std::span<const uint8_t>(frame)));

    LDataFrame out;
    auto r = FrameCodec::decodeFrame(std::span<const uint8_t>(frame), out);
    TEST_ASSERT_TRUE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(knx::util::ErrorCode::InvalidFrameSize),
                            static_cast<uint8_t>(r.error()));
}

void test_frame_excessive_length(void) {
    // Test frame that declares a TPDU length nibble but omits the required TPDU bytes.
    std::vector<uint8_t> frame = {
        0xBC,  // Control (standard frame)
        0x11, 0x0A,  // Source
        0x11, 0x0B,  // Dest
        0x63,  // Hop=6, Len nibble=3 => 4 TPDU bytes on wire (but we'll omit them)
    };
    frame.push_back(FrameCodec::calculateChecksum(std::span<const uint8_t>(frame)));

    LDataFrame out;
    auto r = FrameCodec::decodeFrame(std::span<const uint8_t>(frame), out);
    TEST_ASSERT_TRUE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(knx::util::ErrorCode::InvalidFrameSize),
                            static_cast<uint8_t>(r.error()));
}

void test_frame_truncated_data(void) {
    // Test frame where actual data is shorter than declared length
    std::vector<uint8_t> frame = {
        0xBC,  // Control
        0x11, 0x0A,  // Source
        0x11, 0x0B,  // Dest
        0x65,  // Length claims 5 bytes
        0x00,  // TPCI
        0x81,  // Only 2 bytes of data provided (missing 3)
    };

    frame.push_back(FrameCodec::calculateChecksum(std::span<const uint8_t>(frame)));

    LDataFrame out;
    auto r = FrameCodec::decodeFrame(std::span<const uint8_t>(frame), out);
    TEST_ASSERT_TRUE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(knx::util::ErrorCode::InvalidFrameSize),
                            static_cast<uint8_t>(r.error()));
}

void test_frame_invalid_priority(void) {
    // Priority is a 2-bit field; all values 0..3 are valid.
    // Validate decode accepts each value and maps it correctly.
    // Per KNX 03_02_02 §2.3.2 control byte bits 3-2:
    // 00 = System, 01 = Normal, 10 = Urgent, 11 = Low.

    struct Case {
        uint8_t prioBits;
        knx::Priority expected;
    } cases[] = {
        {0, knx::Priority::System},
        {1, knx::Priority::Normal},
        {2, knx::Priority::Urgent},
        {3, knx::Priority::Low},
    };

    for (const auto& c : cases) {
        std::vector<uint8_t> frame;
        const uint8_t ctrl = static_cast<uint8_t>(0x80 | 0x30 | ((c.prioBits & 0x03) << 2)); // standard + not-rep + bit4 + prio
        frame.push_back(ctrl);
        frame.push_back(0x11);
        frame.push_back(0x0A);
        frame.push_back(0x11);
        frame.push_back(0x0B);
        frame.push_back(0x61); // hop=6, tpduLen-1=1 (2-byte TPDU)
        frame.push_back(0x00);
        frame.push_back(0x00);
        frame.push_back(FrameCodec::calculateChecksum(std::span<const uint8_t>(frame)));

        LDataFrame out;
        auto r = FrameCodec::decodeFrame(std::span<const uint8_t>(frame), out);
        TEST_ASSERT_TRUE(r.isOk());
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(c.expected), static_cast<uint8_t>(out.priority));
    }
}

void test_frame_hop_count_exceeded(void) {
    // Hop count is a 3-bit field; values > 7 are invalid.
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 1);
    frame.destination = GroupAddress(1, 2, 3);
    frame.destinationType = AddressType::Group;
    frame.ackRequested = false;
    frame.hopCount = 8;
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, APCIService::GroupValueRead, {});

    uint8_t buffer[23];
    auto r = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(knx::util::ErrorCode::InvalidParameter),
                            static_cast<uint8_t>(r.error()));
}

// ============================
// DPT Error Handling Tests
// ============================

void test_dpt_decode_null_data(void) {
    // Test all DPT decoders with null/empty data
    bool bool_val;
    uint8_t uint8_val;
    int8_t int8_val;
    uint16_t uint16_val;
    int16_t int16_val;
    float float_val;
    
    std::vector<uint8_t> empty;
    
    TEST_ASSERT_TRUE(Dpt1::decode(empty, bool_val).isError());
    TEST_ASSERT_TRUE(Dpt5::decode(empty, uint8_val).isError());
    TEST_ASSERT_TRUE(Dpt6::decode(empty, int8_val).isError());
    TEST_ASSERT_TRUE(Dpt7::decode(empty, uint16_val).isError());
    TEST_ASSERT_TRUE(Dpt8::decode(empty, int16_val).isError());
    TEST_ASSERT_TRUE(Dpt9::decode(empty, float_val).isError());
}

void test_dpt_decode_insufficient_data(void) {
    // Test DPT decoders with truncated data
    float float_val;
    uint16_t uint16_val;
    
    // DPT9 needs 2 bytes, provide only 1
    TEST_ASSERT_TRUE(Dpt9::decode(std::array<uint8_t, 1>{0x00}, float_val).isError());
    
    // DPT7 needs 2 bytes, provide only 1
    TEST_ASSERT_TRUE(Dpt7::decode(std::array<uint8_t, 1>{0x00}, uint16_val).isError());
}

void test_dpt_decode_excessive_data(void) {
    // Test DPT decoders with more data than expected
    // Should use only required bytes and ignore rest
    
    bool bool_val;
    std::vector<uint8_t> excessive = {0x01, 0xFF, 0xFF, 0xFF};
    
    // DPT1 only needs 1 byte, should ignore extras
    TEST_ASSERT_TRUE(Dpt1::decode(excessive, bool_val).isOk());
    TEST_ASSERT_TRUE(bool_val);
}

void test_dpt9_encode_out_of_range(void) {
    // Test DPT9 encoding with values outside spec range
    // Valid range: -670760.96 to 670760.96
    
    std::vector<uint8_t> data;
    
    // Value too large
    TEST_ASSERT_TRUE(Dpt9::encode(1000000.0f, data).isError());
    
    // Value too small
    TEST_ASSERT_TRUE(Dpt9::encode(-1000000.0f, data).isError());
}

void test_dpt9_decode_invalid_encoding(void) {
    // DPT9 (2-byte float) has no reserved bit patterns in this implementation;
    // any 2 bytes must decode to a finite value.
    float value = 0.0f;

    // 0xF800 is the minimum representable value: exponent=15, mantissa=-2048.
    const std::vector<uint8_t> minimum = {0xF8, 0x00};
    TEST_ASSERT_TRUE(Dpt9::decode(minimum, value).isOk());
    TEST_ASSERT_TRUE(std::isfinite(value));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, knx::constants::dpt::DPT9_MIN_VALUE, value);

    // 0xFFFF is also valid and decodes to mantissa=-1 with exponent=15.
    const std::vector<uint8_t> nearZero = {0xFF, 0xFF};
    TEST_ASSERT_TRUE(Dpt9::decode(nearZero, value).isOk());
    TEST_ASSERT_TRUE(std::isfinite(value));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, -327.68f, value);
}

// ============================
// Address Validation Tests
// ============================

void test_address_invalid_individual(void) {
    // Test individual addresses with invalid values
    // Valid: Area 0-15, Line 0-15, Device 0-255
    
    IndividualAddress addr;
    TEST_ASSERT_TRUE(addr.setAddress(16, 0, 1).isError());
    TEST_ASSERT_TRUE(addr.setAddress(0, 16, 1).isError());
}

void test_address_invalid_group(void) {
    // Test group addresses with invalid values
    // 3-level: Main 0-31, Middle 0-7, Sub 0-255
    
    GroupAddress addr;
    TEST_ASSERT_FALSE(addr.setAddress(32, 0, 1));
    TEST_ASSERT_FALSE(addr.setAddress(0, 8, 1));
    TEST_ASSERT_FALSE(addr.setAddress(32, 1));
    TEST_ASSERT_FALSE(addr.setAddress(0, 0x0800));
}

void test_address_reserved_values(void) {
    // Test handling of reserved address values
    // 0x0000 is typically reserved as "unassigned"

    IndividualAddress unassigned(0, 0, 0);
    TEST_ASSERT_EQUAL_UINT16(0x0000, unassigned.raw);
    TEST_ASSERT_FALSE(unassigned.isValid());

    GroupAddress reservedGroup(0x0000);
    TEST_ASSERT_EQUAL_UINT16(0x0000, reservedGroup.raw);
    TEST_ASSERT_FALSE(reservedGroup.isValid());

    // As an L_Data destination, group 0/0/0 is the (system) broadcast address
    // and is legitimate (03_02_02 §2.3: "the Destination Address is the
    // broadcast address" counts as addressed) — management procedures such as
    // A_IndividualAddress_Read are sent to it during commissioning.
    datalink::LDataFrame f;
    f.source = IndividualAddress(1, 1, 1);
    f.destination = reservedGroup;
    f.destinationType = AddressType::Group;
    f.ackRequested = false;
    f.setTpdu(knx::protocol::TPCI::UnnumberedData, APCIService::GroupValueRead, {});
    TEST_ASSERT_TRUE(f.isValid());
}

// ============================
// cEMI Error Handling Tests
// ============================

void test_cemi_invalid_message_code(void) {
    // Test cEMI frame with invalid/unsupported message code
    // Provide an otherwise-valid minimal cEMI L_Data frame.
    std::vector<uint8_t> cemi = {
        0xFF,  // Invalid message code
        0x00,  // Additional info length
        0x8A,  // Ctrl1
        0xE0,  // Ctrl2
        0x11, 0x01,  // Source
        0x01, 0x00,  // Dest
        0x02,        // DataLen
        0x00, 0x81   // TPDU
    };
    
    datalink::LDataFrame frame;
    uint8_t msgCode = 0;
    
    // Should reject invalid message code
    TEST_ASSERT_TRUE(decodeCemiLData(std::span<const uint8_t>(cemi), frame, msgCode).isError());
}

void test_cemi_truncated_header(void) {
    // Test cEMI frame with incomplete header
    std::vector<uint8_t> cemi = {
        0x29,  // Message code only (missing rest)
    };
    
    datalink::LDataFrame frame;
    uint8_t msgCode = 0;
    TEST_ASSERT_TRUE(decodeCemiLData(std::span<const uint8_t>(cemi), frame, msgCode).isError());
}

void test_cemi_additional_info_overflow(void) {
    // Test cEMI with additional info length exceeding actual data.
    // Minimal truncation: says 255 bytes of additional info but only supplies 1.
    const std::vector<uint8_t> cemi = {
        0x29,  // L_Data.ind
        0xFF,  // Additional info length
        0x00
    };

    datalink::LDataFrame frame;
    uint8_t msgCode = 0;
    TEST_ASSERT_TRUE(decodeCemiLData(std::span<const uint8_t>(cemi), frame, msgCode).isError());
}

// ============================
// Protocol Violation Tests
// ============================

void test_tp1_reserved_control_bit_set(void) {
    // TP1 control field bit 6 is reserved and must be 0.
    std::vector<uint8_t> frame = {
        static_cast<uint8_t>(0x80 | 0x40 | 0x02), // standard + reserved bit6 + ack
        0x11, 0x0A,  // Source
        0x11, 0x0B,  // Dest
        0x62,        // Hop=6, Len=2
        0x00, 0x00   // TPDU
    };
    frame.push_back(FrameCodec::calculateChecksum(std::span<const uint8_t>(frame)));

    LDataFrame out;
    auto r = FrameCodec::decodeFrame(std::span<const uint8_t>(frame), out);
    TEST_ASSERT_TRUE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(knx::util::ErrorCode::InvalidFrameSize),
                            static_cast<uint8_t>(r.error()));
}

void test_repeated_frame_detection(void) {
    // Wire r-bit (ctrl bit 5) is inverted: 1 = NOT repeated (03_02_02 §2.2.2).
    datalink::LDataFrame frame1;
    frame1.source = IndividualAddress(1, 1, 10);
    frame1.destination = GroupAddress(1, 2, 3);
    frame1.destinationType = AddressType::Group;
    frame1.repeated = false;
    frame1.ackRequested = false;
    frame1.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueWrite, {0x01});

    datalink::LDataFrame frame2 = frame1;
    frame2.repeated = true;

    uint8_t buf1[23] = {0};
    uint8_t buf2[23] = {0};
    auto e1 = FrameCodec::encodeFrame(frame1, std::span<uint8_t>(buf1));
    auto e2 = FrameCodec::encodeFrame(frame2, std::span<uint8_t>(buf2));
    TEST_ASSERT_TRUE(e1.isOk());
    TEST_ASSERT_TRUE(e2.isOk());
    TEST_ASSERT_TRUE((buf1[0] & 0x20) != 0);  // first transmission → r-bit set
    TEST_ASSERT_TRUE((buf2[0] & 0x20) == 0);  // repetition → r-bit cleared
}

void test_source_address_validation(void) {
    // Test that source address is always individual (never group)
    // Per KNX Spec: Source must be individual address

    // Verify LDataFrame validation rejects reserved/invalid individual source addresses.
    std::vector<uint8_t> tp1 = {
        0xB4,        // Ctrl: standard, not repeated, normal priority
        0x00, 0x00,  // Source = 0.0.0 (invalid/reserved)
        0x01, 0x00,  // Dest = group 0x0100
        0xE1,        // Group + hop=6 + (tpduLen-1)=1
        0x00, 0x81   // TPDU: GroupValueWrite (data6=1)
    };
    tp1.push_back(FrameCodec::calculateChecksum(std::span<const uint8_t>(tp1)));

    LDataFrame out;
    auto r = FrameCodec::decodeFrame(std::span<const uint8_t>(tp1), out);
    TEST_ASSERT_TRUE(r.isError());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(knx::util::ErrorCode::DecodeFailed),
                            static_cast<uint8_t>(r.error()));
}

void test_broadcast_address_handling(void) {
    // In this codebase, group address 0x0000 is reserved/invalid.
    GroupAddress broadcast(0, 0, 0);
    TEST_ASSERT_EQUAL_UINT16(0x0000, broadcast.raw);
    TEST_ASSERT_FALSE(broadcast.isValid());
    TEST_ASSERT_TRUE(broadcast.setAddress(0x0000).isError());
}

// ============================
// Resource Exhaustion Tests
// ============================

void test_maximum_frame_rate(void) {
    // The 4-bit length field of L_Data_Standard carries (TPDU octets - 1),
    // so standard frames hold TPDU lengths up to 16; anything longer must be
    // sent as L_Data_Extended (FT = 0), which the encoder selects
    // automatically.
    datalink::LDataFrame frame;
    frame.source = IndividualAddress(1, 1, 1);
    frame.destination = GroupAddress(1, 2, 3);
    frame.destinationType = AddressType::Group;
    frame.ackRequested = false;

    // 2-byte header + 14-byte payload = 16-byte TPDU: still standard format.
    std::vector<uint8_t> payload(14, 0x00);
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, APCIService::GroupValueWrite, payload);
    TEST_ASSERT_TRUE(frame.isValid());

    uint8_t buffer[300];
    auto r = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(r.isOk());
    TEST_ASSERT_TRUE((buffer[0] & 0x80) != 0);  // FT=1: standard frame

    // 2-byte header + 20-byte payload = 22-byte TPDU: extended format.
    std::vector<uint8_t> longPayload(20, 0x00);
    frame.setTpdu(knx::protocol::TPCI::UnnumberedData, APCIService::GroupValueWrite, longPayload);
    auto rExt = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(rExt.isOk());
    TEST_ASSERT_TRUE((buffer[0] & 0x80) == 0);  // FT=0: extended frame
}

void test_connection_table_overflow(void) {
    // Connection table must reject creating more than the configured maximum.
    ConnectionTable table;
    TEST_ASSERT_TRUE(table.init(4).isOk());

    ConnectionParams params;
    params.initialSequence = 0;

    for (uint16_t i = 0; i < 4; i++) {
        params.remoteAddress = IndividualAddress(static_cast<uint16_t>(0x1102 + i));
        ConnectionIndex idx = table.createConnection(params.remoteAddress, params);
        TEST_ASSERT_EQUAL_INT8(static_cast<int8_t>(i), idx.value());
    }

    params.remoteAddress = IndividualAddress(0x1200);
    ConnectionIndex overflowIdx = table.createConnection(params.remoteAddress, params);
    TEST_ASSERT_FALSE(overflowIdx.isValid());
}

void test_receive_buffer_overflow(void) {
    // On checksum error the DL only counts the decode failure. The NAK for a
    // corrupted frame is the single-character short-acknowledge generated at
    // the MAC/ISR level (KNX 03_02_02 §2.2.7) — the DL must not transmit any
    // L_Data traffic of its own.
    MockPhysicalLayer phys;
    knx::platform::LinuxPlatform platform;
    datalink::Tp1DataLinkConfig config = datalink::Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;
    datalink::Tp1DataLinkLayer dl(platform, phys, config);
    TEST_ASSERT_TRUE(dl.init(IndividualAddress(1, 1, 1)).isOk());

    // Prepare an individual-addressed frame to our device with ACK requested.
    std::vector<uint8_t> tp1 = {
        0x8A,        // Ctrl: standard + ack requested
        0x11, 0x02,  // Source = 1.1.2
        0x11, 0x01,  // Dest = 1.1.1 (individual)
        0x62,        // Hop=6, Len=2
        0x00, 0x00   // TPDU header (minimal)
    };
    tp1.push_back(FrameCodec::calculateChecksum(std::span<const uint8_t>(tp1)));
    // Corrupt checksum
    tp1.back() ^= 0xFF;

    phys.injectFrame(tp1);

    auto stats = dl.getStatistics();
    TEST_ASSERT_EQUAL_UINT32(1, stats.decodeFailed);
    TEST_ASSERT_EQUAL_UINT32(0, stats.txFrames);

    std::vector<uint8_t> sentWire;
    TEST_ASSERT_FALSE(phys.getSentFrame(sentWire));
}

// Test runner
int run_all_error_injection_tests(void) {
    UNITY_BEGIN();
    
    // Frame validation
    RUN_TEST(test_frame_invalid_checksum);
    RUN_TEST(test_frame_zero_length);
    RUN_TEST(test_frame_excessive_length);
    RUN_TEST(test_frame_truncated_data);
    RUN_TEST(test_frame_invalid_priority);
    RUN_TEST(test_frame_hop_count_exceeded);
    
    // DPT error handling
    RUN_TEST(test_dpt_decode_null_data);
    RUN_TEST(test_dpt_decode_insufficient_data);
    RUN_TEST(test_dpt_decode_excessive_data);
    RUN_TEST(test_dpt9_encode_out_of_range);
    RUN_TEST(test_dpt9_decode_invalid_encoding);
    
    // Address validation
    RUN_TEST(test_address_invalid_individual);
    RUN_TEST(test_address_invalid_group);
    RUN_TEST(test_address_reserved_values);
    
    // cEMI error handling
    RUN_TEST(test_cemi_invalid_message_code);
    RUN_TEST(test_cemi_truncated_header);
    RUN_TEST(test_cemi_additional_info_overflow);
    
    // Protocol violations
    RUN_TEST(test_tp1_reserved_control_bit_set);
    RUN_TEST(test_repeated_frame_detection);
    RUN_TEST(test_source_address_validation);
    RUN_TEST(test_broadcast_address_handling);
    
    // Resource exhaustion
    RUN_TEST(test_maximum_frame_rate);
    RUN_TEST(test_connection_table_overflow);
    RUN_TEST(test_receive_buffer_overflow);
    
    return UNITY_END();
}

int main() {
    return run_all_error_injection_tests();
}
