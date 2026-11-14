// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_dpt10_11_19.cpp
 * @brief Unit tests for DPT 10 (Time), DPT 11 (Date), and DPT 19 (DateTime)
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
// DPT 10 - Time of Day
// ============================

void test_Dpt10_EncodeMidnight(void) {
    Dpt10::Value time{0, 0, 0, 0};  // No day, 00:00:00
    
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt10::encode(Dpt10::Value{0, 0, 0, 0}, out); });
    TEST_ASSERT_EQUAL_UINT(3, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x00, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[2]);
}

void test_Dpt10_EncodeNoon(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt10::encode(1, 12, 0, 0, out); });  // Monday 12:00:00
    TEST_ASSERT_EQUAL_UINT(3, data.size());
    // Day=1 (001), Hour=12 (01100)
    // Byte 0: [D D D H H H H H] = 001 01100 = 0x2C
    // Byte 1: [M M M M M M S S] = 000000 00 = 0x00
    // Byte 2: [S S S S 0 0 0 0] = 0000 0000 = 0x00
    TEST_ASSERT_EQUAL_HEX8(0x2C, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, data[2]);
}

void test_Dpt10_EncodeAfternoonTime(void) {
    Dpt10::Value time{3, 15, 30, 45};  // Wednesday 15:30:45
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt10::encode(time, out); });
    TEST_ASSERT_EQUAL_UINT(3, data.size());
}

void test_Dpt10_EncodeEndOfDay(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt10::encode(7, 23, 59, 59, out); });  // Sunday 23:59:59
    TEST_ASSERT_EQUAL_UINT(3, data.size());
}

void test_Dpt10_EncodeInvalidHour(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt10::encode(1, 24, 0, 0, out); }).isError());  // Hour 24 invalid
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt10::encode(1, 25, 0, 0, out); }).isError());
}

void test_Dpt10_EncodeInvalidMinute(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt10::encode(1, 12, 60, 0, out); }).isError());  // Minute 60 invalid
}

void test_Dpt10_EncodeInvalidSecond(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt10::encode(1, 12, 30, 60, out); }).isError());  // Second 60 invalid
}

void test_Dpt10_EncodeInvalidDayOfWeek(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt10::encode(8, 12, 0, 0, out); }).isError());  // Day 8 invalid
}

void test_Dpt10_DecodeMidnight(void) {
    Dpt10::Value time;
    std::vector<uint8_t> data = {0x00, 0x00, 0x00};
    
    TEST_ASSERT_TRUE(Dpt10::decode(data, time).isOk());
    TEST_ASSERT_EQUAL_UINT8(0, time.dayOfWeek);
    TEST_ASSERT_EQUAL_UINT8(0, time.hour);
    TEST_ASSERT_EQUAL_UINT8(0, time.minute);
    TEST_ASSERT_EQUAL_UINT8(0, time.second);
}

void test_Dpt10_DecodeNoon(void) {
    Dpt10::Value time;
    std::vector<uint8_t> data = {0x2C, 0x00, 0x00};  // Monday 12:00:00
    
    TEST_ASSERT_TRUE(Dpt10::decode(data, time).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, time.dayOfWeek);
    TEST_ASSERT_EQUAL_UINT8(12, time.hour);
    TEST_ASSERT_EQUAL_UINT8(0, time.minute);
    TEST_ASSERT_EQUAL_UINT8(0, time.second);
}

void test_Dpt10_DecodeInsufficientData(void) {
    Dpt10::Value time;
    TEST_ASSERT_TRUE(Dpt10::decode(std::span<const uint8_t>{}, time).isError());
    TEST_ASSERT_TRUE(Dpt10::decode(std::to_array<uint8_t>({0x00}), time).isError());
    TEST_ASSERT_TRUE(Dpt10::decode(std::to_array<uint8_t>({0x00, 0x00}), time).isError());
}

void test_Dpt10_RoundTrip(void) {
    Dpt10::Value original{5, 18, 45, 30};  // Friday 18:45:30
    Dpt10::Value decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt10::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt10::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT8(original.dayOfWeek, decoded.dayOfWeek);
    TEST_ASSERT_EQUAL_UINT8(original.hour, decoded.hour);
    TEST_ASSERT_EQUAL_UINT8(original.minute, decoded.minute);
    TEST_ASSERT_EQUAL_UINT8(original.second, decoded.second);
}

// ============================
// DPT 11 - Date
// ============================

void test_Dpt11_EncodeEpoch(void) {
    Dpt11::Value date{1, 1, 0};  // 2000-01-01
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt11::encode(date, out); });
    TEST_ASSERT_EQUAL_UINT(3, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x01, data[0]);  // Day 1
    TEST_ASSERT_EQUAL_HEX8(0x01, data[1]);  // Month 1
    TEST_ASSERT_EQUAL_HEX8(0x00, data[2]);  // Year 0 (2000)
}

void test_Dpt11_EncodeMidYear(void) {
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt11::encode(15, 6, 25, out); });  // 2025-06-15
    TEST_ASSERT_EQUAL_UINT(3, data.size());
    TEST_ASSERT_EQUAL_HEX8(15, data[0]);
    TEST_ASSERT_EQUAL_HEX8(6, data[1]);
    TEST_ASSERT_EQUAL_HEX8(25, data[2]);
}

void test_Dpt11_EncodeEndOfCentury(void) {
    Dpt11::Value date{31, 12, 99};  // 2099-12-31
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt11::encode(date, out); });
    TEST_ASSERT_EQUAL_UINT(3, data.size());
    TEST_ASSERT_EQUAL_HEX8(31, data[0]);
    TEST_ASSERT_EQUAL_HEX8(12, data[1]);
    TEST_ASSERT_EQUAL_HEX8(99, data[2]);
}

void test_Dpt11_EncodeInvalidDay(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt11::encode(0, 6, 25, out); }).isError());   // Day 0 invalid
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt11::encode(32, 6, 25, out); }).isError());  // Day 32 invalid
}

void test_Dpt11_EncodeInvalidMonth(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt11::encode(15, 0, 25, out); }).isError());   // Month 0 invalid
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt11::encode(15, 13, 25, out); }).isError());  // Month 13 invalid
}

void test_Dpt11_EncodeInvalidYear(void) {
    TEST_ASSERT_TRUE(encodeResult([](std::span<uint8_t> out) { return Dpt11::encode(15, 6, 100, out); }).isError());  // Year 100 invalid
}

void test_Dpt11_DecodeEpoch(void) {
    Dpt11::Value date;
    std::vector<uint8_t> data = {0x01, 0x01, 0x00};
    
    TEST_ASSERT_TRUE(Dpt11::decode(data, date).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, date.day);
    TEST_ASSERT_EQUAL_UINT8(1, date.month);
    TEST_ASSERT_EQUAL_UINT8(0, date.year);  // Year 0 = 2000
}

void test_Dpt11_DecodeMidYear(void) {
    Dpt11::Value date;
    std::vector<uint8_t> data = {15, 6, 25};
    
    TEST_ASSERT_TRUE(Dpt11::decode(data, date).isOk());
    TEST_ASSERT_EQUAL_UINT8(15, date.day);
    TEST_ASSERT_EQUAL_UINT8(6, date.month);
    TEST_ASSERT_EQUAL_UINT8(25, date.year);
}

void test_Dpt11_DecodeInsufficientData(void) {
    Dpt11::Value date;
    TEST_ASSERT_TRUE(Dpt11::decode(std::span<const uint8_t>{}, date).isError());
    TEST_ASSERT_TRUE(Dpt11::decode(std::to_array<uint8_t>({0x01}), date).isError());
    TEST_ASSERT_TRUE(Dpt11::decode(std::to_array<uint8_t>({0x01, 0x01}), date).isError());
}

void test_Dpt11_RoundTrip(void) {
    Dpt11::Value original{28, 2, 50};  // 2050-02-28
    Dpt11::Value decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt11::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt11::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT8(original.day, decoded.day);
    TEST_ASSERT_EQUAL_UINT8(original.month, decoded.month);
    TEST_ASSERT_EQUAL_UINT8(original.year, decoded.year);
}

// ============================
// DPT 19 - Date Time
// ============================

void test_Dpt19_EncodeBasicDateTime(void) {
    Dpt19::Value dt;
    dt.year = 2025;
    dt.month = 12;
    dt.day = 31;
    dt.dayOfWeek = 3;  // Wednesday
    dt.hour = 15;
    dt.minute = 30;
    dt.second = 45;
    dt.fault = false;
    dt.workingDay = true;
    dt.noWD = false;
    dt.noYear = false;
    dt.noDate = false;
    dt.noDayOfWeek = false;
    dt.noTime = false;
    dt.suti = false;  // Winter time
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); });
    TEST_ASSERT_EQUAL_UINT(8, data.size());
    TEST_ASSERT_EQUAL_HEX8(125, data[0]);  // 2025 - 1900 = 125
    TEST_ASSERT_EQUAL_HEX8(12, data[1]);   // Month
    TEST_ASSERT_EQUAL_HEX8(31, data[2]);   // Day
}

void test_Dpt19_EncodeEpochStart(void) {
    Dpt19::Value dt;
    dt.year = 1900;
    dt.month = 1;
    dt.day = 1;
    dt.dayOfWeek = 1;  // Monday
    dt.hour = 0;
    dt.minute = 0;
    dt.second = 0;
    dt.fault = false;
    dt.workingDay = false;
    dt.noWD = true;
    dt.noYear = false;
    dt.noDate = false;
    dt.noDayOfWeek = false;
    dt.noTime = false;
    dt.suti = false;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); });
    TEST_ASSERT_EQUAL_UINT(8, data.size());
    TEST_ASSERT_EQUAL_HEX8(0, data[0]);  // 1900 - 1900 = 0
}

void test_Dpt19_EncodeEpochEnd(void) {
    Dpt19::Value dt;
    dt.year = 2155;
    dt.month = 12;
    dt.day = 31;
    dt.dayOfWeek = 0;  // No day
    dt.hour = 23;
    dt.minute = 59;
    dt.second = 59;
    dt.fault = false;
    dt.workingDay = false;
    dt.noWD = true;
    dt.noYear = false;
    dt.noDate = false;
    dt.noDayOfWeek = true;
    dt.noTime = false;
    dt.suti = true;  // Summer time
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); });
    TEST_ASSERT_EQUAL_UINT(8, data.size());
    TEST_ASSERT_EQUAL_HEX8(255, data[0]);  // 2155 - 1900 = 255
}

void test_Dpt19_EncodeWithFaultFlag(void) {
    Dpt19::Value dt;
    dt.year = 2025;
    dt.month = 6;
    dt.day = 15;
    dt.dayOfWeek = 0;
    dt.hour = 12;
    dt.minute = 0;
    dt.second = 0;
    dt.fault = true;  // Fault flag set
    dt.workingDay = false;
    dt.noWD = false;
    dt.noYear = false;
    dt.noDate = false;
    dt.noDayOfWeek = false;
    dt.noTime = false;
    dt.suti = false;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); });
    TEST_ASSERT_EQUAL_UINT(8, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x80, data[6] & 0x80);  // Fault bit set
}

void test_Dpt19_EncodeInvalidYear(void) {
    Dpt19::Value dt;
    dt.year = 1899;  // Too early
    dt.month = 6;
    dt.day = 15;
    dt.dayOfWeek = 1;
    dt.hour = 12;
    dt.minute = 0;
    dt.second = 0;
    dt.fault = false;
    dt.workingDay = false;
    dt.noWD = false;
    dt.noYear = false;
    dt.noDate = false;
    dt.noDayOfWeek = false;
    dt.noTime = false;
    dt.suti = false;
    
    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); }).isError());
    
    dt.year = 2156;  // Too late
    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); }).isError());
}

void test_Dpt19_DecodeBasicDateTime(void) {
    Dpt19::Value dt;
    // 2025-12-31 Wednesday 15:30:45, working day
    std::vector<uint8_t> data = {125, 12, 31, 0x6F, 0x1E, 0x2D, 0x40, 0x00};

    TEST_ASSERT_TRUE(Dpt19::decode(data, dt).isOk());
    TEST_ASSERT_EQUAL_UINT16(2025, dt.year);
    TEST_ASSERT_EQUAL_UINT8(12, dt.month);
    TEST_ASSERT_EQUAL_UINT8(31, dt.day);
    TEST_ASSERT_EQUAL_UINT8(3, dt.dayOfWeek);
    TEST_ASSERT_EQUAL_UINT8(15, dt.hour);
    TEST_ASSERT_EQUAL_UINT8(30, dt.minute);
    TEST_ASSERT_EQUAL_UINT8(45, dt.second);
    TEST_ASSERT_TRUE(dt.workingDay);
}

// ---------------------------------------------------------------------------
// Golden bytes built from the octet layouts of 03_07_02 (§3.11 N3U5r2U6r2U6,
// §3.12 r3U5r4U4r1U7 with the §1.4 Figure 2 frame, §3.20 DPT_DateTime).
// ---------------------------------------------------------------------------

void test_Dpt10_GoldenBytes(void) {
    // Wednesday 15:30:45 -> [011 01111] [00 011110] [00 101101]
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt10::encode(3, 15, 30, 45, out); });
    TEST_ASSERT_EQUAL_UINT(3, data.size());
    TEST_ASSERT_EQUAL_HEX8(0x6F, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x1E, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x2D, data[2]);

    // Sunday 23:59:59 -> [111 10111] [00 111011] [00 111011]
    data = encodePayload([](std::span<uint8_t> out) { return Dpt10::encode(7, 23, 59, 59, out); });
    TEST_ASSERT_EQUAL_HEX8(0xF7, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x3B, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x3B, data[2]);

    Dpt10::Value time;
    TEST_ASSERT_TRUE(Dpt10::decode(std::to_array<uint8_t>({0xF7, 0x3B, 0x3B}), time).isOk());
    TEST_ASSERT_EQUAL_UINT8(7, time.dayOfWeek);
    TEST_ASSERT_EQUAL_UINT8(23, time.hour);
    TEST_ASSERT_EQUAL_UINT8(59, time.minute);
    TEST_ASSERT_EQUAL_UINT8(59, time.second);
}

void test_Dpt10_DecodeIgnoresReservedBits(void) {
    // The r2 bits above minutes and seconds are not part of the value.
    Dpt10::Value time;
    TEST_ASSERT_TRUE(Dpt10::decode(std::to_array<uint8_t>({0x2C, 0xDE, 0xED}), time).isOk());
    TEST_ASSERT_EQUAL_UINT8(1, time.dayOfWeek);
    TEST_ASSERT_EQUAL_UINT8(12, time.hour);
    TEST_ASSERT_EQUAL_UINT8(30, time.minute);
    TEST_ASSERT_EQUAL_UINT8(45, time.second);
}

void test_Dpt10_DecodeRejectsOutOfRange(void) {
    Dpt10::Value time;
    TEST_ASSERT_TRUE(Dpt10::decode(std::to_array<uint8_t>({0x18, 0x00, 0x00}), time).isError());  // hour 24
    TEST_ASSERT_TRUE(Dpt10::decode(std::to_array<uint8_t>({0x00, 0x3C, 0x00}), time).isError());  // minute 60
    TEST_ASSERT_TRUE(Dpt10::decode(std::to_array<uint8_t>({0x00, 0x00, 0x3C}), time).isError());  // second 60
}

void test_Dpt11_GoldenBytesFigure2(void) {
    // §1.4 Figure 2: octets 8..10 = 00010010 00001100 00000110 (18, 12, 2006).
    auto data = encodePayload([](std::span<uint8_t> out) { return Dpt11::encode(18, 12, 6, out); });
    TEST_ASSERT_EQUAL_HEX8(0x12, data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0C, data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x06, data[2]);

    Dpt11::Value date;
    TEST_ASSERT_TRUE(Dpt11::decode(data, date).isOk());
    TEST_ASSERT_EQUAL_UINT16(2006, date.fullYear());
}

void test_Dpt11_CenturyRule(void) {
    // §3.12 EXAMPLE 5: 99 = 1999, 0 = 2000, 4 = 2004; >= 90 is the 20th century.
    TEST_ASSERT_EQUAL_UINT16(1999, (Dpt11Value{1, 1, 99}.fullYear()));
    TEST_ASSERT_EQUAL_UINT16(1990, (Dpt11Value{1, 1, 90}.fullYear()));
    TEST_ASSERT_EQUAL_UINT16(2089, (Dpt11Value{1, 1, 89}.fullYear()));
    TEST_ASSERT_EQUAL_UINT16(2000, (Dpt11Value{1, 1, 0}.fullYear()));
    TEST_ASSERT_EQUAL_UINT16(2004, (Dpt11Value{1, 1, 4}.fullYear()));
}

void test_Dpt19_GoldenBytes(void) {
    Dpt19::Value dt;
    dt.year = 2025;
    dt.month = 12;
    dt.day = 31;
    dt.dayOfWeek = 3;
    dt.hour = 15;
    dt.minute = 30;
    dt.second = 45;
    dt.workingDay = true;
    dt.suti = true;
    dt.externalSync = true;
    dt.reliableSource = true;

    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); });
    const std::array<uint8_t, 8> expected{125, 12, 31, 0x6F, 0x1E, 0x2D, 0x41, 0xC0};
    TEST_ASSERT_EQUAL_UINT(8, data.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), data.data(), 8);

    Dpt19::Value decoded;
    TEST_ASSERT_TRUE(Dpt19::decode(expected, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT8(15, decoded.hour);
    TEST_ASSERT_EQUAL_UINT8(30, decoded.minute);
    TEST_ASSERT_TRUE(decoded.suti);
    TEST_ASSERT_TRUE(decoded.externalSync);
    TEST_ASSERT_TRUE(decoded.reliableSource);
}

void test_Dpt19_EveryFlagHasItsOwnBit(void) {
    struct FlagCase {
        bool Dpt19Value::*flag;
        uint8_t octet;
        uint8_t mask;
    };
    const FlagCase cases[] = {
        {&Dpt19Value::fault, 6, 0x80},       {&Dpt19Value::workingDay, 6, 0x40},
        {&Dpt19Value::noWD, 6, 0x20},        {&Dpt19Value::noYear, 6, 0x10},
        {&Dpt19Value::noDate, 6, 0x08},      {&Dpt19Value::noDayOfWeek, 6, 0x04},
        {&Dpt19Value::noTime, 6, 0x02},      {&Dpt19Value::suti, 6, 0x01},
        {&Dpt19Value::externalSync, 7, 0x80}, {&Dpt19Value::reliableSource, 7, 0x40},
    };
    for (const auto& c : cases) {
        Dpt19::Value dt;
        dt.year = 2000;
        dt.*(c.flag) = true;
        auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); });
        TEST_ASSERT_EQUAL_HEX8(c.octet == 6 ? c.mask : 0x00, data[6]);
        TEST_ASSERT_EQUAL_HEX8(c.octet == 7 ? c.mask : 0x00, data[7]);
        Dpt19::Value decoded;
        TEST_ASSERT_TRUE(Dpt19::decode(data, decoded).isOk());
        TEST_ASSERT_TRUE(decoded.*(c.flag));
    }
}

void test_Dpt19_Hour24OnlyAsEndOfDay(void) {
    // Note 11: 24:00:00 is valid; hour 24 with non-zero minutes or seconds is
    // invalid and has to be ignored by the receiver.
    Dpt19::Value dt;
    dt.year = 2026;
    dt.hour = 24;
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); });
    TEST_ASSERT_EQUAL_HEX8(0x18, data[3]);
    Dpt19::Value decoded;
    TEST_ASSERT_TRUE(Dpt19::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT8(24, decoded.hour);

    dt.minute = 1;
    TEST_ASSERT_TRUE(encodeResult([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); }).isError());
    TEST_ASSERT_TRUE(Dpt19::decode(std::to_array<uint8_t>({126, 1, 1, 0x18, 0x01, 0x00, 0x00, 0x00}), decoded).isError());
    TEST_ASSERT_TRUE(Dpt19::decode(std::to_array<uint8_t>({126, 1, 1, 0x18, 0x00, 0x01, 0x00, 0x00}), decoded).isError());
    TEST_ASSERT_TRUE(Dpt19::decode(std::to_array<uint8_t>({126, 1, 1, 0x19, 0x00, 0x00, 0x00, 0x00}), decoded).isError());
}

void test_Dpt19_ReservedBitsOfLastOctetRejected(void) {
    // Note 15: "Receivers shall check these bits to be 0."
    Dpt19::Value dt;
    for (uint8_t bit = 0; bit < 6; ++bit) {
        const auto raw = std::to_array<uint8_t>({126, 1, 1, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(1u << bit)});
        TEST_ASSERT_TRUE(Dpt19::decode(raw, dt).isError());
    }
}

void test_Dpt19_InvalidFieldsAreZeroAndUnchecked(void) {
    Dpt19::Value dt;
    dt.noYear = true;
    dt.year = 0;           // out of range, but flagged not valid
    dt.noDate = true;
    dt.month = 0;
    dt.day = 0;
    dt.noTime = true;
    dt.hour = 99;
    dt.noDayOfWeek = true;
    dt.dayOfWeek = 5;
    dt.noWD = true;
    dt.workingDay = true;
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt19::encode(dt, out); });
    const std::array<uint8_t, 8> expected{0, 0, 0, 0x00, 0x00, 0x00, 0x3E, 0x00};
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), data.data(), 8);

    // A receiver does not range-check a field flagged not valid.
    Dpt19::Value decoded;
    TEST_ASSERT_TRUE(Dpt19::decode(std::to_array<uint8_t>({126, 0, 0, 0x1F, 0x3F, 0x3F, 0x0A, 0x00}), decoded).isOk());
    TEST_ASSERT_TRUE(decoded.noDate);
    TEST_ASSERT_TRUE(decoded.noTime);
    // ...but does check one that is flagged valid.
    TEST_ASSERT_TRUE(Dpt19::decode(std::to_array<uint8_t>({126, 0, 1, 0x00, 0x00, 0x00, 0x00, 0x00}), decoded).isError());
}

void test_Dpt19_DecodeFlagsCorrectly(void) {
    Dpt19::Value dt;
    // Flags byte with fault=1, working_day=0, noWD=1, etc.
    std::vector<uint8_t> data = {125, 6, 15, 0x60, 0x00, 0x00, 0xA0, 0x00};
    
    TEST_ASSERT_TRUE(Dpt19::decode(data, dt).isOk());
    TEST_ASSERT_TRUE(dt.fault);
    TEST_ASSERT_FALSE(dt.workingDay);
    TEST_ASSERT_TRUE(dt.noWD);
}

void test_Dpt19_DecodeInsufficientData(void) {
    Dpt19::Value dt;
    TEST_ASSERT_TRUE(Dpt19::decode(std::span<const uint8_t>{}, dt).isError());
    TEST_ASSERT_TRUE(Dpt19::decode(std::to_array<uint8_t>({125, 12, 31}), dt).isError());
    TEST_ASSERT_TRUE(Dpt19::decode(std::to_array<uint8_t>({125, 12, 31, 0x63, 0xDE, 0x2D, 0x40}), dt).isError());
}

void test_Dpt19_RoundTrip(void) {
    Dpt19::Value original;
    original.year = 2050;
    original.month = 7;
    original.day = 20;
    original.dayOfWeek = 4;
    original.hour = 9;
    original.minute = 15;
    original.second = 30;
    original.fault = false;
    original.workingDay = true;
    original.noWD = false;
    original.noYear = false;
    original.noDate = false;
    original.noDayOfWeek = false;
    original.noTime = false;
    original.suti = true;
    
    Dpt19::Value decoded;
    
    auto data = encodePayload([&](std::span<uint8_t> out) { return Dpt19::encode(original, out); });
    TEST_ASSERT_TRUE(Dpt19::decode(data, decoded).isOk());
    TEST_ASSERT_EQUAL_UINT16(original.year, decoded.year);
    TEST_ASSERT_EQUAL_UINT8(original.month, decoded.month);
    TEST_ASSERT_EQUAL_UINT8(original.day, decoded.day);
    TEST_ASSERT_EQUAL_UINT8(original.dayOfWeek, decoded.dayOfWeek);
    TEST_ASSERT_EQUAL_UINT8(original.hour, decoded.hour);
    TEST_ASSERT_EQUAL_UINT8(original.minute, decoded.minute);
    TEST_ASSERT_EQUAL_UINT8(original.second, decoded.second);
    TEST_ASSERT_EQUAL(original.fault, decoded.fault);
    TEST_ASSERT_EQUAL(original.workingDay, decoded.workingDay);
    TEST_ASSERT_EQUAL(original.suti, decoded.suti);
}

int main(void) {
    UNITY_BEGIN();
    
    // DPT 10 tests
    RUN_TEST(test_Dpt10_EncodeMidnight);
    RUN_TEST(test_Dpt10_EncodeNoon);
    RUN_TEST(test_Dpt10_EncodeAfternoonTime);
    RUN_TEST(test_Dpt10_EncodeEndOfDay);
    RUN_TEST(test_Dpt10_EncodeInvalidHour);
    RUN_TEST(test_Dpt10_EncodeInvalidMinute);
    RUN_TEST(test_Dpt10_EncodeInvalidSecond);
    RUN_TEST(test_Dpt10_EncodeInvalidDayOfWeek);
    RUN_TEST(test_Dpt10_DecodeMidnight);
    RUN_TEST(test_Dpt10_DecodeNoon);
    RUN_TEST(test_Dpt10_DecodeInsufficientData);
    RUN_TEST(test_Dpt10_RoundTrip);
    
    // DPT 11 tests
    RUN_TEST(test_Dpt11_EncodeEpoch);
    RUN_TEST(test_Dpt11_EncodeMidYear);
    RUN_TEST(test_Dpt11_EncodeEndOfCentury);
    RUN_TEST(test_Dpt11_EncodeInvalidDay);
    RUN_TEST(test_Dpt11_EncodeInvalidMonth);
    RUN_TEST(test_Dpt11_EncodeInvalidYear);
    RUN_TEST(test_Dpt11_DecodeEpoch);
    RUN_TEST(test_Dpt11_DecodeMidYear);
    RUN_TEST(test_Dpt11_DecodeInsufficientData);
    RUN_TEST(test_Dpt11_RoundTrip);
    
    // DPT 19 tests
    RUN_TEST(test_Dpt19_EncodeBasicDateTime);
    RUN_TEST(test_Dpt19_EncodeEpochStart);
    RUN_TEST(test_Dpt19_EncodeEpochEnd);
    RUN_TEST(test_Dpt19_EncodeWithFaultFlag);
    RUN_TEST(test_Dpt19_EncodeInvalidYear);
    RUN_TEST(test_Dpt19_DecodeBasicDateTime);
    RUN_TEST(test_Dpt19_DecodeFlagsCorrectly);
    RUN_TEST(test_Dpt19_DecodeInsufficientData);
    RUN_TEST(test_Dpt19_RoundTrip);

    // Spec layouts
    RUN_TEST(test_Dpt10_GoldenBytes);
    RUN_TEST(test_Dpt10_DecodeIgnoresReservedBits);
    RUN_TEST(test_Dpt10_DecodeRejectsOutOfRange);
    RUN_TEST(test_Dpt11_GoldenBytesFigure2);
    RUN_TEST(test_Dpt11_CenturyRule);
    RUN_TEST(test_Dpt19_GoldenBytes);
    RUN_TEST(test_Dpt19_EveryFlagHasItsOwnBit);
    RUN_TEST(test_Dpt19_Hour24OnlyAsEndOfDay);
    RUN_TEST(test_Dpt19_ReservedBitsOfLastOctetRejected);
    RUN_TEST(test_Dpt19_InvalidFieldsAreZeroAndUnchecked);

    return UNITY_END();
}
