// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file dpt.hpp
 * @brief Data Point Types (DPT) - KNX standard data encodings
 * 
 * Supports DPT encodings/decodings per KNX specification:
 * - DPT 1: Boolean (1 bit)
 * - DPT 2: 1-bit controlled (2 bits - control + value)
 * - DPT 3: 3-bit controlled (4 bits - control + step code)
 * - DPT 4: Character (8 bits - ASCII)
 * - DPT 5: Unsigned integer (8 bits, 0-255)
 * - DPT 6: 8-bit signed (-128 to 127)
 * - DPT 7: 16-bit unsigned (0-65535)
 * - DPT 8: 16-bit signed (-32768 to 32767)
 * - DPT 9: Float (16 bits, 4-bit exp, 12-bit mantissa)
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include "knx/util/result.hpp"

namespace knx {
namespace application {

enum class DptMainType : uint16_t {
    Unsupported = 0,
    Boolean = 1,
    Controlled1Bit = 2,
    Controlled3Bit = 3,
    Character = 4,
    Unsigned8 = 5,
    Signed8 = 6,
    Unsigned16 = 7,
    Signed16 = 8,
    Float2Byte = 9,
    TimeOfDay = 10,
    Date = 11,
    Unsigned32 = 12,
    Signed32 = 13,
    Float4Byte = 14,
    AccessControl = 15,
    String = 16,
    SceneNumber = 17,
    SceneControl = 18,
    DateTime = 19,
    HvacMode = 20,
    // Main types 21 (B8) and 22 (B16) are KNX bit-set / status words. They are
    // carried here as raw unsigned integers (see DptValue::Type::Unsigned8 /
    // Unsigned16); callers pack/unpack the individual status bits per the KNX
    // spec for the concrete sub-type (e.g. DPT 21.102 StatusRHC, 22.101
    // StatusRHCC — HVAC room heating/cooling controller status).
    Bitfield8 = 21,
    Bitfield16 = 22,
    RgbColor = 232,
    XyYColor = 242,
    HsvColor = 243,
    ColorTransition = 244,
};

struct DptId {
    DptMainType main{DptMainType::Unsupported};
    uint16_t sub{0};

    constexpr DptId() = default;
    constexpr DptId(DptMainType mainType, uint16_t subType = 0)
        : main(mainType)
        , sub(subType)
    {
    }

    constexpr uint16_t mainNumber() const { return static_cast<uint16_t>(main); }
    constexpr bool hasSubtype() const { return sub != 0; }
};

constexpr bool operator==(const DptId& lhs, const DptId& rhs)
{
    return lhs.main == rhs.main && lhs.sub == rhs.sub;
}

constexpr bool operator!=(const DptId& lhs, const DptId& rhs)
{
    return !(lhs == rhs);
}

constexpr DptId makeDptId(uint16_t mainNumber, uint16_t subType = 0)
{
    return DptId{static_cast<DptMainType>(mainNumber), subType};
}

struct Dpt2Value {
    bool control{false};
    bool value{false};
};

struct Dpt3Value {
    bool control{false};
    uint8_t stepCode{0};
};

struct Dpt10Value {
    uint8_t dayOfWeek{0};
    uint8_t hour{0};
    uint8_t minute{0};
    uint8_t second{0};
};

struct Dpt11Value {
    uint8_t day{1};
    uint8_t month{1};
    uint8_t year{0};
};

struct Dpt15Value {
    uint8_t digit0{0};
    uint8_t digit1{0};
    uint8_t digit2{0};
    uint8_t digit3{0};
    bool error{false};
    bool permission{false};
    bool readDirection{false};
    bool encrypted{false};
};

struct Dpt17Value {
    uint8_t sceneNumber{0};
    bool valid{true};
};

struct Dpt18Value {
    uint8_t sceneNumber{0};
    bool learn{false};
};

struct Dpt19Value {
    uint16_t year{1900};
    uint8_t month{1};
    uint8_t day{1};
    uint8_t dayOfWeek{0};
    uint8_t hour{0};
    uint8_t minute{0};
    uint8_t second{0};
    bool fault{false};
    bool workingDay{false};
    bool noWD{false};
    bool noYear{false};
    bool noDate{false};
    bool noDayOfWeek{false};
    bool noTime{false};
    bool suti{false};
};

enum class Dpt20Mode : uint8_t {
    Auto = 0,
    Comfort = 1,
    Standby = 2,
    Economy = 3,
    BuildingProtection = 4,
};

struct Dpt232Value {
    uint8_t red{0};
    uint8_t green{0};
    uint8_t blue{0};
};

struct Dpt242Value {
    uint16_t x{0};
    uint16_t y{0};
    uint8_t brightness{0};
    bool valid{false};
};

struct Dpt243Value {
    uint16_t hue{0};
    uint8_t saturation{0};
    uint8_t value{0};
};

struct Dpt244Value {
    uint8_t red{0};
    uint8_t green{0};
    uint8_t blue{0};
    uint16_t fadeTime{0};
    uint8_t reserved{0};
};

namespace dptids {

#define KNX_DPT_CATALOG_ENTRY(symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
inline constexpr DptId symbol{DptMainType::mainType, subType};
#include "knx/application/dpt_catalog.inc"
#undef KNX_DPT_CATALOG_ENTRY

} // namespace dptids

namespace dpttags {

#define KNX_DPT_CATALOG_ENTRY(symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
struct symbol {};
#include "knx/application/dpt_catalog.inc"
#undef KNX_DPT_CATALOG_ENTRY

} // namespace dpttags

/**
 * @brief DPT value variant
 */
class DptValue {
public:
    enum class Type {
        Unsupported,
        Boolean,
        Controlled1Bit,
        Controlled3Bit,
        Character,
        Unsigned8,
        Signed8,
        Unsigned16,
        Signed16,
        Float2Byte,
        TimeOfDay,
        Date,
        Unsigned32,
        Signed32,
        Float4Byte,
        AccessControl,
        String,
        SceneNumber,
        SceneControl,
        DateTime,
        HvacMode,
        RgbColor,
        XyYColor,
        HsvColor,
        ColorTransition,
    };

    DptValue()
        : _type(Type::Unsupported)
        , _value(std::monostate{})
    {
    }

    explicit DptValue(bool value)
        : _type(Type::Boolean)
        , _value(value)
    {
    }

    explicit DptValue(Dpt2Value value)
        : _type(Type::Controlled1Bit)
        , _value(value)
    {
    }

    explicit DptValue(Dpt3Value value)
        : _type(Type::Controlled3Bit)
        , _value(value)
    {
    }

    explicit DptValue(char value)
        : _type(Type::Character)
        , _value(value)
    {
    }

    explicit DptValue(uint8_t value)
        : _type(Type::Unsigned8)
        , _value(value)
    {
    }

    explicit DptValue(int8_t value)
        : _type(Type::Signed8)
        , _value(value)
    {
    }

    explicit DptValue(uint16_t value)
        : _type(Type::Unsigned16)
        , _value(value)
    {
    }

    explicit DptValue(int16_t value)
        : _type(Type::Signed16)
        , _value(value)
    {
    }

    explicit DptValue(float value, Type floatType = Type::Float2Byte)
        : _type(floatType)
        , _value(value)
    {
    }

    explicit DptValue(Dpt10Value value)
        : _type(Type::TimeOfDay)
        , _value(value)
    {
    }

    explicit DptValue(Dpt11Value value)
        : _type(Type::Date)
        , _value(value)
    {
    }

    explicit DptValue(uint32_t value)
        : _type(Type::Unsigned32)
        , _value(value)
    {
    }

    explicit DptValue(int32_t value)
        : _type(Type::Signed32)
        , _value(value)
    {
    }

    explicit DptValue(Dpt15Value value)
        : _type(Type::AccessControl)
        , _value(value)
    {
    }

    explicit DptValue(std::string value)
        : _type(Type::String)
        , _value(std::move(value))
    {
    }

    explicit DptValue(Dpt17Value value)
        : _type(Type::SceneNumber)
        , _value(value)
    {
    }

    explicit DptValue(Dpt18Value value)
        : _type(Type::SceneControl)
        , _value(value)
    {
    }

    explicit DptValue(Dpt19Value value)
        : _type(Type::DateTime)
        , _value(value)
    {
    }

    explicit DptValue(Dpt20Mode value)
        : _type(Type::HvacMode)
        , _value(value)
    {
    }

    explicit DptValue(Dpt232Value value)
        : _type(Type::RgbColor)
        , _value(value)
    {
    }

    explicit DptValue(Dpt242Value value)
        : _type(Type::XyYColor)
        , _value(value)
    {
    }

    explicit DptValue(Dpt243Value value)
        : _type(Type::HsvColor)
        , _value(value)
    {
    }

    explicit DptValue(Dpt244Value value)
        : _type(Type::ColorTransition)
        , _value(value)
    {
    }

    Type type() const { return _type; }

    bool asBool() const { return std::get<bool>(_value); }
    const Dpt2Value& asControlled1Bit() const { return std::get<Dpt2Value>(_value); }
    const Dpt3Value& asControlled3Bit() const { return std::get<Dpt3Value>(_value); }
    char asChar() const { return std::get<char>(_value); }
    uint8_t asUInt8() const { return std::get<uint8_t>(_value); }
    uint8_t asInt() const { return std::get<uint8_t>(_value); }
    int8_t asInt8() const { return std::get<int8_t>(_value); }
    uint16_t asUInt16() const { return std::get<uint16_t>(_value); }
    int16_t asInt16() const { return std::get<int16_t>(_value); }
    float asFloat() const { return std::get<float>(_value); }
    const Dpt10Value& asTimeOfDay() const { return std::get<Dpt10Value>(_value); }
    const Dpt11Value& asDate() const { return std::get<Dpt11Value>(_value); }
    uint32_t asUInt32() const { return std::get<uint32_t>(_value); }
    int32_t asInt32() const { return std::get<int32_t>(_value); }
    const Dpt15Value& asAccessControl() const { return std::get<Dpt15Value>(_value); }
    const std::string& asString() const { return std::get<std::string>(_value); }
    const Dpt17Value& asSceneNumber() const { return std::get<Dpt17Value>(_value); }
    const Dpt18Value& asSceneControl() const { return std::get<Dpt18Value>(_value); }
    const Dpt19Value& asDateTime() const { return std::get<Dpt19Value>(_value); }
    Dpt20Mode asHvacMode() const { return std::get<Dpt20Mode>(_value); }
    const Dpt232Value& asRgbColor() const { return std::get<Dpt232Value>(_value); }
    const Dpt242Value& asXyYColor() const { return std::get<Dpt242Value>(_value); }
    const Dpt243Value& asHsvColor() const { return std::get<Dpt243Value>(_value); }
    const Dpt244Value& asColorTransition() const { return std::get<Dpt244Value>(_value); }

    template <typename T>
    const T* tryGet() const
    {
        return std::get_if<T>(&_value);
    }

private:
    Type _type;
    std::variant<std::monostate,
                 bool,
                 Dpt2Value,
                 Dpt3Value,
                 char,
                 uint8_t,
                 int8_t,
                 uint16_t,
                 int16_t,
                 float,
                 Dpt10Value,
                 Dpt11Value,
                 uint32_t,
                 int32_t,
                 Dpt15Value,
                 std::string,
                 Dpt17Value,
                 Dpt18Value,
                 Dpt19Value,
                 Dpt20Mode,
                 Dpt232Value,
                 Dpt242Value,
                 Dpt243Value,
                 Dpt244Value> _value;
};

using DptEncodeDynamicFn = util::Result<size_t> (*)(const DptValue&, std::span<uint8_t>);
using DptDecodeDynamicFn = util::Result<DptValue> (*)(std::span<const uint8_t>);

/**
 * @brief Catalog strings (`DptInfo::name` / `DptInfo::text`).
 *
 * The exporter and host tooling need the human-readable catalog; firmware does
 * not, and on a device the strings are pure flash cost for text nothing ever
 * reads.  Define KNX_DPT_CATALOG_STRINGS=0 to drop them (the pointers become
 * null and the string data disappears from the image).
 */
#ifndef KNX_DPT_CATALOG_STRINGS
#  define KNX_DPT_CATALOG_STRINGS 1
#endif

#if KNX_DPT_CATALOG_STRINGS
#  define KNX_DPT_NAME(literal) literal
#  define KNX_DPT_TEXT(literal) literal
#else
#  define KNX_DPT_NAME(literal) nullptr
#  define KNX_DPT_TEXT(literal) nullptr
#endif

/**
 * @brief Metadata for one catalog datapoint type.
 *
 * Deliberately holds no encode/decode function pointers.  A table of them makes
 * every codec reachable from a single symbol, which defeats linker garbage
 * collection: a product using four datapoint types would still link all of
 * them.  encodeDptValue()/decodeDptValue() dispatch on `valueType` instead.
 */
struct DptInfo {
    DptId id{};
    const char* name{nullptr};
    const char* text{nullptr};
    DptValue::Type valueType{DptValue::Type::Unsupported};
    bool runtimeSupported{false};
};

/**
 * @brief Canonical on-the-wire payload width of a datapoint type, in bits.
 *
 * Sub-octet types (DPT 1/2/3) return 1, 2 and 4; everything else returns a
 * multiple of 8.  Returns 0 for a DPT whose width this stack does not know.
 *
 * This is the width the KNX Group Object Descriptor and the ETS `ObjectSize`
 * attribute are derived from, so it must not depend on an object's current
 * value — an unset 2-byte float is still a 2-byte object.
 */
constexpr uint16_t dptPayloadBits(DptId id) noexcept {
    switch (id.main) {
        case DptMainType::Boolean:         return 1;
        case DptMainType::Controlled1Bit:  return 2;
        case DptMainType::Controlled3Bit:  return 4;
        case DptMainType::Character:
        case DptMainType::Unsigned8:
        case DptMainType::Signed8:
        case DptMainType::SceneNumber:
        case DptMainType::SceneControl:
        case DptMainType::HvacMode:
        case DptMainType::Bitfield8:       return 8;
        case DptMainType::Unsigned16:
        case DptMainType::Signed16:
        case DptMainType::Float2Byte:
        case DptMainType::Bitfield16:      return 16;
        case DptMainType::TimeOfDay:
        case DptMainType::Date:            return 24;
        case DptMainType::Unsigned32:
        case DptMainType::Signed32:
        case DptMainType::Float4Byte:
        case DptMainType::AccessControl:   return 32;
        case DptMainType::RgbColor:        return 24;
        case DptMainType::String:          return 112;  // 14 octets
        case DptMainType::DateTime:        return 64;   // 8 octets
        case DptMainType::XyYColor:        return 48;   // 6 octets
        case DptMainType::HsvColor:        return 24;   // 3 octets
        case DptMainType::ColorTransition: return 48;   // 6 octets
        case DptMainType::Unsupported:     break;
    }
    return 0;
}

/// Canonical payload width in whole octets, rounding sub-octet types up to 1.
constexpr uint16_t dptPayloadOctets(DptId id) noexcept {
    const uint16_t bits = dptPayloadBits(id);
    return bits == 0 ? 0u : static_cast<uint16_t>((bits + 7u) / 8u);
}

/**
 * @brief Widest payload any catalog datapoint type occupies, in octets.
 *
 * Computed from the catalog itself rather than written down, so adding a wide
 * DPT cannot silently outgrow a buffer sized against it. Currently 14 (DPT 16,
 * the 14-character string).
 */
consteval uint16_t maxCatalogPayloadOctets()
{
    uint16_t widest = 0;
#define KNX_DPT_CATALOG_ENTRY(symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
    if (const uint16_t octets = dptPayloadOctets(dptids::symbol); octets > widest) {                            \
        widest = octets;                                                                                        \
    }
#include "knx/application/dpt_catalog.inc"
#undef KNX_DPT_CATALOG_ENTRY
    return widest;
}

const DptInfo* lookupDptInfo(DptId id);
const DptInfo* dptRegistryEntries();
size_t dptRegistrySize();
bool supportsDpt(DptId id);
bool dptValueMatches(DptId id, const DptValue& value);
util::Result<size_t> encodeDptValue(DptId id, const DptValue& value, std::span<uint8_t> data);
util::Result<DptValue> decodeDptValue(DptId id, std::span<const uint8_t> data);

/// Extract a scalar magnitude from a DPT value for change-threshold comparison.
/// Returns true and writes the value (in engineering units) for numeric types
/// (bool, (un)signed 8/16/32, 2- and 4-byte float, character, scene number);
/// returns false for non-scalar types (strings, colours, date/time, …), which
/// callers treat as "any byte change counts as a change".
bool dptValueAsScalar(const DptValue& value, double& out);

template <typename>
struct DptTraits;

template <typename>
struct DependentFalse : std::false_type {};

#define KNX_DPT_CATALOG_ENTRY(symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
template <> \
struct DptTraits<dpttags::symbol> { \
    using value_type = cppType; \
    static inline constexpr DptId kId = dptids::symbol; \
    static inline constexpr DptValue::Type kValueType = DptValue::Type::valueType; \
\
    static util::Result<size_t> encode(value_type value, std::span<uint8_t> data); \
    static util::Result<value_type> decode(std::span<const uint8_t> data); \
    static util::Result<size_t> encodeDynamic(const DptValue& value, std::span<uint8_t> data); \
    static util::Result<DptValue> decodeDynamic(std::span<const uint8_t> data); \
};
#include "knx/application/dpt_catalog.inc"
#undef KNX_DPT_CATALOG_ENTRY

template <DptMainType>
struct DptMainTypeTraits;

template <typename DptTag>
inline bool dptValueMatchesExact(const DptValue& value)
{
    return value.type() == DptTraits<DptTag>::kValueType;
}

template <typename DptTag>
inline DptValue makeDptValue(typename DptTraits<DptTag>::value_type value)
{
    return DptValue(value);
}

template <typename DptTag>
inline util::Result<typename DptTraits<DptTag>::value_type> dptValueAs(const DptValue& value)
{
    if (!dptValueMatchesExact<DptTag>(value)) {
        return util::ErrorCode::OperationNotSupported;
    }

    using ValueType = typename DptTraits<DptTag>::value_type;
    const auto* stored = value.template tryGet<ValueType>();
    if (stored == nullptr) {
        return util::ErrorCode::OperationNotSupported;
    }

    return *stored;
}

template <>
struct DptMainTypeTraits<DptMainType::Boolean> {
    using value_type = bool;
    static inline constexpr DptId kId{DptMainType::Boolean, 0};
    static inline constexpr DptValue::Type kValueType = DptValue::Type::Boolean;

    static util::Result<size_t> encode(value_type value, std::span<uint8_t> data)
    {
        return DptTraits<dpttags::Bool>::encode(value, data);
    }

    static util::Result<value_type> decode(std::span<const uint8_t> data)
    {
        return DptTraits<dpttags::Bool>::decode(data);
    }
};

template <>
struct DptMainTypeTraits<DptMainType::Unsigned8> {
    using value_type = uint8_t;
    static inline constexpr DptId kId{DptMainType::Unsigned8, 0};
    static inline constexpr DptValue::Type kValueType = DptValue::Type::Unsigned8;

    static util::Result<size_t> encode(value_type value, std::span<uint8_t> data)
    {
        return DptTraits<dpttags::Unsigned8>::encode(value, data);
    }

    static util::Result<value_type> decode(std::span<const uint8_t> data)
    {
        return DptTraits<dpttags::Unsigned8>::decode(data);
    }
};

template <>
struct DptMainTypeTraits<DptMainType::Float2Byte> {
    using value_type = float;
    static inline constexpr DptId kId{DptMainType::Float2Byte, 0};
    static inline constexpr DptValue::Type kValueType = DptValue::Type::Float2Byte;

    static util::Result<size_t> encode(value_type value, std::span<uint8_t> data)
    {
        return DptTraits<dpttags::Float2Byte>::encode(value, data);
    }

    static util::Result<value_type> decode(std::span<const uint8_t> data)
    {
        return DptTraits<dpttags::Float2Byte>::decode(data);
    }
};

/**
 * @brief DPT 1 - Boolean
 */
class Dpt1 {
public:
    static util::Result<size_t> encode(bool value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, bool& value);
};

/**
 * @brief DPT 5 - Unsigned integer (0-255)
 */
class Dpt5 {
public:
    static util::Result<size_t> encode(uint8_t value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, uint8_t& value);
};

/**
 * @brief DPT 9 - Float (2 bytes)
 */
class Dpt9 {
public:
    static util::Result<size_t> encode(float value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, float& value);
};

/**
 * @brief DPT 2 - 1-bit controlled (priority/force)
 * 
 * 2 bits: control (bit 1) + value (bit 0)
 * Used for controlled switching with priority override
 */
class Dpt2 {
public:
    using Value = Dpt2Value;
    
    static util::Result<size_t> encode(bool control, bool value, std::span<uint8_t> data);
    static util::Result<size_t> encode(const Value& val, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& value);
};

/**
 * @brief DPT 3 - 3-bit controlled (dimming/blinds)
 * 
 * 4 bits: control (bit 3) + step code (bits 0-2)
 * Used for increase/decrease control (dimming, blinds)
 */
class Dpt3 {
public:
    using Value = Dpt3Value;
    
    static util::Result<size_t> encode(bool control, uint8_t stepCode, std::span<uint8_t> data);
    static util::Result<size_t> encode(const Value& val, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& value);
};

/**
 * @brief DPT 4 - Character (ASCII)
 * 
 * 8 bits: ASCII character (ISO-8859-1)
 */
class Dpt4 {
public:
    static util::Result<size_t> encode(char value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, char& value);
};

/**
 * @brief DPT 6 - 8-bit signed (-128 to 127)
 * 
 * Used for relative values, percentages with sign
 */
class Dpt6 {
public:
    static util::Result<size_t> encode(int8_t value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, int8_t& value);
};

/**
 * @brief DPT 7 - 16-bit unsigned (0 to 65535)
 * 
 * Used for counters, time periods in ms
 */
class Dpt7 {
public:
    static util::Result<size_t> encode(uint16_t value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, uint16_t& value);
};

/**
 * @brief DPT 8 - 16-bit signed (-32768 to 32767)
 * 
 * Used for delta values, pulses
 */
class Dpt8 {
public:
    static util::Result<size_t> encode(int16_t value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, int16_t& value);
};

/**
 * @brief DPT 10 - Time of Day
 * 
 * 3 bytes: Day of week (3 bits), Hour (5 bits), Minutes (6 bits), Seconds (6 bits)
 * Format: [0 0 0 D D D H H] [H H H M M M M M] [M S S S S S S S]
 * Day: 0=no day, 1=Monday, 2=Tuesday, ..., 7=Sunday
 * Hour: 0-23, Minutes: 0-59, Seconds: 0-59
 */
class Dpt10 {
public:
    using Value = Dpt10Value;
    
    static util::Result<size_t> encode(const Value& time, std::span<uint8_t> data);
    static util::Result<size_t> encode(uint8_t dayOfWeek, uint8_t hour, uint8_t minute, uint8_t second, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& time);
};

/**
 * @brief DPT 11 - Date
 * 
 * 3 bytes: Day (5 bits), Month (4 bits), Year (7 bits)
 * Format: [0 0 0 D D D D D] [0 0 0 0 M M M M] [0 Y Y Y Y Y Y Y]
 * Day: 1-31, Month: 1-12, Year: 0-99 (offset from 2000, so 0=2000, 99=2099)
 */
class Dpt11 {
public:
    using Value = Dpt11Value;
    
    static util::Result<size_t> encode(const Value& date, std::span<uint8_t> data);
    static util::Result<size_t> encode(uint8_t day, uint8_t month, uint8_t year, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& date);
};

/**
 * @brief DPT 19 - Date Time
 * 
 * 8 bytes: Date + Time combined with additional flags
 * Format: Year (8 bits, 1900-2155), Month (4 bits), Day (5 bits),
 *         Day of week (3 bits), Hour (5 bits), Minutes (6 bits), Seconds (6 bits),
 *         Flags (8 bits): fault, working day, no WD, no year, no date, no DoW, no time, SUTI
 */
class Dpt19 {
public:
    using Value = Dpt19Value;
    
    static util::Result<size_t> encode(const Value& datetime, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& datetime);
};

/**
 * @brief DPT 12 - 32-bit Unsigned Value
 * 
 * 4 bytes: Unsigned integer 0 to 4,294,967,295
 * Used for counters, time periods in seconds
 */
class Dpt12 {
public:
    static util::Result<size_t> encode(uint32_t value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, uint32_t& value);
};

/**
 * @brief DPT 13 - 32-bit Signed Value
 * 
 * 4 bytes: Signed integer -2,147,483,648 to 2,147,483,647
 * Used for delta values, flow rates
 */
class Dpt13 {
public:
    static util::Result<size_t> encode(int32_t value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, int32_t& value);
};

/**
 * @brief DPT 14 - 32-bit Float Value
 * 
 * 4 bytes: IEEE 754 single precision floating point
 * Used for physical values (temperature, pressure, etc.)
 */
class Dpt14 {
public:
    static util::Result<size_t> encode(float value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, float& value);
};

/**
 * @brief DPT 16 - String (ASCII)
 * 
 * 14 bytes: NULL-terminated ASCII string
 * Maximum 13 characters + NULL terminator
 */
class Dpt16 {
public:
    static util::Result<size_t> encode(const std::string& value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, std::string& value);
};

/**
 * @brief DPT 17 - Scene Number
 * 
 * 1 byte: Scene number (0-63) with optional validity flag
 * Bit 7: 0=scene valid, 1=scene invalid
 * Bits 0-5: Scene number (0-63)
 */
class Dpt17 {
public:
    using Value = Dpt17Value;
    
    static util::Result<size_t> encode(uint8_t sceneNumber, std::span<uint8_t> data);
    static util::Result<size_t> encode(const Value& scene, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& scene);
};

/**
 * @brief DPT 15 - Access Control
 * 
 * 4 bytes: Access code (4 digits) + control bits
 * Format: [D3 D2 D1 D0] where each Di = [digit(4bits) E P R C]
 * E=Error, P=Permission, R=Read direction, C=Encryption
 */
class Dpt15 {
public:
    using Value = Dpt15Value;
    
    static util::Result<size_t> encode(const Value& access, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& access);
};

/**
 * @brief DPT 18 - Scene Control
 * 
 * 1 byte: Scene number (0-63) with activate/learn control
 * Bit 7: 0=activate scene, 1=learn scene
 * Bits 5-0: Scene number (0-63)
 */
class Dpt18 {
public:
    using Value = Dpt18Value;

    static util::Result<size_t> encode(const Value& scene, std::span<uint8_t> data);
    static util::Result<size_t> encode(uint8_t sceneNumber, bool learn, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& scene);
    static util::Result<void> decode(std::span<const uint8_t> data, uint8_t& sceneNumber, bool& learn);
};

/**
 * @brief DPT 20 - HVAC Mode
 * 
 * 1 byte: HVAC operating mode
 */
class Dpt20 {
public:
    using Mode = Dpt20Mode;
    using Value = Mode;

    static util::Result<size_t> encode(Mode mode, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Mode& mode);
};

/**
 * @brief DPT 232 - RGB Color
 * 
 * 3 bytes: Red, Green, Blue (0-255 each)
 */
class Dpt232 {
public:
    using Value = Dpt232Value;

    static util::Result<size_t> encode(const Value& color, std::span<uint8_t> data);
    static util::Result<size_t> encode(uint8_t red, uint8_t green, uint8_t blue, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& color);
    static util::Result<void> decode(std::span<const uint8_t> data, uint8_t& red, uint8_t& green, uint8_t& blue);
};

/**
 * @brief DPT 242 - xyY Color (CIE 1931)
 * 
 * 6 bytes: x coordinate (uint16), y coordinate (uint16), brightness (uint8), valid flag (uint8)
 */
class Dpt242 {
public:
    using Value = Dpt242Value;
    
    static util::Result<size_t> encode(const Value& color, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& color);
};

/**
 * @brief DPT 243 - HSV Color
 * 
 * 3 bytes: Hue (0-360°), Saturation (0-100%), Value/Brightness (0-100%)
 */
class Dpt243 {
public:
    using Value = Dpt243Value;

    static util::Result<size_t> encode(const Value& color, std::span<uint8_t> data);
    static util::Result<size_t> encode(uint16_t hue, uint8_t saturation, uint8_t value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& color);
    static util::Result<void> decode(std::span<const uint8_t> data, uint16_t& hue, uint8_t& saturation, uint8_t& value);
};

/**
 * @brief DPT 244 - Color Transition
 * 
 * 6 bytes: Color transition timing information
 */
class Dpt244 {
public:
    using Value = Dpt244Value;
    
    static util::Result<size_t> encode(const Value& transition, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& transition);
};

} // namespace application
} // namespace knx
