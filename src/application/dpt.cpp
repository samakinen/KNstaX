// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file dpt.cpp
 * @brief DPT implementation
 */

#include "knx/application/dpt.hpp"
#include "knx/constants.hpp"
#include "knx/util/bit_ops.hpp"
#include <bit>
#include <cmath>
#include <limits>
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
//
// The table is the run-time catalogue (KNX_ENABLE_DPT_CATALOGUE): names for
// lookupDptInfo() and the host tools. The codecs do not need it: with it off the
// dynamic paths take the value type from dptValueTypeFor().
#if KNX_FEATURE_DPT_CATALOGUE
#define KNX_DPT_CATALOG_ENTRY(symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
    {dptids::symbol, KNX_DPT_NAME(shortName), KNX_DPT_TEXT(description), \
     DptTraits<dpttags::symbol>::kValueType, true},
const DptInfo kKnownDpts[] = {
#include "knx/application/dpt_catalog.inc"
};
#undef KNX_DPT_CATALOG_ENTRY
#endif

/**
 * @brief Dispatch dynamic encode/decode on the value type.
 *
 * Every catalog entry sharing a DptValue::Type shares its codec (verified
 * across the whole catalog), so one representative tag per value type covers
 * all of them.  X-macro so encode and decode cannot drift apart.
 */
#define KNX_DPT_CODEC_DISPATCH_ALWAYS(X)                \
    X(Boolean,         Bool)                            \
    X(Controlled1Bit,  Controlled1Bit)                  \
    X(Controlled3Bit,  Controlled3Bit)                  \
    X(Unsigned8,       Unsigned8)                       \
    X(Signed8,         Signed8)                         \
    X(Unsigned16,      Unsigned16)                      \
    X(Signed16,        Signed16)                        \
    X(Unsigned32,      Unsigned32)                      \
    X(Signed32,        Signed32)                        \
    X(SceneNumber,     SceneNumber)                     \
    X(SceneControl,    SceneControl)                    \
    X(HvacMode,        HvacMode)

// The families a build can configure out (Kconfig KNX_DPT_*): a value type of
// an absent family has no case, so its codec is neither compiled nor linked and
// the request is answered OperationNotSupported like any unknown type.
#if KNX_FEATURE_DPT_FLOAT
#  define KNX_DPT_CODEC_DISPATCH_FLOAT(X)               \
    X(Float2Byte,      Float2Byte)                      \
    X(Float4Byte,      Float4Byte)
#else
#  define KNX_DPT_CODEC_DISPATCH_FLOAT(X)
#endif
#if KNX_FEATURE_DPT_TIME
#  define KNX_DPT_CODEC_DISPATCH_TIME(X)                \
    X(TimeOfDay,       TimeOfDay)                       \
    X(Date,            Date)                            \
    X(DateTime,        DateTime)
#else
#  define KNX_DPT_CODEC_DISPATCH_TIME(X)
#endif
#if KNX_FEATURE_DPT_STRING
#  define KNX_DPT_CODEC_DISPATCH_STRING(X)              \
    X(Character,       Character)                       \
    X(String,          String)
#else
#  define KNX_DPT_CODEC_DISPATCH_STRING(X)
#endif
#if KNX_FEATURE_DPT_COLOUR
#  define KNX_DPT_CODEC_DISPATCH_COLOUR(X)              \
    X(RgbColor,        RgbColor)                        \
    X(XyYColor,        XyYColor)                        \
    X(ColourTransitionXyY, ColourTransitionXyY)         \
    X(ConverterStatus, ConverterStatus)
#else
#  define KNX_DPT_CODEC_DISPATCH_COLOUR(X)
#endif
#if KNX_FEATURE_DPT_ACCESS
#  define KNX_DPT_CODEC_DISPATCH_ACCESS(X)              \
    X(AccessControl,   AccessControl)
#else
#  define KNX_DPT_CODEC_DISPATCH_ACCESS(X)
#endif

#define KNX_DPT_CODEC_DISPATCH(X)                       \
    KNX_DPT_CODEC_DISPATCH_ALWAYS(X)                    \
    KNX_DPT_CODEC_DISPATCH_FLOAT(X)                     \
    KNX_DPT_CODEC_DISPATCH_TIME(X)                      \
    KNX_DPT_CODEC_DISPATCH_STRING(X)                    \
    KNX_DPT_CODEC_DISPATCH_COLOUR(X)                    \
    KNX_DPT_CODEC_DISPATCH_ACCESS(X)

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
        default:  // a value type whose family is configured out has no case
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
        default:  // a value type whose family is configured out has no case
            break;
    }
    return util::ErrorCode::OperationNotSupported;
}

} // namespace

#if KNX_FEATURE_DPT_CATALOGUE
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
#endif  // KNX_FEATURE_DPT_CATALOGUE

namespace {

/**
 * @brief The runtime value type @p id is carried as, or Unsupported.
 *
 * With the run-time catalogue this is the catalogue's answer, as it always was
 * (an exact entry, else the main type's own entry, none for an unknown type).
 * Without it the catalogue's table is not built and the id alone decides
 * (dptValueTypeFor()), restricted to the families this build has. The two agree
 * for every catalogue entry (test_dpt checks it); they can differ only for an
 * id the catalogue does not list.
 */
DptValue::Type resolveValueType(DptId id)
{
#if KNX_FEATURE_DPT_CATALOGUE
    const auto* info = lookupDptInfo(id);
    return (info != nullptr && info->runtimeSupported) ? info->valueType : DptValue::Type::Unsupported;
#else
    return dptFamilyEnabled(id) ? dptValueTypeFor(id) : DptValue::Type::Unsupported;
#endif
}

} // namespace

bool supportsDpt(DptId id)
{
    return resolveValueType(id) != DptValue::Type::Unsupported;
}

bool dptValueMatches(DptId id, const DptValue& value)
{
    const auto type = resolveValueType(id);
    return type != DptValue::Type::Unsupported && type == value.type();
}

util::Result<size_t> encodeDptValue(DptId id, const DptValue& value, std::span<uint8_t> data) {
    const auto type = resolveValueType(id);
    if (type == DptValue::Type::Unsupported || type != value.type()) {
        return util::ErrorCode::OperationNotSupported;
    }

    return encodeForValueType(type, value, data);
}

util::Result<DptValue> decodeDptValue(DptId id, std::span<const uint8_t> data) {
    const auto type = resolveValueType(id);
    if (type == DptValue::Type::Unsupported) {
        return util::ErrorCode::OperationNotSupported;
    }

    return decodeForValueType(type, data);
}

#if KNX_FEATURE_DPT_FLOAT
bool dptValueAsScalar(const DptValue& value, double& out) {
    switch (value.type()) {
        case DptValue::Type::Boolean:     out = value.asBool() ? 1.0 : 0.0;             return true;
#if KNX_FEATURE_DPT_STRING
        case DptValue::Type::Character:   out = static_cast<double>(value.asChar());    return true;
#endif
        case DptValue::Type::Unsigned8:   out = static_cast<double>(value.asUInt8());   return true;
        case DptValue::Type::Signed8:     out = static_cast<double>(value.asInt8());    return true;
        case DptValue::Type::Unsigned16:  out = static_cast<double>(value.asUInt16());  return true;
        case DptValue::Type::Signed16:    out = static_cast<double>(value.asInt16());   return true;
#if KNX_FEATURE_DPT_FLOAT
        case DptValue::Type::Float2Byte:  out = static_cast<double>(value.asFloat());   return true;
#endif
        case DptValue::Type::Unsigned32:  out = static_cast<double>(value.asUInt32());  return true;
        case DptValue::Type::Signed32:    out = static_cast<double>(value.asInt32());   return true;
#if KNX_FEATURE_DPT_FLOAT
        case DptValue::Type::Float4Byte:  out = static_cast<double>(value.asFloat());   return true;
#endif
        default:
            return false;
    }
}
#endif  // KNX_FEATURE_DPT_FLOAT

bool dptValueAsInteger(const DptValue& value, int64_t& out) {
    switch (value.type()) {
        case DptValue::Type::Boolean:     out = value.asBool() ? 1 : 0;           return true;
        case DptValue::Type::Unsigned8:   out = value.asUInt8();                  return true;
        case DptValue::Type::Signed8:     out = value.asInt8();                   return true;
        case DptValue::Type::Unsigned16:  out = value.asUInt16();                 return true;
        case DptValue::Type::Signed16:    out = value.asInt16();                  return true;
        case DptValue::Type::Unsigned32:  out = value.asUInt32();                 return true;
        case DptValue::Type::Signed32:    out = value.asInt32();                  return true;
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

#if KNX_FEATURE_DPT_FLOAT
// DPT 9 - Float (2 bytes)
// Format: SEEE EMMM MMMM MMMM
// S = sign bit (bit 15), E = exponent (bits 14-11), M = signed 11-bit mantissa (bits 10-0)
// Value = (0.01 * M) * 2^E
util::Result<size_t> Dpt9::encode(float value, std::span<uint8_t> data) {
    // 03_07_02 §3.10: 7FFFh "shall always be used to denote invalid data";
    // NaN is this codec's in-memory form of that code.
    if (std::isnan(value)) {
        return writeWord(DPT9_INVALID, data);
    }
    if (std::isinf(value)) {
        return util::ErrorCode::InvalidParameter;
    }

    // Each exponent's mantissa is rounded once, from the value itself: rounding
    // the previous exponent's already-rounded mantissa again (halving it) can
    // land one step off the nearest code.
    const double hundredths = static_cast<double>(value) * 100.0;
    for (int exponent = 0; exponent <= DPT9_MAX_EXPONENT; ++exponent) {
        // Division by a power of two is exact; it also keeps ldexp out of the image.
        const long mantissa = std::lround(hundredths / static_cast<double>(1u << exponent));
        if (mantissa < DPT9_MIN_MANTISSA || mantissa > DPT9_MAX_MANTISSA) {
            continue;
        }
        const uint16_t raw = static_cast<uint16_t>(
            (mantissa < 0 ? DPT9_SIGN_MASK : 0u) |
            (static_cast<unsigned>(exponent) << 11) |
            (static_cast<uint16_t>(mantissa) & DPT9_MANTISSA_MASK));
        // The only finite value that rounds onto 7FFFh lies above the range.
        if (raw == DPT9_INVALID) {
            return util::ErrorCode::InvalidParameter;
        }
        return writeWord(raw, data);
    }
    return util::ErrorCode::InvalidParameter;
}

util::Result<void> Dpt9::decode(std::span<const uint8_t> data, float& value) {
    if (data.size() < 2) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    const uint16_t raw = bits::makeWord(data[0], data[1]);
    if (raw == DPT9_INVALID) {
        value = std::numeric_limits<float>::quiet_NaN();
        return util::Result<void>::ok();
    }
    const int exponent = (raw >> 11) & 0x0F;
    int mantissa = raw & DPT9_MANTISSA_MASK;
    if ((raw & DPT9_SIGN_MASK) != 0u) {
        mantissa -= (1 << DPT9_MANTISSA_BITS);
    }

    value = std::ldexp(static_cast<float>(mantissa) * DPT9_SCALE_FACTOR, exponent);
    return util::Result<void>::ok();
}

#endif  // KNX_FEATURE_DPT_FLOAT
// DPT 2 - 1-bit controlled (2 bits: control + value)
util::Result<size_t> Dpt2::encode(bool control, bool value, std::span<uint8_t> data) {
    uint8_t byte = static_cast<uint8_t>(((control ? 1 : 0) << 1) | (value ? 1 : 0));
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
    uint8_t byte = static_cast<uint8_t>(((control ? 1 : 0) << 3) | (stepCode & DPT3_STEPCODE_MASK));
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

#if KNX_FEATURE_DPT_STRING
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

#endif  // KNX_FEATURE_DPT_STRING
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

#if KNX_FEATURE_DPT_TIME
// DPT 10 - Time of Day
util::Result<size_t> Dpt10::encode(const Value& time, std::span<uint8_t> data) {
    return encode(time.dayOfWeek, time.hour, time.minute, time.second, data);
}

util::Result<size_t> Dpt10::encode(uint8_t dayOfWeek, uint8_t hour, uint8_t minute, uint8_t second, std::span<uint8_t> data) {
    // Validate ranges
    if (dayOfWeek > DPT10_MAX_DAY || hour > DPT10_MAX_HOUR || minute > DPT10_MAX_MINUTE || second > DPT10_MAX_SECOND) {
        return util::ErrorCode::InvalidParameter;
    }
    
    // 03_07_02 §3.11, N3U5r2U6r2U6: [N N N H H H H H] [0 0 M M M M M M] [0 0 S S S S S S]
    return writeArray(std::array<uint8_t, 3>{
                          static_cast<uint8_t>((dayOfWeek << 5) | hour),
                          minute,
                          second},
                      data);
}

util::Result<void> Dpt10::decode(std::span<const uint8_t> data, Value& time) {
    if (data.size() < 3) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    // Reserved bits (the top two of the minute and second octets) are masked.
    time.dayOfWeek = (data[0] >> 5) & 0x07;
    time.hour = data[0] & DPT10_HOUR_MASK;
    time.minute = data[1] & DPT10_MINUTE_MASK;
    time.second = data[2] & DPT10_SECOND_MASK;
    
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

// DPT 19.001 - DPT_DateTime (03_07_02 §3.20)
//
// Octets MSB first: [year - 1900] [r4 month] [r3 day] [DoW(3) hour(5)]
// [r2 minutes] [r2 seconds] [F WD NWD NY ND NDoW NT SUTI] [CLQ SRC r6]
namespace {

constexpr uint8_t kDpt19Fault = 0x80;
constexpr uint8_t kDpt19WorkingDay = 0x40;
constexpr uint8_t kDpt19NoWorkingDay = 0x20;
constexpr uint8_t kDpt19NoYear = 0x10;
constexpr uint8_t kDpt19NoDate = 0x08;
constexpr uint8_t kDpt19NoDayOfWeek = 0x04;
constexpr uint8_t kDpt19NoTime = 0x02;
constexpr uint8_t kDpt19SummerTime = 0x01;
constexpr uint8_t kDpt19ExternalSync = 0x80;   // CLQ
constexpr uint8_t kDpt19ReliableSource = 0x40; // SRC
constexpr uint8_t kDpt19Reserved = 0x3F;

constexpr uint16_t kDpt19YearBase = 1900;
constexpr uint16_t kDpt19YearMax = 2155;

// Hour 0…24, where 24 is valid only as 24:00:00 (Note 11).
constexpr bool dpt19TimeValid(uint8_t hour, uint8_t minute, uint8_t second)
{
    if (hour == 24) {
        return minute == 0 && second == 0;
    }
    return hour <= 23 && minute <= 59 && second <= 59;
}

} // namespace

util::Result<size_t> Dpt19::encode(const Value& datetime, std::span<uint8_t> data) {
    // Only fields the flags declare valid are range-checked; the others are
    // transmitted as zero.
    if (!datetime.noYear && (datetime.year < kDpt19YearBase || datetime.year > kDpt19YearMax)) {
        return util::ErrorCode::InvalidParameter;
    }
    if (!datetime.noDate && (datetime.month < 1 || datetime.month > 12 ||
                             datetime.day < 1 || datetime.day > 31)) {
        return util::ErrorCode::InvalidParameter;
    }
    if (!datetime.noDayOfWeek && datetime.dayOfWeek > 7) {
        return util::ErrorCode::InvalidParameter;
    }
    if (!datetime.noTime && !dpt19TimeValid(datetime.hour, datetime.minute, datetime.second)) {
        return util::ErrorCode::InvalidParameter;
    }

    const uint8_t year = datetime.noYear ? 0u : static_cast<uint8_t>(datetime.year - kDpt19YearBase);
    const uint8_t month = datetime.noDate ? 0u : datetime.month;
    const uint8_t day = datetime.noDate ? 0u : datetime.day;
    const uint8_t dayOfWeek = datetime.noDayOfWeek ? 0u : datetime.dayOfWeek;
    const uint8_t hour = datetime.noTime ? 0u : datetime.hour;
    const uint8_t minute = datetime.noTime ? 0u : datetime.minute;
    const uint8_t second = datetime.noTime ? 0u : datetime.second;

    uint8_t flags = 0;
    if (datetime.fault) flags |= kDpt19Fault;
    if (datetime.workingDay && !datetime.noWD) flags |= kDpt19WorkingDay;
    if (datetime.noWD) flags |= kDpt19NoWorkingDay;
    if (datetime.noYear) flags |= kDpt19NoYear;
    if (datetime.noDate) flags |= kDpt19NoDate;
    if (datetime.noDayOfWeek) flags |= kDpt19NoDayOfWeek;
    if (datetime.noTime) flags |= kDpt19NoTime;
    if (datetime.suti) flags |= kDpt19SummerTime;

    uint8_t quality = 0;
    if (datetime.externalSync) quality |= kDpt19ExternalSync;
    if (datetime.reliableSource) quality |= kDpt19ReliableSource;

    return writeArray(std::array<uint8_t, 8>{
                          year,
                          month,
                          day,
                          static_cast<uint8_t>((dayOfWeek << 5) | hour),
                          minute,
                          second,
                          flags,
                          quality},
                      data);
}

util::Result<void> Dpt19::decode(std::span<const uint8_t> data, Value& datetime) {
    if (data.size() < 8) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    // Note 15: the reserved bits of the last octet "shall be 0 ... Receivers
    // shall check these bits to be 0."
    if ((data[7] & kDpt19Reserved) != 0u) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    Value decoded{};
    decoded.year = static_cast<uint16_t>(kDpt19YearBase + data[0]);
    decoded.month = data[1] & 0x0F;
    decoded.day = data[2] & 0x1F;
    decoded.dayOfWeek = (data[3] >> 5) & 0x07;
    decoded.hour = data[3] & 0x1F;
    decoded.minute = data[4] & 0x3F;
    decoded.second = data[5] & 0x3F;

    decoded.fault = (data[6] & kDpt19Fault) != 0;
    decoded.workingDay = (data[6] & kDpt19WorkingDay) != 0;
    decoded.noWD = (data[6] & kDpt19NoWorkingDay) != 0;
    decoded.noYear = (data[6] & kDpt19NoYear) != 0;
    decoded.noDate = (data[6] & kDpt19NoDate) != 0;
    decoded.noDayOfWeek = (data[6] & kDpt19NoDayOfWeek) != 0;
    decoded.noTime = (data[6] & kDpt19NoTime) != 0;
    decoded.suti = (data[6] & kDpt19SummerTime) != 0;
    decoded.externalSync = (data[7] & kDpt19ExternalSync) != 0;
    decoded.reliableSource = (data[7] & kDpt19ReliableSource) != 0;

    // The year octet covers exactly 1900…2155, so it needs no check. Fields
    // flagged "not valid" are not checked either.
    if (!decoded.noDate && (decoded.month < 1 || decoded.month > 12 ||
                            decoded.day < 1 || decoded.day > 31)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    // Note 11: "Messages with invalid values ("Hour = 24", Minutes and Seconds
    // not zero) have to be ignored by the receiver."
    if (!decoded.noTime && !dpt19TimeValid(decoded.hour, decoded.minute, decoded.second)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    datetime = decoded;
    return util::Result<void>::ok();
}

#endif  // KNX_FEATURE_DPT_TIME
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

#if KNX_FEATURE_DPT_FLOAT
// DPT 14 - 32-bit Float (IEEE 754 single precision, 03_07_02 §3.15)
util::Result<size_t> Dpt14::encode(float value, std::span<uint8_t> data) {
    if (std::isnan(value) || std::isinf(value)) {
        return util::ErrorCode::InvalidParameter;
    }
    static_assert(std::numeric_limits<float>::is_iec559, "DPT 14 requires IEEE 754 float");
    return writeDword(std::bit_cast<uint32_t>(value), data);
}

util::Result<void> Dpt14::decode(std::span<const uint8_t> data, float& value) {
    if (data.size() < 4) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    value = std::bit_cast<float>(bits::makeDword(data[0], data[1], data[2], data[3]));
    return util::Result<void>::ok();
}

#endif  // KNX_FEATURE_DPT_FLOAT
#if KNX_FEATURE_DPT_STRING
// DPT 16 - String, 14 octets (03_07_02 §3.17): "The length is fixed to 14
// octets ... If the string to be transmitted is smaller then 14 octets, unused
// trailing octets in the character string shall be set to NULL (00h)."
// A 14-character string therefore fills every octet and has no terminator.
namespace {
constexpr size_t kDpt16Octets = 14u;
} // namespace

util::Result<size_t> Dpt16::encode(const std::string& value, std::span<uint8_t> data) {
    if (value.length() > kDpt16Octets) {
        return util::ErrorCode::InvalidParameter;
    }

    auto capacityResult = ensureCapacity(data, kDpt16Octets);
    if (capacityResult.isError()) {
        return capacityResult.error();
    }

    size_t index = 0;
    for (char c : value) {
        data[index++] = static_cast<uint8_t>(c);
    }
    while (index < kDpt16Octets) {
        data[index++] = 0x00u;
    }

    return kDpt16Octets;
}

util::Result<void> Dpt16::decode(std::span<const uint8_t> data, std::string& value) {
    if (data.size() < kDpt16Octets) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    value.clear();
    for (size_t i = 0; i < kDpt16Octets; ++i) {
        if (data[i] == 0x00) break;
        value += static_cast<char>(data[i]);
    }

    return util::Result<void>::ok();
}

#endif  // KNX_FEATURE_DPT_STRING
// DPT 17.001 - Scene Number, r2U6 (03_07_02 §3.18)
util::Result<size_t> Dpt17::encode(uint8_t sceneNumber, std::span<uint8_t> data) {
    if (sceneNumber > 63) {
        return util::ErrorCode::InvalidParameter;
    }
    return writeArray(std::array<uint8_t, 1>{sceneNumber}, data);
}

util::Result<size_t> Dpt17::encode(const Value& scene, std::span<uint8_t> data) {
    return encode(scene.sceneNumber, data);
}

util::Result<void> Dpt17::decode(std::span<const uint8_t> data, Value& scene) {
    if (data.empty()) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    // The two reserved high bits are ignored.
    scene.sceneNumber = data[0] & 0x3F;
    return util::Result<void>::ok();
}

#if KNX_FEATURE_DPT_ACCESS
// DPT 15.000 - DPT_Access_Data, U4U4U4U4U4U4B4N4 (03_07_02 §3.16)
// [D6 D5] [D4 D3] [D2 D1] [E P D C | Index]
namespace {
constexpr uint32_t kDpt15MaxCode = 999999u;
constexpr uint8_t kDpt15Error = 0x80;
constexpr uint8_t kDpt15Permission = 0x40;
constexpr uint8_t kDpt15ReadDirection = 0x20;
constexpr uint8_t kDpt15Encrypted = 0x10;
} // namespace

util::Result<size_t> Dpt15::encode(const Value& access, std::span<uint8_t> data) {
    if (access.accessCode > kDpt15MaxCode || access.index > 0x0F) {
        return util::ErrorCode::InvalidParameter;
    }

    // Six decimal digits, D1 (least significant) first.
    uint8_t digits[6]{};
    uint32_t code = access.accessCode;
    for (auto& digit : digits) {
        digit = static_cast<uint8_t>(code % 10u);
        code /= 10u;
    }

    uint8_t last = access.index;
    if (access.error) last |= kDpt15Error;
    if (access.permission) last |= kDpt15Permission;
    if (access.readDirection) last |= kDpt15ReadDirection;
    if (access.encrypted) last |= kDpt15Encrypted;

    // One 32-bit word, D6 in the most significant nibble.
    uint32_t word = last;
    for (size_t i = 0; i < 6; ++i) {
        word |= static_cast<uint32_t>(digits[i]) << (8u + 4u * i);
    }
    return writeDword(word, data);
}

util::Result<void> Dpt15::decode(std::span<const uint8_t> data, Value& access) {
    if (data.size() < 4) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    uint32_t code = 0;
    for (size_t i = 0; i < 3; ++i) {
        const uint8_t high = (data[i] >> 4) & 0x0F;
        const uint8_t low = data[i] & 0x0F;
        if (high > 9 || low > 9) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        code = code * 100u + high * 10u + low;
    }

    access.accessCode = code;
    access.error = (data[3] & kDpt15Error) != 0;
    access.permission = (data[3] & kDpt15Permission) != 0;
    access.readDirection = (data[3] & kDpt15ReadDirection) != 0;
    access.encrypted = (data[3] & kDpt15Encrypted) != 0;
    access.index = data[3] & 0x0F;
    return util::Result<void>::ok();
}

#endif  // KNX_FEATURE_DPT_ACCESS
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

#if KNX_FEATURE_DPT_COLOUR
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

// DPT 242.600 - DPT_Colour_xyY, U16U16U8r6B2 (03_07_02 §6.9)
namespace {
constexpr uint8_t kColourValid = 0x02;      // C
constexpr uint8_t kBrightnessValid = 0x01;  // B

constexpr uint8_t colourValidity(bool colourValid, bool brightnessValid)
{
    return static_cast<uint8_t>((colourValid ? kColourValid : 0u) |
                                (brightnessValid ? kBrightnessValid : 0u));
}
} // namespace

util::Result<size_t> Dpt242::encode(const Value& color, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 6>{
                          static_cast<uint8_t>(color.x >> 8),
                          static_cast<uint8_t>(color.x & 0xFFu),
                          static_cast<uint8_t>(color.y >> 8),
                          static_cast<uint8_t>(color.y & 0xFFu),
                          color.brightness,
                          colourValidity(color.colourValid, color.brightnessValid)},
                      data);
}

util::Result<void> Dpt242::decode(std::span<const uint8_t> data, Value& color) {
    if (data.size() < 6) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    // The six reserved bits of the last octet are ignored.
    color.x = bits::makeWord(data[0], data[1]);
    color.y = bits::makeWord(data[2], data[3]);
    color.brightness = data[4];
    color.colourValid = (data[5] & kColourValid) != 0;
    color.brightnessValid = (data[5] & kBrightnessValid) != 0;
    return util::Result<void>::ok();
}

// DPT 243.600 - DPT_Colour_Transition_xyY, U16U16U16U8r6B2 (03_07_02 §6.10)
util::Result<size_t> Dpt243::encode(const Value& transition, std::span<uint8_t> data) {
    return writeArray(std::array<uint8_t, 8>{
                          static_cast<uint8_t>(transition.timePeriod >> 8),
                          static_cast<uint8_t>(transition.timePeriod & 0xFFu),
                          static_cast<uint8_t>(transition.x >> 8),
                          static_cast<uint8_t>(transition.x & 0xFFu),
                          static_cast<uint8_t>(transition.y >> 8),
                          static_cast<uint8_t>(transition.y & 0xFFu),
                          transition.brightness,
                          colourValidity(transition.colourValid, transition.brightnessValid)},
                      data);
}

util::Result<void> Dpt243::decode(std::span<const uint8_t> data, Value& transition) {
    if (data.size() < 8) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    transition.timePeriod = bits::makeWord(data[0], data[1]);
    transition.x = bits::makeWord(data[2], data[3]);
    transition.y = bits::makeWord(data[4], data[5]);
    transition.brightness = data[6];
    transition.colourValid = (data[7] & kColourValid) != 0;
    transition.brightnessValid = (data[7] & kBrightnessValid) != 0;
    return util::Result<void>::ok();
}

// DPT 244.600 - DPT_Converter_Status, N4B4N2N2N2N2 (03_07_02 §6.11)
// [CM(4) | HS: 0 0 switchOn inhibitActive] [FP(2) DP(2) PP(2) CF(2)]
namespace {
constexpr uint8_t kConverterModeMax = 9;    // 10…15 reserved
constexpr uint8_t kConverterPendingMax = 2; // 3 reserved
constexpr uint8_t kHsInhibitActive = 0x01;
constexpr uint8_t kHsSwitchOn = 0x02;
constexpr uint8_t kHsReserved = 0x0C;

constexpr bool converterStatusValid(const Dpt244Value& s)
{
    return s.mode <= kConverterModeMax && s.functionTestPending <= kConverterPendingMax &&
           s.durationTestPending <= kConverterPendingMax &&
           s.partialDurationTestPending <= kConverterPendingMax && s.failure <= kConverterPendingMax;
}
} // namespace

util::Result<size_t> Dpt244::encode(const Value& status, std::span<uint8_t> data) {
    if (!converterStatusValid(status)) {
        return util::ErrorCode::InvalidParameter;
    }
    const uint8_t high = static_cast<uint8_t>((status.mode << 4) |
                                              (status.hardwiredSwitchOn ? kHsSwitchOn : 0u) |
                                              (status.hardwiredInhibitActive ? kHsInhibitActive : 0u));
    const uint8_t low = static_cast<uint8_t>((status.functionTestPending << 6) |
                                             (status.durationTestPending << 4) |
                                             (status.partialDurationTestPending << 2) |
                                             status.failure);
    return writeWord(static_cast<uint16_t>((high << 8) | low), data);
}

util::Result<void> Dpt244::decode(std::span<const uint8_t> data, Value& status) {
    if (data.size() < 2) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    if ((data[0] & kHsReserved) != 0u) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    Value decoded{};
    decoded.mode = static_cast<uint8_t>(data[0] >> 4);
    decoded.hardwiredInhibitActive = (data[0] & kHsInhibitActive) != 0;
    decoded.hardwiredSwitchOn = (data[0] & kHsSwitchOn) != 0;
    decoded.functionTestPending = static_cast<uint8_t>(data[1] >> 6);
    decoded.durationTestPending = static_cast<uint8_t>((data[1] >> 4) & 0x03);
    decoded.partialDurationTestPending = static_cast<uint8_t>((data[1] >> 2) & 0x03);
    decoded.failure = static_cast<uint8_t>(data[1] & 0x03);
    if (!converterStatusValid(decoded)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    status = decoded;
    return util::Result<void>::ok();
}
#endif  // KNX_FEATURE_DPT_COLOUR

} // namespace application
} // namespace knx
