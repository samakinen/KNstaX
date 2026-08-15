// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file dpt.cpp
 * @brief DPT implementation
 */

#include "knx/application/dpt.hpp"
#include "knx/constants.hpp"
#include "knx/util/bit_ops.hpp"
#include <cmath>
#include <span>

using namespace knx::constants::dpt;
namespace bits = knx::util;

namespace knx {
namespace application {

namespace {

template <typename Trait>
util::Result<size_t> encodeDynamicWithTrait(const DptValue& value, std::span<uint8_t> data)
{
    return Trait::encodeDynamic(value, data);
}

template <typename Trait>
util::Result<DptValue> decodeDynamicWithTrait(std::span<const uint8_t> data)
{
    return Trait::decodeDynamic(data);
}

template <DptValue::Type ValueType, typename ValueT>
DptValue makeTypedDptValue(ValueT value)
{
    if constexpr (std::is_same_v<ValueT, float>) {
        return DptValue(value, ValueType);
    } else {
        return DptValue(value);
    }
}

util::Result<void> ensureCapacity(std::span<uint8_t> out, size_t needed)
{
    if (out.size() < needed) {
        return util::ErrorCode::BufferTooSmall;
    }
    return util::Result<void>::ok();
}

template <size_t Extent>
util::Result<size_t> writeArray(const std::array<uint8_t, Extent>& bytes, std::span<uint8_t> out)
{
    auto capacityResult = ensureCapacity(out, Extent);
    if (capacityResult.isError()) {
        return capacityResult.error();
    }

    std::copy(bytes.begin(), bytes.end(), out.begin());
    return Extent;
}

util::Result<size_t> writeWord(uint16_t value, std::span<uint8_t> out)
{
    auto capacityResult = ensureCapacity(out, 2u);
    if (capacityResult.isError()) {
        return capacityResult.error();
    }

    bits::storeWordBE(std::span<uint8_t, 2>(out.data(), 2), value);
    return 2u;
}

util::Result<size_t> writeDword(uint32_t value, std::span<uint8_t> out)
{
    auto capacityResult = ensureCapacity(out, 4u);
    if (capacityResult.isError()) {
        return capacityResult.error();
    }

    bits::storeDwordBE(std::span<uint8_t, 4>(out.data(), 4), value);
    return 4u;
}

// The catalog is metadata only.  It deliberately holds no per-entry function
// pointers: a table of them keeps every codec instantiation reachable, so the
// linker cannot drop a single one and a device that speaks four datapoint types
// pays for all seventy-six.  Encode/decode instead dispatch on DptValue::Type
// (see codecFor* below), which maps one-to-one onto the codecs and collapses
// ~150 template instantiations into 24 shared ones.
#define KNX_DPT_CATALOG_ENTRY(symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
    {dptids::symbol, KNX_DPT_NAME(shortName), KNX_DPT_TEXT(description), \
     DptTraits<dpttags::symbol>::kValueType, true},
const DptInfo kKnownDpts[] = {
#include "knx/application/dpt_catalog.inc"
};
#undef KNX_DPT_CATALOG_ENTRY

/**
 * @brief Dispatch dynamic encode/decode on the value type.
 *
 * Every catalog entry sharing a DptValue::Type shares its codec (verified
 * across the whole catalog), so one representative tag per value type covers
 * all of them.  X-macro so encode and decode cannot drift apart.
 */
#define KNX_DPT_CODEC_DISPATCH(X)                       \
    X(Boolean,         Bool)                            \
    X(Controlled1Bit,  Controlled1Bit)                  \
    X(Controlled3Bit,  Controlled3Bit)                  \
    X(Character,       Character)                       \
    X(Unsigned8,       Unsigned8)                       \
    X(Signed8,         Signed8)                         \
    X(Unsigned16,      Unsigned16)                      \
    X(Signed16,        Signed16)                        \
    X(Float2Byte,      Float2Byte)                      \
    X(TimeOfDay,       TimeOfDay)                       \
    X(Date,            Date)                            \
    X(Unsigned32,      Unsigned32)                      \
    X(Signed32,        Signed32)                        \
    X(Float4Byte,      Float4Byte)                      \
    X(AccessControl,   AccessControl)                   \
    X(String,          String)                          \
    X(SceneNumber,     SceneNumber)                     \
    X(SceneControl,    SceneControl)                    \
    X(DateTime,        DateTime)                        \
    X(HvacMode,        HvacMode)                        \
    X(RgbColor,        RgbColor)                        \
    X(XyYColor,        XyYColor)                        \
    X(HsvColor,        HsvColor)                        \
    X(ColorTransition, ColorTransition)

util::Result<size_t> encodeForValueType(DptValue::Type type,
                                        const DptValue& value,
                                        std::span<uint8_t> data)
{
    switch (type) {
#define KNX_DPT_ENCODE_CASE(valueType, tag)                                   \
        case DptValue::Type::valueType:                                       \
            return DptTraits<dpttags::tag>::encodeDynamic(value, data);
        KNX_DPT_CODEC_DISPATCH(KNX_DPT_ENCODE_CASE)
#undef KNX_DPT_ENCODE_CASE
        case DptValue::Type::Unsupported:
            break;
    }
    return util::ErrorCode::OperationNotSupported;
}

util::Result<DptValue> decodeForValueType(DptValue::Type type, std::span<const uint8_t> data)
{
    switch (type) {
#define KNX_DPT_DECODE_CASE(valueType, tag)                                   \
        case DptValue::Type::valueType:                                       \
            return DptTraits<dpttags::tag>::decodeDynamic(data);
        KNX_DPT_CODEC_DISPATCH(KNX_DPT_DECODE_CASE)
#undef KNX_DPT_DECODE_CASE
        case DptValue::Type::Unsupported:
            break;
    }
    return util::ErrorCode::OperationNotSupported;
}

} // namespace

const DptInfo* dptRegistryEntries()
{
    return kKnownDpts;
}

size_t dptRegistrySize()
{
    return sizeof(kKnownDpts) / sizeof(kKnownDpts[0]);
}

const DptInfo* lookupDptInfo(DptId id)
{
    for (const auto& info : kKnownDpts) {
        if (info.id == id) {
            return &info;
        }
    }

    if (id.sub != 0) {
        const DptId fallback{id.main, 0};
        for (const auto& info : kKnownDpts) {
            if (info.id == fallback) {
                return &info;
            }
        }
    }

    return nullptr;
}

bool supportsDpt(DptId id)
{
    const auto* info = lookupDptInfo(id);
    return info && info->runtimeSupported;
}

bool dptValueMatches(DptId id, const DptValue& value)
{
    const auto* info = lookupDptInfo(id);
    return info && info->valueType == value.type();
}

util::Result<size_t> encodeDptValue(DptId id, const DptValue& value, std::span<uint8_t> data) {
    const auto* info = lookupDptInfo(id);
    if (!info || !info->runtimeSupported) {
        return util::ErrorCode::OperationNotSupported;
    }

    if (info->valueType != value.type()) {
        return util::ErrorCode::OperationNotSupported;
    }

    return encodeForValueType(info->valueType, value, data);
}

util::Result<DptValue> decodeDptValue(DptId id, std::span<const uint8_t> data) {
    const auto* info = lookupDptInfo(id);
    if (!info || !info->runtimeSupported) {
        return util::ErrorCode::OperationNotSupported;
    }

    return decodeForValueType(info->valueType, data);
}

bool dptValueAsScalar(const DptValue& value, double& out) {
    switch (value.type()) {
        case DptValue::Type::Boolean:     out = value.asBool() ? 1.0 : 0.0;             return true;
        case DptValue::Type::Character:   out = static_cast<double>(value.asChar());    return true;
        case DptValue::Type::Unsigned8:   out = static_cast<double>(value.asUInt8());   return true;
        case DptValue::Type::Signed8:     out = static_cast<double>(value.asInt8());    return true;
        case DptValue::Type::Unsigned16:  out = static_cast<double>(value.asUInt16());  return true;
        case DptValue::Type::Signed16:    out = static_cast<double>(value.asInt16());   return true;
        case DptValue::Type::Float2Byte:  out = static_cast<double>(value.asFloat());   return true;
        case DptValue::Type::Unsigned32:  out = static_cast<double>(value.asUInt32());  return true;
        case DptValue::Type::Signed32:    out = static_cast<double>(value.asInt32());   return true;
        case DptValue::Type::Float4Byte:  out = static_cast<double>(value.asFloat());   return true;
        default:
            return false;
    }
}

#define KNX_DPT_CATALOG_ENTRY(symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
util::Result<size_t> DptTraits<dpttags::symbol>::encode(value_type value, std::span<uint8_t> data) \
{ \
    return codecType::encode(value, data); \
} \
\
util::Result<DptTraits<dpttags::symbol>::value_type> \
DptTraits<dpttags::symbol>::decode(std::span<const uint8_t> data) \
{ \
    value_type value{}; \
    auto result = codecType::decode(data, value); \
    if (result.isError()) { \
        return result.error(); \
    } \
    return value; \
} \
\
util::Result<size_t> DptTraits<dpttags::symbol>::encodeDynamic(const DptValue& value, std::span<uint8_t> data) \
{ \
    auto typedValue = dptValueAs<dpttags::symbol>(value); \
    if (typedValue.isError()) { \
        return typedValue.error(); \
    } \
    return encode(typedValue.value(), data); \
} \
\
util::Result<DptValue> DptTraits<dpttags::symbol>::decodeDynamic(std::span<const uint8_t> data) \
{ \
    auto result = decode(data); \
    if (result.isError()) { \
        return result.error(); \
    } \
    return makeTypedDptValue<DptValue::Type::valueType>(result.value()); \
}
#include "knx/application/dpt_catalog.inc"
#undef KNX_DPT_CATALOG_ENTRY

// DPT 1 - Boolean (1 bit in LSB of first byte)
util::Result<size_t> Dpt1::encode(bool value, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 1>{static_cast<uint8_t>(value ? DPT1_TRUE : DPT1_FALSE)}, data);
}

util::Result<void> Dpt1::decode(std::span<const uint8_t> data, bool& value) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    value = (data[0] & DPT1_MASK) != 0;
    return util::Result<void>::ok();
}

// DPT 5 - Unsigned integer 8-bit
util::Result<size_t> Dpt5::encode(uint8_t value, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 1>{value}, data);
}

util::Result<void> Dpt5::decode(std::span<const uint8_t> data, uint8_t& value) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    value = data[0];
    return util::Result<void>::ok();
}

// DPT 9 - Float (2 bytes)
// Format: SEEE EMMM MMMM MMMM
// S = sign bit (bit 15), E = exponent (bits 14-11), M = signed 11-bit mantissa (bits 10-0)
// Value = (0.01 * M) * 2^E
util::Result<size_t> Dpt9::encode(float value, std::span<uint8_t> data) {
    if (std::isnan(value) || std::isinf(value)) {
        return util::ErrorCode::InvalidParameter;
    }

    // Reject values outside representable DPT9 range.
    if (value < DPT9_MIN_VALUE || value > DPT9_MAX_VALUE) {
        return util::ErrorCode::InvalidParameter;
    }

    // Scale to the signed 11-bit mantissa resolution.
    int32_t mantissa = static_cast<int32_t>(std::lround(static_cast<double>(value) / DPT9_SCALE_FACTOR));
    uint8_t exponent = 0;

    // Normalize mantissa to the signed 11-bit range using round-to-nearest.
    while ((mantissa < DPT9_MIN_MANTISSA || mantissa > DPT9_MAX_MANTISSA) && exponent < DPT9_MAX_EXPONENT) {
        mantissa = static_cast<int32_t>(std::lround(static_cast<double>(mantissa) / 2.0));
        ++exponent;
    }

    // Still not representable.
    if (mantissa < DPT9_MIN_MANTISSA || mantissa > DPT9_MAX_MANTISSA) {
        return util::ErrorCode::InvalidParameter;
    }

    const uint16_t encodedMantissa = static_cast<uint16_t>(mantissa) & DPT9_MANTISSA_MASK;
    const uint16_t raw = static_cast<uint16_t>(
        ((mantissa < 0) ? DPT9_SIGN_MASK : 0u) |
        (static_cast<uint16_t>(exponent & 0x0F) << 11) |
        encodedMantissa);
    return writeWord(raw, data);
}

util::Result<void> Dpt9::decode(std::span<const uint8_t> data, float& value) {
    if (data.size() < 2) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    const uint16_t raw = bits::makeWord(data[0], data[1]);
    const int exponent = (raw >> 11) & 0x0F;
    int mantissa = raw & DPT9_MANTISSA_MASK;
    if ((raw & DPT9_SIGN_MASK) != 0u) {
        mantissa -= (1 << DPT9_MANTISSA_BITS);
    }

    value = std::ldexp(static_cast<float>(mantissa) * DPT9_SCALE_FACTOR, exponent);
    return util::Result<void>::ok();
}

// DPT 2 - 1-bit controlled (2 bits: control + value)
util::Result<size_t> Dpt2::encode(bool control, bool value, std::span<uint8_t> data) {
    uint8_t byte = ((control ? 1 : 0) << 1) | (value ? 1 : 0);
    return writeArray(std::array<uint8_t, 1>{byte}, data);
}

util::Result<size_t> Dpt2::encode(const Value& val, std::span<uint8_t> data) {
    return encode(val.control, val.value, data);
}

util::Result<void> Dpt2::decode(std::span<const uint8_t> data, Value& value) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    value.control = (data[0] & DPT2_CONTROL_MASK) != 0;
    value.value = (data[0] & DPT2_VALUE_MASK) != 0;
    return util::Result<void>::ok();
}

// DPT 3 - 3-bit controlled (4 bits: control + 3-bit step code)
util::Result<size_t> Dpt3::encode(bool control, uint8_t stepCode, std::span<uint8_t> data) {
    if (stepCode > DPT3_STEPCODE_MASK) {
        return util::ErrorCode::InvalidParameter;
    }
    uint8_t byte = ((control ? 1 : 0) << 3) | (stepCode & DPT3_STEPCODE_MASK);
    return writeArray(std::array<uint8_t, 1>{byte}, data);
}

util::Result<size_t> Dpt3::encode(const Value& val, std::span<uint8_t> data) {
    return encode(val.control, val.stepCode, data);
}

util::Result<void> Dpt3::decode(std::span<const uint8_t> data, Value& value) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    value.control = (data[0] & DPT3_CONTROL_MASK) != 0;
    value.stepCode = data[0] & DPT3_STEPCODE_MASK;
    return util::Result<void>::ok();
}

// DPT 4 - Character (ASCII 8-bit)
util::Result<size_t> Dpt4::encode(char value, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 1>{static_cast<uint8_t>(value)}, data);
}

util::Result<void> Dpt4::decode(std::span<const uint8_t> data, char& value) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    value = static_cast<char>(data[0]);
    return util::Result<void>::ok();
}

// DPT 6 - 8-bit signed (-128 to 127)
util::Result<size_t> Dpt6::encode(int8_t value, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 1>{static_cast<uint8_t>(value)}, data);
}

util::Result<void> Dpt6::decode(std::span<const uint8_t> data, int8_t& value) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    value = static_cast<int8_t>(data[0]);
    return util::Result<void>::ok();
}

// DPT 7 - 16-bit unsigned (big-endian)
util::Result<size_t> Dpt7::encode(uint16_t value, std::span<uint8_t> data) {
    return writeWord(value, data);
}

util::Result<void> Dpt7::decode(std::span<const uint8_t> data, uint16_t& value) {
    if (data.size() < 2) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    value = bits::makeWord(data[0], data[1]);
    return util::Result<void>::ok();
}

// DPT 8 - 16-bit signed (big-endian)
util::Result<size_t> Dpt8::encode(int16_t value, std::span<uint8_t> data) {
    return writeWord(static_cast<uint16_t>(value), data);
}

util::Result<void> Dpt8::decode(std::span<const uint8_t> data, int16_t& value) {
    if (data.size() < 2) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    uint16_t unsigned_val = bits::makeWord(data[0], data[1]);
    value = static_cast<int16_t>(unsigned_val);
    return util::Result<void>::ok();
}

// DPT 10 - Time of Day
util::Result<size_t> Dpt10::encode(const Value& time, std::span<uint8_t> data) {
    return encode(time.dayOfWeek, time.hour, time.minute, time.second, data);
}

util::Result<size_t> Dpt10::encode(uint8_t dayOfWeek, uint8_t hour, uint8_t minute, uint8_t second, std::span<uint8_t> data) {
    // Validate ranges
    if (dayOfWeek > DPT10_MAX_DAY || hour > DPT10_MAX_HOUR || minute > DPT10_MAX_MINUTE || second > DPT10_MAX_SECOND) {
        return util::ErrorCode::InvalidParameter;
    }
    
    // Byte 0: [D D D H H H H H] - Day (3 bits) + Hour (5 bits)
    uint8_t byte0 = ((dayOfWeek & 0x07) << 5) | (hour & DPT10_HOUR_MASK);
    
    // Byte 1: [M M M M M M S S] - Minutes (6 bits) + Seconds high (2 bits)
    uint8_t byte1 = ((minute & DPT10_MINUTE_MASK) << 2) | ((second >> 4) & 0x03);
    
    // Byte 2: [S S S S 0 0 0 0] - Seconds low (4 bits)
    uint8_t byte2 = (second & DPT10_SECOND_MASK) << 4;

    return writeArray(std::array<uint8_t, 3>{byte0, byte1, byte2}, data);
}

util::Result<void> Dpt10::decode(std::span<const uint8_t> data, Value& time) {
    if (data.size() < 3) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    // Extract day of week and hour
    time.dayOfWeek = (data[0] >> 5) & 0x07;
    time.hour = data[0] & DPT10_HOUR_MASK;
    
    // Extract minutes and seconds
    time.minute = (data[1] >> 2) & DPT10_MINUTE_MASK;
    time.second = ((data[1] & 0x03) << 4) | ((data[2] >> 4) & DPT10_SECOND_MASK);
    
    // Validate ranges
    if (time.dayOfWeek > DPT10_MAX_DAY || time.hour > DPT10_MAX_HOUR || time.minute > DPT10_MAX_MINUTE || time.second > DPT10_MAX_SECOND) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    return util::Result<void>::ok();
}

// DPT 11 - Date
util::Result<size_t> Dpt11::encode(const Value& date, std::span<uint8_t> data) {
    return encode(date.day, date.month, date.year, data);
}

util::Result<size_t> Dpt11::encode(uint8_t day, uint8_t month, uint8_t year, std::span<uint8_t> data) {
    // Validate ranges
    if (day < DPT11_MIN_DAY || day > DPT11_MAX_DAY || month < DPT11_MIN_MONTH || month > DPT11_MAX_MONTH || year > DPT11_MAX_YEAR) {
        return util::ErrorCode::InvalidParameter;
    }

    return writeArray(std::array<uint8_t, 3>{
                          static_cast<uint8_t>(day & DPT11_DAY_MASK),
                          static_cast<uint8_t>(month & DPT11_MONTH_MASK),
                          static_cast<uint8_t>(year & DPT11_YEAR_MASK)},
                      data);
}

util::Result<void> Dpt11::decode(std::span<const uint8_t> data, Value& date) {
    if (data.size() < 3) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    date.day = data[0] & DPT11_DAY_MASK;
    date.month = data[1] & DPT11_MONTH_MASK;
    date.year = data[2] & DPT11_YEAR_MASK;
    
    // Validate ranges
    if (date.day < DPT11_MIN_DAY || date.day > DPT11_MAX_DAY || date.month < DPT11_MIN_MONTH || date.month > DPT11_MAX_MONTH || date.year > DPT11_MAX_YEAR) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    return util::Result<void>::ok();
}

// DPT 19 - Date Time
util::Result<size_t> Dpt19::encode(const Value& datetime, std::span<uint8_t> data) {
    // Validate ranges
    if (datetime.year < 1900 || datetime.year > 2155 ||
        datetime.month < 1 || datetime.month > 12 ||
        datetime.day < 1 || datetime.day > 31 ||
        datetime.dayOfWeek > 7 ||
        datetime.hour > 23 || datetime.minute > 59 || datetime.second > 59) {
        return util::ErrorCode::InvalidParameter;
    }
    
    uint8_t year_offset = static_cast<uint8_t>(datetime.year - 1900);

    uint8_t byte3 = ((datetime.dayOfWeek & 0x07) << 5) | ((datetime.hour >> 2) & 0x07);

    uint8_t byte4 = ((datetime.hour & 0x03) << 6) | (datetime.minute & 0x3F);

    uint8_t flags1 = 0;
    if (datetime.fault) flags1 |= 0x80;
    if (datetime.workingDay) flags1 |= 0x40;
    if (datetime.noWD) flags1 |= 0x20;
    if (datetime.noYear) flags1 |= 0x10;
    if (datetime.noDate) flags1 |= 0x08;
    if (datetime.noDayOfWeek) flags1 |= 0x04;
    if (datetime.noTime) flags1 |= 0x02;
    if (datetime.suti) flags1 |= 0x01;

    return writeArray(std::array<uint8_t, 8>{
                          year_offset,
                          static_cast<uint8_t>(datetime.month & 0x0F),
                          static_cast<uint8_t>(datetime.day & 0x1F),
                          byte3,
                          byte4,
                          static_cast<uint8_t>(datetime.second & 0x3F),
                          flags1,
                          0x00},
                      data);
}

util::Result<void> Dpt19::decode(std::span<const uint8_t> data, Value& datetime) {
    if (data.size() < 8) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    // Extract year
    datetime.year = 1900 + data[0];
    
    // Extract month, day
    datetime.month = data[1] & 0x0F;
    datetime.day = data[2] & 0x1F;
    
    // Extract day of week and hour
    datetime.dayOfWeek = (data[3] >> 5) & 0x07;
    datetime.hour = ((data[3] & 0x07) << 2) | ((data[4] >> 6) & 0x03);
    
    // Extract minute and second
    datetime.minute = data[4] & 0x3F;
    datetime.second = data[5] & 0x3F;
    
    // Extract flags
    datetime.fault = (data[6] & 0x80) != 0;
    datetime.workingDay = (data[6] & 0x40) != 0;
    datetime.noWD = (data[6] & 0x20) != 0;
    datetime.noYear = (data[6] & 0x10) != 0;
    datetime.noDate = (data[6] & 0x08) != 0;
    datetime.noDayOfWeek = (data[6] & 0x04) != 0;
    datetime.noTime = (data[6] & 0x02) != 0;
    datetime.suti = (data[6] & 0x01) != 0;
    
    // Validate ranges (unless marked as invalid)
    if (!datetime.noYear && (datetime.year < 1900 || datetime.year > 2155)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (!datetime.noDate && (datetime.month < 1 || datetime.month > 12 || datetime.day < 1 || datetime.day > 31)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (!datetime.noDayOfWeek && datetime.dayOfWeek > 7) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (!datetime.noTime && (datetime.hour > 23 || datetime.minute > 59 || datetime.second > 59)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    return util::Result<void>::ok();
}

// DPT 12 - 32-bit Unsigned (big-endian)
util::Result<size_t> Dpt12::encode(uint32_t value, std::span<uint8_t> data) {
    return writeDword(value, data);
}

util::Result<void> Dpt12::decode(std::span<const uint8_t> data, uint32_t& value) {
    if (data.size() < 4) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    value = bits::makeDword(data[0], data[1], data[2], data[3]);
    return util::Result<void>::ok();
}

// DPT 13 - 32-bit Signed (big-endian)
util::Result<size_t> Dpt13::encode(int32_t value, std::span<uint8_t> data) {
    return writeDword(static_cast<uint32_t>(value), data);
}

util::Result<void> Dpt13::decode(std::span<const uint8_t> data, int32_t& value) {
    if (data.size() < 4) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    uint32_t unsigned_val = bits::makeDword(data[0], data[1], data[2], data[3]);
    value = static_cast<int32_t>(unsigned_val);
    return util::Result<void>::ok();
}

// DPT 14 - 32-bit Float (IEEE 754)
util::Result<size_t> Dpt14::encode(float value, std::span<uint8_t> data) {
    if (std::isnan(value) || std::isinf(value)) {
        return util::ErrorCode::InvalidParameter;
    }
    
    // Use union to access raw bytes
    union {
        float f;
        uint32_t u;
    } converter;
    
    converter.f = value;
    
    return writeDword(converter.u, data);
}

util::Result<void> Dpt14::decode(std::span<const uint8_t> data, float& value) {
    if (data.size() < 4) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    union {
        float f;
        uint32_t u;
    } converter;
    
    converter.u = bits::makeDword(data[0], data[1], data[2], data[3]);
    
    value = converter.f;
    return util::Result<void>::ok();
}

// DPT 16 - String (ASCII, 14 bytes max)
util::Result<size_t> Dpt16::encode(const std::string& value, std::span<uint8_t> data) {
    // Maximum 13 characters + NULL terminator
    if (value.length() > 13) {
        return util::ErrorCode::InvalidParameter;
    }

    auto capacityResult = ensureCapacity(data, 14u);
    if (capacityResult.isError()) {
        return capacityResult.error();
    }

    size_t index = 0;
    for (char c : value) {
        data[index++] = static_cast<uint8_t>(c);
    }
    while (index < 14u) {
        data[index++] = 0x00u;
    }

    return 14u;
}

util::Result<void> Dpt16::decode(std::span<const uint8_t> data, std::string& value) {
    if (data.size() < 14) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    value.clear();
    
    // Extract string until NULL or end of 14 bytes
    for (size_t i = 0; i < 14; ++i) {
        if (data[i] == 0x00) break;
        value += static_cast<char>(data[i]);
    }
    
    return util::Result<void>::ok();
}

// DPT 17 - Scene Number
util::Result<size_t> Dpt17::encode(uint8_t sceneNumber, std::span<uint8_t> data) {
    if (sceneNumber > 63) {
        return util::ErrorCode::InvalidParameter;
    }

    return writeArray(std::array<uint8_t, 1>{static_cast<uint8_t>(sceneNumber & 0x3F)}, data);
}

util::Result<size_t> Dpt17::encode(const Value& scene, std::span<uint8_t> data) {
    if (scene.sceneNumber > 63) {
        return util::ErrorCode::InvalidParameter;
    }
    
    // Bit 7 = validity (0=valid, 1=invalid), bits 0-5 = scene number
    uint8_t byte = scene.sceneNumber & 0x3F;
    if (!scene.valid) {
        byte |= 0x80;  // Set invalid bit
    }
    
    return writeArray(std::array<uint8_t, 1>{byte}, data);
}

util::Result<void> Dpt17::decode(std::span<const uint8_t> data, Value& scene) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    scene.sceneNumber = data[0] & 0x3F;
    scene.valid = (data[0] & 0x80) == 0;  // Bit 7 = 0 means valid
    
    return util::Result<void>::ok();
}

// DPT 15 - Access Control (4 bytes)
util::Result<size_t> Dpt15::encode(const Value& access, std::span<uint8_t> data) {
    // Validate digit ranges
    if (access.digit0 > 9 || access.digit1 > 9 || 
        access.digit2 > 9 || access.digit3 > 9) {
        return util::ErrorCode::InvalidParameter;
    }
    
    auto capacityResult = ensureCapacity(data, 4u);
    if (capacityResult.isError()) {
        return capacityResult.error();
    }
    
    // Each byte: [digit(4bits) E P R C]
    // digit in upper nibble, control bits in lower nibble
    uint8_t controlBits = 0;
    if (access.encrypted) controlBits |= 0x01;
    if (access.readDirection) controlBits |= 0x02;
    if (access.permission) controlBits |= 0x04;
    if (access.error) controlBits |= 0x08;
    
    data[0] = (access.digit3 << 4) | controlBits;
    data[1] = (access.digit2 << 4) | controlBits;
    data[2] = (access.digit1 << 4) | controlBits;
    data[3] = (access.digit0 << 4) | controlBits;
    
    return 4u;
}

util::Result<void> Dpt15::decode(std::span<const uint8_t> data, Value& access) {
    if (data.size() < 4) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    // Extract digits from upper nibbles
    access.digit3 = (data[0] >> 4) & 0x0F;
    access.digit2 = (data[1] >> 4) & 0x0F;
    access.digit1 = (data[2] >> 4) & 0x0F;
    access.digit0 = (data[3] >> 4) & 0x0F;
    
    // Validate digits
    if (access.digit0 > 9 || access.digit1 > 9 || 
        access.digit2 > 9 || access.digit3 > 9) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    // Extract control bits from last byte (all bytes should have same control bits)
    uint8_t controlBits = data[3] & 0x0F;
    access.encrypted = (controlBits & 0x01) != 0;
    access.readDirection = (controlBits & 0x02) != 0;
    access.permission = (controlBits & 0x04) != 0;
    access.error = (controlBits & 0x08) != 0;
    
    return util::Result<void>::ok();
}

// DPT 18 - Scene Control (1 byte)
util::Result<size_t> Dpt18::encode(const Value& scene, std::span<uint8_t> data) {
    return encode(scene.sceneNumber, scene.learn, data);
}

util::Result<size_t> Dpt18::encode(uint8_t sceneNumber, bool learn, std::span<uint8_t> data) {
    // Scene number must be 0-63
    if (sceneNumber > 63) {
        return util::ErrorCode::InvalidParameter;
    }
    
    uint8_t value = sceneNumber & 0x3F;
    if (learn) {
        value |= 0x80;  // Set bit 7 for learn
    }

    return writeArray(std::array<uint8_t, 1>{value}, data);
}

util::Result<void> Dpt18::decode(std::span<const uint8_t> data, Value& scene) {
    return decode(data, scene.sceneNumber, scene.learn);
}

util::Result<void> Dpt18::decode(std::span<const uint8_t> data, uint8_t& sceneNumber, bool& learn) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    sceneNumber = data[0] & 0x3F;
    learn = (data[0] & 0x80) != 0;
    
    return util::Result<void>::ok();
}

// DPT 20 - HVAC Mode (1 byte)
util::Result<size_t> Dpt20::encode(Mode mode, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 1>{static_cast<uint8_t>(mode)}, data);
}

util::Result<void> Dpt20::decode(std::span<const uint8_t> data, Mode& mode) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    uint8_t value = data[0];
    if (value > 4) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    mode = static_cast<Mode>(value);
    return util::Result<void>::ok();
}

// DPT 232 - RGB Color (3 bytes)
util::Result<size_t> Dpt232::encode(const Value& color, std::span<uint8_t> data) {
    return encode(color.red, color.green, color.blue, data);
}

util::Result<size_t> Dpt232::encode(uint8_t red, uint8_t green, uint8_t blue, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 3>{red, green, blue}, data);
}

util::Result<void> Dpt232::decode(std::span<const uint8_t> data, Value& color) {
    return decode(data, color.red, color.green, color.blue);
}

util::Result<void> Dpt232::decode(std::span<const uint8_t> data, uint8_t& red, uint8_t& green, uint8_t& blue) {
    if (data.size() < 3) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    red = data[0];
    green = data[1];
    blue = data[2];
    
    return util::Result<void>::ok();
}

// DPT 242 - xyY Color (6 bytes)
util::Result<size_t> Dpt242::encode(const Value& color, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 6>{
                          static_cast<uint8_t>((color.x >> 8) & 0xFFu),
                          static_cast<uint8_t>(color.x & 0xFFu),
                          static_cast<uint8_t>((color.y >> 8) & 0xFFu),
                          static_cast<uint8_t>(color.y & 0xFFu),
                          color.brightness,
                          static_cast<uint8_t>(color.valid ? 0x01u : 0x00u)},
                      data);
}

util::Result<void> Dpt242::decode(std::span<const uint8_t> data, Value& color) {
    if (data.size() < 6) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    // x coordinate (big-endian)
    color.x = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    
    // y coordinate (big-endian)
    color.y = (static_cast<uint16_t>(data[2]) << 8) | data[3];
    
    // Brightness
    color.brightness = data[4];
    
    // Valid flag
    color.valid = (data[5] & 0x01) != 0;
    
    return util::Result<void>::ok();
}

// DPT 243 - HSV Color (3 bytes)
util::Result<size_t> Dpt243::encode(const Value& color, std::span<uint8_t> data) {
    return encode(color.hue, color.saturation, color.value, data);
}

util::Result<size_t> Dpt243::encode(uint16_t hue, uint8_t saturation, uint8_t value, std::span<uint8_t> data) {
    // Validate ranges
    if (hue > 360 || saturation > 100 || value > 100) {
        return util::ErrorCode::InvalidParameter;
    }

    // Hue: 0-360° mapped to 0-255 (360° / 255 ≈ 1.41° per step)
    uint8_t hueScaled = static_cast<uint8_t>((hue * 255u) / 360u);

    // Saturation: 0-100% mapped to 0-255
    uint8_t satScaled = static_cast<uint8_t>((static_cast<uint16_t>(saturation) * 255u) / 100u);

    // Value: 0-100% mapped to 0-255
    uint8_t valScaled = static_cast<uint8_t>((static_cast<uint16_t>(value) * 255u) / 100u);

    return writeArray(std::array<uint8_t, 3>{hueScaled, satScaled, valScaled}, data);
}

util::Result<void> Dpt243::decode(std::span<const uint8_t> data, Value& color) {
    return decode(data, color.hue, color.saturation, color.value);
}

util::Result<void> Dpt243::decode(std::span<const uint8_t> data, uint16_t& hue, uint8_t& saturation, uint8_t& value) {
    if (data.size() < 3) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    // Hue: 0-255 mapped to 0-360°
    hue = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) * 360u) / 255u);
    
    // Saturation: 0-255 mapped to 0-100%
    saturation = static_cast<uint8_t>((static_cast<uint16_t>(data[1]) * 100u) / 255u);
    
    // Value: 0-255 mapped to 0-100%
    value = static_cast<uint8_t>((static_cast<uint16_t>(data[2]) * 100u) / 255u);
    
    return util::Result<void>::ok();
}

// DPT 244 - Color Transition (6 bytes)
util::Result<size_t> Dpt244::encode(const Value& transition, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 6>{
                          transition.red,
                          transition.green,
                          transition.blue,
                          static_cast<uint8_t>((transition.fadeTime >> 8) & 0xFFu),
                          static_cast<uint8_t>(transition.fadeTime & 0xFFu),
                          transition.reserved},
                      data);
}

util::Result<void> Dpt244::decode(std::span<const uint8_t> data, Value& transition) {
    if (data.size() < 6) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    transition.red = data[0];
    transition.green = data[1];
    transition.blue = data[2];
    
    // Fade time (big-endian)
    transition.fadeTime = (static_cast<uint16_t>(data[3]) << 8) | data[4];
    
    transition.reserved = data[5];
    
    return util::Result<void>::ok();
}

} // namespace application
} // namespace knx
