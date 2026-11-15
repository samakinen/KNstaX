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
#include "knx/config.hpp"
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
    ColourTransitionXyY = 243,
    ConverterStatus = 244,
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

/// DPT 10.001 DPT_TimeOfDay (03_07_02 §3.11).
struct Dpt10Value {
    uint8_t dayOfWeek{0};  ///< 0 = no day, 1 = Monday … 7 = Sunday
    uint8_t hour{0};       ///< 0…23
    uint8_t minute{0};     ///< 0…59
    uint8_t second{0};     ///< 0…59
};

/// DPT 11.001 DPT_Date (03_07_02 §3.12).
struct Dpt11Value {
    uint8_t day{1};    ///< 1…31
    uint8_t month{1};  ///< 1…12
    /// Two-digit year as carried on the wire, 0…99. Century rule (§3.12):
    /// ≥ 90 is the 20th century, < 90 the 21st, i.e. 1990…2089; see fullYear().
    uint8_t year{0};

    constexpr uint16_t fullYear() const
    {
        return static_cast<uint16_t>(year >= 90u ? 1900u + year : 2000u + year);
    }
};

/// DPT 15.000 DPT_Access_Data (03_07_02 §3.16), U4U4U4U4U4U4B4N4.
struct Dpt15Value {
    /// Access identification code as a decimal number, 0…999 999: its six
    /// digits are D6 (most significant) … D1 on the wire.
    uint32_t accessCode{0};
    bool error{false};          ///< E: reading of the access code failed
    bool permission{false};     ///< P: access accepted
    bool readDirection{false};  ///< D: 0 = left to right, 1 = right to left
    bool encrypted{false};      ///< C: access information is ciphered
    uint8_t index{0};           ///< Index of the access code, 0…15
};

/// DPT 17.001 DPT_SceneNumber (03_07_02 §3.18), r2U6.
struct Dpt17Value {
    uint8_t sceneNumber{0};  ///< 0…63
};

struct Dpt18Value {
    uint8_t sceneNumber{0};
    bool learn{false};
};

/// DPT 19.001 DPT_DateTime (03_07_02 §3.20).
///
/// A field whose "not valid" flag is set (NY, ND, NDoW, NT, NWD) is
/// transmitted as zero and not range-checked on reception.
struct Dpt19Value {
    uint16_t year{1900};       ///< 1900…2155 (octet: year − 1900)
    uint8_t month{1};          ///< 1…12
    uint8_t day{1};            ///< 1…31
    uint8_t dayOfWeek{0};      ///< 0 = any day, 1 = Monday … 7 = Sunday
    uint8_t hour{0};           ///< 0…24; 24 only as 24:00:00 (Note 11)
    uint8_t minute{0};         ///< 0…59
    uint8_t second{0};         ///< 0…59
    bool fault{false};         ///< F
    bool workingDay{false};    ///< WD
    bool noWD{false};          ///< NWD: WD not valid
    bool noYear{false};        ///< NY
    bool noDate{false};        ///< ND: month and day not valid
    bool noDayOfWeek{false};   ///< NDoW
    bool noTime{false};        ///< NT: hour, minutes, seconds not valid
    bool suti{false};          ///< SUTI: summer time (hour already corrected)
    bool externalSync{false};  ///< CLQ: clock with external sync signal
    bool reliableSource{false};///< SRC: reliable synchronisation source
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

/// DPT 242.600 DPT_Colour_xyY (03_07_02 §6.9), U16U16U8r6B2.
struct Dpt242Value {
    uint16_t x{0};               ///< x × 65 535
    uint16_t y{0};               ///< y × 65 535
    uint8_t brightness{0};       ///< as DPT 5.001 (0…255 = 0…100 %)
    bool colourValid{false};     ///< C: x and y valid
    bool brightnessValid{false}; ///< B: brightness valid
};

/// DPT 243.600 DPT_Colour_Transition_xyY (03_07_02 §6.10), U16U16U16U8r6B2.
struct Dpt243Value {
    uint16_t timePeriod{0};      ///< fading time, 100 ms units (as DPT 7.004)
    uint16_t x{0};
    uint16_t y{0};
    uint8_t brightness{0};
    bool colourValid{false};
    bool brightnessValid{false};
};

/// DPT 244.600 DPT_Converter_Status (03_07_02 §6.11), N4B4N2N2N2N2.
struct Dpt244Value {
    uint8_t mode{0};                     ///< CM, 0…9 (10…15 reserved)
    bool hardwiredInhibitActive{false};  ///< HS bit 0
    bool hardwiredSwitchOn{false};       ///< HS bit 1
    uint8_t functionTestPending{0};      ///< FP, 0…2 (3 reserved)
    uint8_t durationTestPending{0};      ///< DP, 0…2
    uint8_t partialDurationTestPending{0}; ///< PP, 0…2
    uint8_t failure{0};                  ///< CF, 0…2
};

namespace dptids {

// The identifier of a type exists whether or not its family is configured in:
// it is a constant, and keeping it is what lets a port that uses a type of an
// absent family fail with a message naming the Kconfig symbol (endpoint
// definition, PortSpec) rather than with an unknown name.
#define KNX_DPT_CATALOG_ENTRY(symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
inline constexpr DptId symbol{DptMainType::mainType, subType};
#define KNX_DPT_CATALOG_OFF(family, symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
inline constexpr DptId symbol{DptMainType::mainType, subType};
#include "knx/application/dpt_catalog.inc"
#undef KNX_DPT_CATALOG_ENTRY
#undef KNX_DPT_CATALOG_OFF

} // namespace dptids

namespace detail {

/// Base of the tag of every catalogue type whose family is configured out; one
/// derived marker per family, so DptTraits can name the symbol to enable.
struct DptFamilyOff {};
struct DptFamilyOffFloat : DptFamilyOff {};
struct DptFamilyOffTime : DptFamilyOff {};
struct DptFamilyOffString : DptFamilyOff {};
struct DptFamilyOffColour : DptFamilyOff {};
struct DptFamilyOffAccess : DptFamilyOff {};

} // namespace detail

namespace dpttags {

#define KNX_DPT_CATALOG_ENTRY(symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
struct symbol {};
#define KNX_DPT_CATALOG_OFF(family, symbol, mainType, subType, valueType, cppType, codecType, shortName, description) \
struct symbol : detail::DptFamilyOff##family {};
#include "knx/application/dpt_catalog.inc"
#undef KNX_DPT_CATALOG_ENTRY
#undef KNX_DPT_CATALOG_OFF

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
        ColourTransitionXyY,
        ConverterStatus,
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
        : _type(Type::ColourTransitionXyY)
        , _value(value)
    {
    }

    explicit DptValue(Dpt244Value value)
        : _type(Type::ConverterStatus)
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
    const Dpt243Value& asColourTransitionXyY() const { return std::get<Dpt243Value>(_value); }
    const Dpt244Value& asConverterStatus() const { return std::get<Dpt244Value>(_value); }

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

#if KNX_FEATURE_DPT_CATALOGUE
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
#endif  // KNX_FEATURE_DPT_CATALOGUE

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
        case DptMainType::ColourTransitionXyY: return 64; // 8 octets
        case DptMainType::ConverterStatus: return 16;   // 2 octets
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

/**
 * @brief The families of datapoint type a build can configure out.
 *
 * Integer, boolean, scene, dimming (DPT 3) and HVAC mode types are always
 * built. The rest follow Kconfig: KNX_DPT_FLOAT (DPT 9, 14), KNX_DPT_TIME
 * (10, 11, 19), KNX_DPT_STRING (4, 16), KNX_DPT_COLOUR (232, 242-244) and
 * KNX_DPT_ACCESS (15).
 */
constexpr bool dptIsFloatFamily(DptId id) noexcept
{
    return id.main == DptMainType::Float2Byte || id.main == DptMainType::Float4Byte;
}
constexpr bool dptIsTimeFamily(DptId id) noexcept
{
    return id.main == DptMainType::TimeOfDay || id.main == DptMainType::Date || id.main == DptMainType::DateTime;
}
constexpr bool dptIsStringFamily(DptId id) noexcept
{
    return id.main == DptMainType::Character || id.main == DptMainType::String;
}
constexpr bool dptIsColourFamily(DptId id) noexcept
{
    return id.main == DptMainType::RgbColor || id.main == DptMainType::XyYColor
        || id.main == DptMainType::ColourTransitionXyY || id.main == DptMainType::ConverterStatus;
}
constexpr bool dptIsAccessFamily(DptId id) noexcept
{
    return id.main == DptMainType::AccessControl;
}

/// True when the codec of @p id's family is part of this build.
constexpr bool dptFamilyEnabled(DptId id) noexcept
{
    return (KNX_FEATURE_DPT_FLOAT || !dptIsFloatFamily(id))
        && (KNX_FEATURE_DPT_TIME || !dptIsTimeFamily(id))
        && (KNX_FEATURE_DPT_STRING || !dptIsStringFamily(id))
        && (KNX_FEATURE_DPT_COLOUR || !dptIsColourFamily(id))
        && (KNX_FEATURE_DPT_ACCESS || !dptIsAccessFamily(id));
}

/**
 * @brief The runtime value type a datapoint type is carried as, from the id alone.
 *
 * Every catalogue entry sharing a DptValue::Type shares its codec, so the type
 * follows from the main type, with the few sub-type exceptions below. This is
 * what the product layer compiles into its descriptors, and what the dynamic
 * encode/decode paths use when the run-time catalogue (KNX_ENABLE_DPT_CATALOGUE)
 * is not built. Returns Unsupported for a main type this stack has no codec
 * for. It does not look at the build's families: see dptFamilyEnabled().
 */
constexpr DptValue::Type dptValueTypeFor(DptId dpt) noexcept
{
    switch (dpt.main) {
        case DptMainType::Boolean:
            return DptValue::Type::Boolean;
        case DptMainType::Controlled1Bit:
            return DptValue::Type::Controlled1Bit;
        case DptMainType::Controlled3Bit:
            return DptValue::Type::Controlled3Bit;
        case DptMainType::Character:
            return DptValue::Type::Character;
        case DptMainType::Unsigned8:
            return DptValue::Type::Unsigned8;
        case DptMainType::Signed8:
            return DptValue::Type::Signed8;
        case DptMainType::Unsigned16:
            return DptValue::Type::Unsigned16;
        case DptMainType::Signed16:
            return DptValue::Type::Signed16;
        case DptMainType::Float2Byte:
            return DptValue::Type::Float2Byte;
        case DptMainType::TimeOfDay:
            return DptValue::Type::TimeOfDay;
        case DptMainType::Date:
            return DptValue::Type::Date;
        case DptMainType::Unsigned32:
            return DptValue::Type::Unsigned32;
        case DptMainType::Signed32:
            return DptValue::Type::Signed32;
        case DptMainType::Float4Byte:
            return DptValue::Type::Float4Byte;
        case DptMainType::AccessControl:
            return DptValue::Type::AccessControl;
        case DptMainType::String:
            return DptValue::Type::String;
        case DptMainType::SceneNumber:
            return DptValue::Type::SceneNumber;
        case DptMainType::SceneControl:
            return DptValue::Type::SceneControl;
        case DptMainType::DateTime:
            return DptValue::Type::DateTime;
        case DptMainType::HvacMode:
            // Only DPT-20.102 (and the bare main type) carry the typed
            // Dpt20Mode enum; every other DPT-20 sub-type is transported as a
            // raw byte (see dpt_catalog.inc), so its runtime value type is
            // Unsigned8 to match the port's uint8_t storage.
            return (dpt.sub == 0 || dpt.sub == 102) ? DptValue::Type::HvacMode : DptValue::Type::Unsigned8;
        case DptMainType::Bitfield8:
            return DptValue::Type::Unsigned8;
        case DptMainType::Bitfield16:
            return DptValue::Type::Unsigned16;
        case DptMainType::RgbColor:
            return DptValue::Type::RgbColor;
        case DptMainType::XyYColor:
            return DptValue::Type::XyYColor;
        case DptMainType::ColourTransitionXyY:
            return DptValue::Type::ColourTransitionXyY;
        case DptMainType::ConverterStatus:
            return DptValue::Type::ConverterStatus;
        case DptMainType::Unsupported:
            break;
    }
    return DptValue::Type::Unsupported;
}

#if KNX_FEATURE_DPT_CATALOGUE
const DptInfo* lookupDptInfo(DptId id);
const DptInfo* dptRegistryEntries();
size_t dptRegistrySize();
#endif
bool supportsDpt(DptId id);
bool dptValueMatches(DptId id, const DptValue& value);
util::Result<size_t> encodeDptValue(DptId id, const DptValue& value, std::span<uint8_t> data);
util::Result<DptValue> decodeDptValue(DptId id, std::span<const uint8_t> data);

/// Extract a scalar magnitude from a DPT value for change-threshold comparison.
/// Returns true and writes the value (in engineering units) for numeric types
/// (bool, (un)signed 8/16/32, 2- and 4-byte float, character, scene number);
/// returns false for non-scalar types (strings, colours, date/time, …), which
/// callers treat as "any byte change counts as a change".
#if KNX_FEATURE_DPT_FLOAT
bool dptValueAsScalar(const DptValue& value, double& out);
#endif

/// Integer magnitude of a boolean or (un)signed 8/16/32-bit value, for the
/// integer change threshold of a build without KNX_DPT_FLOAT (no double
/// arithmetic). False for every other type.
bool dptValueAsInteger(const DptValue& value, int64_t& out);

template <typename>
struct DependentFalse : std::false_type {};

/**
 * @brief Codec traits of one catalogue type, specialised per catalogue entry.
 *
 * The primary template is what a tag without a specialisation gets: the tag of
 * a type whose family is configured out of the build, or not a catalogue tag
 * at all. Using it is a compile error that names the Kconfig symbol to enable.
 */
template <typename Tag>
struct DptTraits {
    static_assert(!std::is_base_of_v<detail::DptFamilyOffFloat, Tag>,
                  "DPT 9 and 14 (floating point) are not part of this build: enable KNX_DPT_FLOAT (CONFIG_KNX_DPT_FLOAT=y)");
    static_assert(!std::is_base_of_v<detail::DptFamilyOffTime, Tag>,
                  "DPT 10, 11 and 19 (time and date) are not part of this build: enable KNX_DPT_TIME (CONFIG_KNX_DPT_TIME=y)");
    static_assert(!std::is_base_of_v<detail::DptFamilyOffString, Tag>,
                  "DPT 4 and 16 (character and string) are not part of this build: enable KNX_DPT_STRING (CONFIG_KNX_DPT_STRING=y)");
    static_assert(!std::is_base_of_v<detail::DptFamilyOffColour, Tag>,
                  "DPT 232 and 242-244 (colour) are not part of this build: enable KNX_DPT_COLOUR (CONFIG_KNX_DPT_COLOUR=y)");
    static_assert(!std::is_base_of_v<detail::DptFamilyOffAccess, Tag>,
                  "DPT 15 (access identification) is not part of this build: enable KNX_DPT_ACCESS (CONFIG_KNX_DPT_ACCESS=y)");
    static_assert(std::is_base_of_v<detail::DptFamilyOff, Tag>,
                  "DptTraits needs a tag from knx::application::dpttags");

    // Placeholders so that the one static_assert above is the only diagnostic.
    using value_type = int;
    static inline constexpr DptId kId{};
    static inline constexpr DptValue::Type kValueType = DptValue::Type::Unsupported;
};

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

#if KNX_FEATURE_DPT_FLOAT
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
#endif

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
 * @brief DPT 9 - Float (2 bytes), 03_07_02 §3.10
 *
 * FloatValue = 0.01 * M * 2^E, M 12-bit two's complement, E 0…15, encoded with
 * the smallest exponent that fits. 7FFFh denotes invalid data: it decodes to a
 * quiet NaN and a NaN encodes to it. ±Inf and values outside
 * [-671 088.64 … 670 433.28] are rejected.
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
 * @brief DPT 10.001 DPT_TimeOfDay (03_07_02 §3.11)
 *
 * 3 octets N3U5r2U6r2U6: [N N N H H H H H] [0 0 M M M M M M] [0 0 S S S S S S]
 * Day: 0 = no day, 1 = Monday … 7 = Sunday; hour 0…23, minutes 0…59,
 * seconds 0…59. Reserved bits are sent as 0 and ignored on reception.
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
 * Day: 1-31, Month: 1-12, Year: 0-99 (century rule of 03_07_02 §3.12:
 * 90…99 = 1990…1999, 0…89 = 2000…2089; see Dpt11Value::fullYear()).
 */
class Dpt11 {
public:
    using Value = Dpt11Value;
    
    static util::Result<size_t> encode(const Value& date, std::span<uint8_t> data);
    static util::Result<size_t> encode(uint8_t day, uint8_t month, uint8_t year, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& date);
};

/**
 * @brief DPT 19.001 DPT_DateTime (03_07_02 §3.20)
 *
 * 8 octets U8[r4U4][r3U5][U3U5][r2U6][r2U6]B16, MSB first:
 *   [year − 1900] [0000 month] [000 day] [DoW(3) hour(5)] [00 minutes]
 *   [00 seconds] [F WD NWD NY ND NDoW NT SUTI] [CLQ SRC 000000]
 * Hour 24 is allowed only as 24:00:00 (Note 11); a frame with hour 24 and
 * non-zero minutes or seconds is rejected. The six reserved bits of the last
 * octet must be 0 (Note 15: "Receivers shall check these bits to be 0").
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
 * 14 octets A112 (03_07_02 §3.17): up to 14 characters, unused trailing
 * octets set to NUL. A 14-character string has no terminator on the wire.
 */
class Dpt16 {
public:
    static util::Result<size_t> encode(const std::string& value, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, std::string& value);
};

/**
 * @brief DPT 17.001 DPT_SceneNumber (03_07_02 §3.18)
 *
 * 1 octet r2U6: [0 0 S S S S S S], scene number 0…63. The two reserved bits
 * are sent as 0 and ignored on reception.
 */
class Dpt17 {
public:
    using Value = Dpt17Value;
    
    static util::Result<size_t> encode(uint8_t sceneNumber, std::span<uint8_t> data);
    static util::Result<size_t> encode(const Value& scene, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& scene);
};

/**
 * @brief DPT 15.000 DPT_Access_Data (03_07_02 §3.16)
 *
 * 4 octets U4U4U4U4U4U4B4N4: [D6 D5] [D4 D3] [D2 D1] [E P D C Index]
 * Each digit 0…9; E = detection error, P = permission, D = read direction,
 * C = encryption, Index 0…15.
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
 * @brief DPT 242.600 DPT_Colour_xyY (03_07_02 §6.9)
 *
 * 6 octets U16U16U8r6B2: x, y, brightness, then [0 0 0 0 0 0 C B] with
 * C = colour (x/y) valid and B = brightness valid.
 */
class Dpt242 {
public:
    using Value = Dpt242Value;
    
    static util::Result<size_t> encode(const Value& color, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& color);
};

/**
 * @brief DPT 243.600 DPT_Colour_Transition_xyY (03_07_02 §6.10)
 *
 * 8 octets U16U16U16U8r6B2: time period (100 ms), x, y, brightness, then
 * [0 0 0 0 0 0 C B]. The field order follows the clause's field table
 * (Time Period, x-axis, y-axis, Brightness) and xknx; the octet diagram in
 * the same clause is typeset ambiguously.
 */
class Dpt243 {
public:
    using Value = Dpt243Value;

    static util::Result<size_t> encode(const Value& transition, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& transition);
};

/**
 * @brief DPT 244.600 DPT_Converter_Status (03_07_02 §6.11)
 *
 * 2 octets N4B4N2N2N2N2: [CM(4) 0 0 HS1 HS0] [FP(2) DP(2) PP(2) CF(2)].
 * Reserved values (CM 10…15, FP/DP/PP/CF 3, HS bits 2/3) are rejected.
 */
class Dpt244 {
public:
    using Value = Dpt244Value;

    static util::Result<size_t> encode(const Value& status, std::span<uint8_t> data);
    static util::Result<void> decode(std::span<const uint8_t> data, Value& status);
};

} // namespace application
} // namespace knx
