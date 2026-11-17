// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen
/**
 * @file parameter_layout.hpp
 * @brief ETS parameters as a byte image with a compile-time layout
 *
 * Prototype (step P0) of docs/design/parameter-model.md. It stands beside the
 * StaticParameterSchema of commissioned_product.hpp and is not yet wired into
 * the runtime or the exporter.
 *
 * A product declares parameter blocks, each with its own id enum, and a layout
 * that lists the blocks and how often each repeats:
 *
 *   enum class In : uint8_t { Function, DebounceMs };
 *   inline constexpr auto kInputBlock = params::parameterBlock<In>(
 *       {.key = "input", .name = "Input {n}"},
 *       params::param<In::Function, InputFunction>({.name = "Function", .options = kFunctionOptions}),
 *       params::param<In::DebounceMs, uint8_t>({.name = "Debounce", .def = 30, .min = 10, .max = 200}));
 *   inline constexpr auto kParameters = params::parameterLayout(kDeviceBlock, params::repeat<8>(kInputBlock));
 *
 * From that declaration come, at compile time: every offset, the image size,
 * the default image, a layout fingerprint and a bounds table.
 *
 * Firmware reads through a view over the image:
 *
 *   params::ViewFor<kParameters> view(image);
 *   auto function = view.block<In>(channel).get<In::Function>();
 *
 * Firmware code must name the layout only as a type or a template argument
 * (ViewFor, kDefaultImageOf, kBoundsOf). Taking a run-time reference to the
 * layout object links every name, option and condition into the image.
 */

#pragma once

#include "knx/product/export_descriptor.hpp"

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace knx::product::params {

// ---------------------------------------------------------------------------
// Value types
// ---------------------------------------------------------------------------

namespace detail {

template <typename V>
concept SmallEnum = std::is_enum_v<V> && sizeof(V) <= 2u;

} // namespace detail

/// What a parameter can hold: bool, 8- and 16-bit integers, enums over those,
/// Dpt9Float (2 octets) and float (4 octets).
template <typename V>
concept ParameterValue = std::same_as<V, bool> || std::same_as<V, uint8_t> || std::same_as<V, uint16_t>
                         || std::same_as<V, int16_t> || std::same_as<V, float> || std::same_as<V, Dpt9Float>
                         || detail::SmallEnum<V>;

/// Octets a value of type V takes in the image.
template <ParameterValue V>
inline constexpr size_t kWidth = std::same_as<V, bool>        ? 1u
                                 : std::same_as<V, float>     ? 4u
                                 : std::same_as<V, Dpt9Float> ? 2u
                                                              : sizeof(V);

namespace detail {

template <ParameterValue V>
constexpr ExportParameterValueKind kindOf()
{
    if (std::same_as<V, bool>) {
        return ExportParameterValueKind::Boolean;
    }
    if (std::same_as<V, uint8_t>) {
        return ExportParameterValueKind::Unsigned8;
    }
    if (std::same_as<V, uint16_t>) {
        return ExportParameterValueKind::Unsigned16;
    }
    if (std::same_as<V, int16_t>) {
        return ExportParameterValueKind::Signed16;
    }
    if (std::same_as<V, float>) {
        return ExportParameterValueKind::Float;
    }
    if (std::same_as<V, Dpt9Float>) {
        return ExportParameterValueKind::FloatDpt9;
    }
    return ExportParameterValueKind::Enum;
}

/// The value as a plain integer, for options, conditions and bounds.
template <typename V>
constexpr int32_t toInteger(V value)
{
    if constexpr (std::is_enum_v<V>) {
        return static_cast<int32_t>(static_cast<std::underlying_type_t<V>>(value));
    } else {
        return static_cast<int32_t>(value);
    }
}

/// KNX DPT 9 encoding of a float: 0.01 × mantissa × 2^exponent, mantissa in
/// 12-bit two's complement. Usable in constant expressions, which the shared
/// run-time codec is not.
constexpr uint16_t encodeDpt9(float value)
{
    const float scaled = value * 100.0f;
    auto mantissa = static_cast<int32_t>(scaled < 0.0f ? scaled - 0.5f : scaled + 0.5f);
    uint32_t exponent = 0;
    while ((mantissa < -2048 || mantissa > 2047) && exponent < 15u) {
        mantissa >>= 1;
        ++exponent;
    }
    return static_cast<uint16_t>(((mantissa < 0) ? 0x8000u : 0u) | (exponent << 11u)
                                 | (static_cast<uint32_t>(mantissa) & 0x07FFu));
}

inline float decodeDpt9(uint16_t raw) noexcept
{
    int32_t mantissa = static_cast<int32_t>(raw & 0x07FFu);
    if ((raw & 0x8000u) != 0u) {
        mantissa -= 2048;
    }
    const uint32_t exponent = (raw >> 11u) & 0x0Fu;
    return 0.01f * static_cast<float>(mantissa * (int32_t{1} << exponent));
}

template <ParameterValue V>
constexpr void encode(V value, uint8_t* out)
{
    if constexpr (std::same_as<V, float>) {
        const auto raw = std::bit_cast<uint32_t>(value);
        out[0] = static_cast<uint8_t>(raw >> 24u);
        out[1] = static_cast<uint8_t>(raw >> 16u);
        out[2] = static_cast<uint8_t>(raw >> 8u);
        out[3] = static_cast<uint8_t>(raw);
    } else if constexpr (std::same_as<V, Dpt9Float>) {
        const uint16_t raw = encodeDpt9(value.value);
        out[0] = static_cast<uint8_t>(raw >> 8u);
        out[1] = static_cast<uint8_t>(raw);
    } else if constexpr (kWidth<V> == 1u) {
        out[0] = static_cast<uint8_t>(toInteger(value));
    } else {
        const auto raw = static_cast<uint16_t>(toInteger(value));
        out[0] = static_cast<uint8_t>(raw >> 8u);
        out[1] = static_cast<uint8_t>(raw);
    }
}

template <ParameterValue V>
inline V decode(const uint8_t* in) noexcept
{
    if constexpr (std::same_as<V, bool>) {
        return in[0] != 0u;
    } else if constexpr (std::same_as<V, float>) {
        const uint32_t raw = (static_cast<uint32_t>(in[0]) << 24u) | (static_cast<uint32_t>(in[1]) << 16u)
                             | (static_cast<uint32_t>(in[2]) << 8u) | in[3];
        return std::bit_cast<float>(raw);
    } else if constexpr (std::same_as<V, Dpt9Float>) {
        return Dpt9Float{decodeDpt9(static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8u) | in[1]))};
    } else if constexpr (kWidth<V> == 1u) {
        return static_cast<V>(in[0]);
    } else {
        const auto raw = static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8u) | in[1]);
        if constexpr (std::is_enum_v<V>) {
            return static_cast<V>(static_cast<std::underlying_type_t<V>>(raw));
        } else {
            return static_cast<V>(raw);
        }
    }
}

template <typename IdEnum, size_t N>
consteval size_t indexOfId(std::array<IdEnum, N> ids, IdEnum id)
{
    for (size_t i = 0; i < N; ++i) {
        if (ids[i] == id) {
            return i;
        }
    }
    return N;
}

template <typename IdEnum, size_t N>
consteval bool idsUnique(std::array<IdEnum, N> ids)
{
    for (size_t a = 0; a < N; ++a) {
        for (size_t b = a + 1u; b < N; ++b) {
            if (ids[a] == ids[b]) {
                return false;
            }
        }
    }
    return true;
}

template <size_t N>
consteval size_t sumBefore(std::array<size_t, N> sizes, size_t index)
{
    size_t sum = 0;
    for (size_t i = 0; i < index && i < N; ++i) {
        sum += sizes[i];
    }
    return sum;
}

template <typename... Ts>
    requires(sizeof...(Ts) <= 1u)
consteval bool allDistinct()
{
    return true;
}

template <typename T, typename... Rest>
    requires(sizeof...(Rest) >= 1u)
consteval bool allDistinct()
{
    return (!std::same_as<T, Rest> && ...) && allDistinct<Rest...>();
}

/// Index of the single true entry; N when none is, N + 1 when several are.
template <size_t N>
consteval size_t entryOf(std::array<bool, N> match)
{
    size_t found = N;
    for (size_t i = 0; i < N; ++i) {
        if (match[i]) {
            if (found != N) {
                return N + 1u;
            }
            found = i;
        }
    }
    return found;
}

/// Its address stands for the block whose id enum is IdEnum, where a type
/// cannot be stored (conditions in constexpr data).
template <typename IdEnum>
inline constexpr char kBlockTag{};

} // namespace detail

// ---------------------------------------------------------------------------
// Export-only data: options and conditions
// ---------------------------------------------------------------------------

/// One selectable value of an enumerated parameter (an ETS drop-down entry).
struct Option {
    int32_t value{0};
    std::string_view label{};
};

template <typename V>
struct TypedOption {
    V value;
    std::string_view label;
};

/**
 * @brief A named option list several parameters can share.
 *
 *   inline constexpr auto kOnOff = params::options<bool>({{false, "Off"}, {true, "On"}});
 */
template <typename V, size_t N>
constexpr std::array<Option, N> options(const TypedOption<V> (&list)[N])
{
    std::array<Option, N> result{};
    for (size_t i = 0; i < N; ++i) {
        result[i] = Option{detail::toInteger(list[i].value), list[i].label};
    }
    return result;
}

enum class ConditionOp : uint8_t {
    Always,
    AnyOf,    ///< the controlling parameter holds one of the values
    NotEqual, ///< it holds anything but values[0]
    Range,    ///< values[0] <= it <= values[1]
};

inline constexpr size_t kMaxConditionValues = 8;

/// When a parameter, a block or an object is shown in ETS. Export data only:
/// the device never evaluates it, and a hidden parameter keeps its value.
struct Condition {
    ConditionOp op{ConditionOp::Always};
    /// Block of the controlling parameter (address of its tag).
    const void* block{nullptr};
    uint16_t parameterId{0};
    std::array<int32_t, kMaxConditionValues> values{};
    uint8_t valueCount{0};
};

template <auto Id, typename... Vs>
    requires(sizeof...(Vs) >= 1u && sizeof...(Vs) <= kMaxConditionValues)
constexpr Condition whenAny(Vs... values)
{
    Condition condition{ConditionOp::AnyOf, &detail::kBlockTag<decltype(Id)>,
                        static_cast<uint16_t>(detail::toInteger(Id))};
    size_t at = 0;
    ((condition.values[at++] = detail::toInteger(values)), ...);
    condition.valueCount = static_cast<uint8_t>(sizeof...(Vs));
    return condition;
}

template <auto Id, typename V>
constexpr Condition when(V value)
{
    return whenAny<Id>(value);
}

template <auto Id, typename V>
constexpr Condition whenNot(V value)
{
    Condition condition = whenAny<Id>(value);
    condition.op = ConditionOp::NotEqual;
    return condition;
}

template <auto Id, typename V>
constexpr Condition whenRange(V low, V high)
{
    Condition condition = whenAny<Id>(low, high);
    condition.op = ConditionOp::Range;
    return condition;
}

// ---------------------------------------------------------------------------
// Parameters and blocks
// ---------------------------------------------------------------------------

/// Everything a parameter declares beside its id and type.
template <ParameterValue V>
struct ParamSpec {
    /// Label ETS shows.
    std::string_view name{};
    V def{};
    /// Inclusive bounds. Equal values mean "not set": the bounds are then the
    /// option values, or the range of the type.
    double min{0.0};
    double max{0.0};
    std::string_view unit{};
    std::span<const Option> options{};
    Condition visible{};
    /// Technical key for the export; empty derives one from the block key and the id.
    std::string_view key{};
};

template <auto Id, ParameterValue V>
struct Param {
    using id_type = decltype(Id);
    using value_type = V;
    static constexpr auto id = Id;

    ParamSpec<V> spec{};
};

template <auto Id, ParameterValue V>
constexpr Param<Id, V> param(ParamSpec<V> spec)
{
    return Param<Id, V>{spec};
}

struct BlockSpec {
    std::string_view key{};
    /// Section or channel name. "{n}" is the 1-based instance of a repeated block.
    std::string_view name{};
    Condition visible{};
};

template <typename IdEnum, typename... Ps>
struct ParameterBlock {
    static_assert(std::is_enum_v<IdEnum>, "a parameter block is identified by its id enum");
    static_assert((std::same_as<typename Ps::id_type, IdEnum> && ...),
                  "every parameter of a block uses the block's id enum");

    using id_type = IdEnum;
    static constexpr size_t kParameterCount = sizeof...(Ps);
    /// Octets one instance of the block takes.
    static constexpr size_t kStride = (size_t{0} + ... + kWidth<typename Ps::value_type>);

    static_assert(detail::idsUnique<IdEnum, sizeof...(Ps)>({Ps::id...}),
                  "parameter ids must be unique within a block");

    template <IdEnum Id>
    static constexpr size_t kIndex = detail::indexOfId<IdEnum, sizeof...(Ps)>({Ps::id...}, Id);

    template <IdEnum Id>
        requires(kIndex<Id> < sizeof...(Ps))
    using value_type_of = typename std::tuple_element_t<kIndex<Id>, std::tuple<Ps...>>::value_type;

    /// Offset of a parameter from the start of its block instance.
    template <IdEnum Id>
        requires(kIndex<Id> < sizeof...(Ps))
    static constexpr size_t kOffset =
        detail::sumBefore<sizeof...(Ps)>({kWidth<typename Ps::value_type>...}, kIndex<Id>);

    BlockSpec spec{};
    std::tuple<Ps...> parameters{};
};

template <typename IdEnum, typename... Ps>
constexpr ParameterBlock<IdEnum, Ps...> parameterBlock(BlockSpec spec, Ps... parameters)
{
    return ParameterBlock<IdEnum, Ps...>{spec, std::tuple<Ps...>{parameters...}};
}

/// A block that appears N times in the layout, one after the other.
template <size_t N, typename BlockT>
struct Repeated {
    static_assert(N >= 1u, "a block appears at least once");
    using block_type = BlockT;
    static constexpr size_t kCount = N;
    static constexpr size_t kBytes = N * BlockT::kStride;

    BlockT block{};
};

template <size_t N, typename IdEnum, typename... Ps>
constexpr Repeated<N, ParameterBlock<IdEnum, Ps...>> repeat(const ParameterBlock<IdEnum, Ps...>& block)
{
    return Repeated<N, ParameterBlock<IdEnum, Ps...>>{block};
}

namespace detail {

template <typename IdEnum, typename... Ps>
constexpr Repeated<1, ParameterBlock<IdEnum, Ps...>> asRepeated(const ParameterBlock<IdEnum, Ps...>& block)
{
    return Repeated<1, ParameterBlock<IdEnum, Ps...>>{block};
}

template <size_t N, typename BlockT>
constexpr Repeated<N, BlockT> asRepeated(const Repeated<N, BlockT>& repeated)
{
    return repeated;
}

constexpr uint32_t fnv1a(uint32_t hash, uint32_t value)
{
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
        hash ^= (value >> shift) & 0xFFu;
        hash *= 16777619u;
    }
    return hash;
}

template <typename IdEnum, typename... Ps>
constexpr uint32_t fingerprintBlock(uint32_t hash, std::type_identity<ParameterBlock<IdEnum, Ps...>>)
{
    ((hash = fnv1a(fnv1a(hash, static_cast<uint32_t>(toInteger(Ps::id))),
                   (static_cast<uint32_t>(kindOf<typename Ps::value_type>()) << 8u)
                       | static_cast<uint32_t>(kWidth<typename Ps::value_type>))),
     ...);
    return hash;
}

template <typename... Rs>
consteval uint32_t fingerprintLayout()
{
    uint32_t hash = 2166136261u;
    ((hash = fingerprintBlock(fnv1a(fnv1a(hash, Rs::kCount), Rs::block_type::kStride),
                              std::type_identity<typename Rs::block_type>{})),
     ...);
    return hash;
}

} // namespace detail

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

template <typename... Rs>
struct ParameterLayout {
    static constexpr size_t kBlockCount = sizeof...(Rs);
    /// Octets of the whole image.
    static constexpr size_t kSize = (size_t{0} + ... + Rs::kBytes);
    /// Parameters as declared, a repeated block counted once.
    static constexpr size_t kDeclaredParameters = (size_t{0} + ... + Rs::block_type::kParameterCount);

    static_assert(detail::allDistinct<typename Rs::block_type::id_type...>(),
                  "two blocks of one layout share an id enum; give each block its own");

    template <typename IdEnum>
    static constexpr size_t kEntry =
        detail::entryOf<sizeof...(Rs)>({std::same_as<typename Rs::block_type::id_type, IdEnum>...});

    template <typename IdEnum>
        requires(kEntry<IdEnum> < sizeof...(Rs))
    using block_type = typename std::tuple_element_t<kEntry<IdEnum>, std::tuple<Rs...>>::block_type;

    /// Offset of the first instance of the block from the start of the image.
    template <typename IdEnum>
        requires(kEntry<IdEnum> < sizeof...(Rs))
    static constexpr size_t kBase = detail::sumBefore<sizeof...(Rs)>({Rs::kBytes...}, kEntry<IdEnum>);

    /// How many instances the block has.
    template <typename IdEnum>
        requires(kEntry<IdEnum> < sizeof...(Rs))
    static constexpr size_t kCount = std::tuple_element_t<kEntry<IdEnum>, std::tuple<Rs...>>::kCount;

    /// Changes whenever an id, a value type, an order or a repeat count does.
    /// Stored with the image, so a block written for another layout is not
    /// taken into use.
    static constexpr uint32_t kFingerprint = detail::fingerprintLayout<Rs...>();

    std::tuple<Rs...> entries{};
};

/// The product's parameter memory: blocks in image order, plain or repeat<N>().
template <typename... Es>
constexpr auto parameterLayout(const Es&... entries)
{
    return ParameterLayout<decltype(detail::asRepeated(entries))...>{
        std::tuple<decltype(detail::asRepeated(entries))...>{detail::asRepeated(entries)...}};
}

// ---------------------------------------------------------------------------
// Default image
// ---------------------------------------------------------------------------

/// The image with every parameter at its declared default.
template <typename... Rs>
constexpr auto defaultImage(const ParameterLayout<Rs...>& layout)
    -> std::array<uint8_t, ParameterLayout<Rs...>::kSize>
{
    std::array<uint8_t, ParameterLayout<Rs...>::kSize> image{};
    size_t at = 0;
    const auto writeEntry = [&](const auto& repeated) {
        for (size_t instance = 0; instance < repeated.kCount; ++instance) {
            std::apply(
                [&](const auto&... parameter) {
                    ((detail::encode(parameter.spec.def, image.data() + at),
                      at += kWidth<typename std::remove_cvref_t<decltype(parameter)>::value_type>),
                     ...);
                },
                repeated.block.parameters);
        }
    };
    std::apply([&](const auto&... repeated) { (writeEntry(repeated), ...); }, layout.entries);
    return image;
}

/// The default image of a layout named as a template argument. Firmware uses
/// this form: it links the octets and nothing else of the layout.
template <const auto& Layout>
inline constexpr auto kDefaultImageOf = defaultImage(Layout);

// ---------------------------------------------------------------------------
// Bounds: what a loaded image is checked against
// ---------------------------------------------------------------------------

struct BoundsEntry {
    uint16_t offset{0};   ///< from the start of the block instance
    uint8_t width{0};     ///< 1 or 2 octets; 0 = not checked (floats)
    bool isSigned{false}; ///< compare as int16_t
    uint16_t min{0};
    uint16_t max{0};
};

struct BoundsBlock {
    uint16_t base{0};
    uint16_t stride{0};
    uint16_t count{0};
    uint16_t firstEntry{0};
    uint16_t entryCount{0};
};

template <size_t BlockCountV, size_t EntryCountV>
struct BoundsTable {
    std::array<BoundsBlock, BlockCountV> blocks{};
    std::array<BoundsEntry, EntryCountV> entries{};
};

namespace detail {

/// True for a 16-bit value read as int16_t: int16_t itself and enums over it.
template <typename V>
inline constexpr bool kSigned16 = std::same_as<V, int16_t>;

template <SmallEnum V>
inline constexpr bool kSigned16<V> = std::same_as<std::underlying_type_t<V>, int16_t>;

template <typename ParamT>
constexpr BoundsEntry boundsOf(const ParamT& parameter, size_t offset)
{
    using V = typename ParamT::value_type;
    BoundsEntry entry{};
    entry.offset = static_cast<uint16_t>(offset);
    if constexpr (std::same_as<V, float> || std::same_as<V, Dpt9Float>) {
        return entry;
    } else {
        entry.width = static_cast<uint8_t>(kWidth<V>);
        entry.isSigned = kSigned16<V>;
        int32_t low = 0;
        int32_t high = 0;
        const auto& spec = parameter.spec;
        if (spec.min != spec.max) {
            low = static_cast<int32_t>(spec.min);
            high = static_cast<int32_t>(spec.max);
        } else if (!spec.options.empty()) {
            low = high = spec.options.front().value;
            for (const Option& option : spec.options) {
                low = option.value < low ? option.value : low;
                high = option.value > high ? option.value : high;
            }
        } else if constexpr (std::same_as<V, bool>) {
            high = 1;
        } else if (entry.isSigned) {
            low = std::numeric_limits<int16_t>::min();
            high = std::numeric_limits<int16_t>::max();
        } else {
            high = kWidth<V> == 1u ? 0xFF : 0xFFFF;
        }
        entry.min = static_cast<uint16_t>(low);
        entry.max = static_cast<uint16_t>(high);
        return entry;
    }
}

} // namespace detail

template <typename... Rs>
constexpr auto boundsTable(const ParameterLayout<Rs...>& layout)
    -> BoundsTable<sizeof...(Rs), ParameterLayout<Rs...>::kDeclaredParameters>
{
    BoundsTable<sizeof...(Rs), ParameterLayout<Rs...>::kDeclaredParameters> table{};
    size_t blockAt = 0;
    size_t entryAt = 0;
    size_t base = 0;
    const auto addEntry = [&](const auto& repeated) {
        using BlockT = typename std::remove_cvref_t<decltype(repeated)>::block_type;
        table.blocks[blockAt++] = BoundsBlock{static_cast<uint16_t>(base), static_cast<uint16_t>(BlockT::kStride),
                                              static_cast<uint16_t>(repeated.kCount),
                                              static_cast<uint16_t>(entryAt),
                                              static_cast<uint16_t>(BlockT::kParameterCount)};
        size_t offset = 0;
        std::apply(
            [&](const auto&... parameter) {
                ((table.entries[entryAt++] = detail::boundsOf(parameter, offset),
                  offset += kWidth<typename std::remove_cvref_t<decltype(parameter)>::value_type>),
                 ...);
            },
            repeated.block.parameters);
        base += repeated.kBytes;
    };
    std::apply([&](const auto&... repeated) { (addEntry(repeated), ...); }, layout.entries);
    return table;
}

template <const auto& Layout>
inline constexpr auto kBoundsOf = boundsTable(Layout);

/// True when every checked parameter of the image is inside its bounds. One
/// loop for every product: the tables are the only per-product part.
inline bool imageWithinBounds(std::span<const BoundsBlock> blocks, std::span<const BoundsEntry> entries,
                              std::span<const uint8_t> image) noexcept
{
    for (const BoundsBlock& block : blocks) {
        if (static_cast<size_t>(block.base) + static_cast<size_t>(block.stride) * block.count > image.size()) {
            return false;
        }
        for (size_t instance = 0; instance < block.count; ++instance) {
            const uint8_t* at = image.data() + block.base + instance * block.stride;
            for (const BoundsEntry& entry : entries.subspan(block.firstEntry, block.entryCount)) {
                if (entry.width == 0u) {
                    continue;
                }
                const uint8_t* in = at + entry.offset;
                const auto raw = entry.width == 1u
                                     ? static_cast<uint16_t>(in[0])
                                     : static_cast<uint16_t>((static_cast<uint16_t>(in[0]) << 8u) | in[1]);
                const bool inside = entry.isSigned
                                        ? static_cast<int16_t>(raw) >= static_cast<int16_t>(entry.min)
                                              && static_cast<int16_t>(raw) <= static_cast<int16_t>(entry.max)
                                        : raw >= entry.min && raw <= entry.max;
                if (!inside) {
                    return false;
                }
            }
        }
    }
    return true;
}

template <size_t BlockCountV, size_t EntryCountV>
inline bool imageWithinBounds(const BoundsTable<BlockCountV, EntryCountV>& table,
                              std::span<const uint8_t> image) noexcept
{
    return imageWithinBounds(std::span<const BoundsBlock>(table.blocks),
                             std::span<const BoundsEntry>(table.entries), image);
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

/// One instance of a block inside an image. One pointer; copy it freely.
template <typename BlockT>
class BlockView {
public:
    using id_type = typename BlockT::id_type;

    explicit constexpr BlockView(const uint8_t* base) noexcept
        : _base(base)
    {
    }

    /// The parameter's value: a load at a constant offset.
    template <id_type Id>
    [[nodiscard]] auto get() const noexcept -> typename BlockT::template value_type_of<Id>
    {
        return detail::decode<typename BlockT::template value_type_of<Id>>(_base + BlockT::template kOffset<Id>);
    }

private:
    const uint8_t* _base;
};

/// Typed read access to a parameter image of the given layout.
template <typename LayoutT>
class ParameterView {
public:
    explicit constexpr ParameterView(std::span<const uint8_t, LayoutT::kSize> image) noexcept
        : _image(image.data())
    {
    }

    /// Instances of the block whose id enum is IdEnum.
    template <typename IdEnum>
    static constexpr size_t count() noexcept
    {
        return LayoutT::template kCount<IdEnum>;
    }

    /**
     * @brief The block whose id enum is IdEnum, instance 0-based.
     *
     * An instance past the last one reads the last one, so a wrong index can
     * never read outside the image.
     */
    template <typename IdEnum>
    [[nodiscard]] auto block(size_t instance = 0) const noexcept
        -> BlockView<typename LayoutT::template block_type<IdEnum>>
    {
        using BlockT = typename LayoutT::template block_type<IdEnum>;
        constexpr size_t count = LayoutT::template kCount<IdEnum>;
        const size_t index = instance < count ? instance : count - 1u;
        return BlockView<BlockT>(_image + LayoutT::template kBase<IdEnum> + index * BlockT::kStride);
    }

private:
    const uint8_t* _image;
};

/// The view type for a layout named as a template argument:
/// `params::ViewFor<kParameters> view(image);`
template <const auto& Layout>
using ViewFor = ParameterView<std::remove_cvref_t<decltype(Layout)>>;

// ---------------------------------------------------------------------------
// Export: walking a layout (host tools and tests; not for firmware)
// ---------------------------------------------------------------------------

/// One declared parameter as the exporter needs it.
struct ParameterInfo {
    std::string_view blockKey{};
    std::string_view blockName{};
    Condition blockVisible{};
    /// Tag of the block (compare with Condition::block).
    const void* block{nullptr};
    size_t blockBase{0};
    size_t blockStride{0};
    size_t blockCount{0};

    uint16_t id{0};
    ExportParameterValueKind kind{ExportParameterValueKind::None};
    size_t width{0};
    /// From the start of the block instance; the image offset of instance i
    /// is blockBase + i × blockStride + offset.
    size_t offset{0};
    double defaultValue{0.0};
    double min{0.0};
    double max{0.0};
    std::string_view key{};
    std::string_view name{};
    std::string_view unit{};
    std::span<const Option> options{};
    Condition visible{};
};

/// Calls visit(const ParameterInfo&) for every declared parameter, in image order.
template <typename... Rs, typename Visitor>
constexpr void forEachParameter(const ParameterLayout<Rs...>& layout, Visitor&& visit)
{
    size_t base = 0;
    const auto walkEntry = [&](const auto& repeated) {
        using BlockT = typename std::remove_cvref_t<decltype(repeated)>::block_type;
        size_t offset = 0;
        std::apply(
            [&](const auto&... parameter) {
                const auto one = [&](const auto& p) {
                    using ParamT = std::remove_cvref_t<decltype(p)>;
                    using V = typename ParamT::value_type;
                    ParameterInfo info{};
                    info.blockKey = repeated.block.spec.key;
                    info.blockName = repeated.block.spec.name;
                    info.blockVisible = repeated.block.spec.visible;
                    info.block = &detail::kBlockTag<typename BlockT::id_type>;
                    info.blockBase = base;
                    info.blockStride = BlockT::kStride;
                    info.blockCount = repeated.kCount;
                    info.id = static_cast<uint16_t>(detail::toInteger(ParamT::id));
                    info.kind = detail::kindOf<V>();
                    info.width = kWidth<V>;
                    info.offset = offset;
                    if constexpr (std::same_as<V, Dpt9Float>) {
                        info.defaultValue = static_cast<double>(p.spec.def.value);
                    } else if constexpr (std::same_as<V, float>) {
                        info.defaultValue = static_cast<double>(p.spec.def);
                    } else {
                        info.defaultValue = static_cast<double>(detail::toInteger(p.spec.def));
                    }
                    info.min = p.spec.min;
                    info.max = p.spec.max;
                    info.key = p.spec.key;
                    info.name = p.spec.name;
                    info.unit = p.spec.unit;
                    info.options = p.spec.options;
                    info.visible = p.spec.visible;
                    visit(info);
                    offset += kWidth<V>;
                };
                (one(parameter), ...);
            },
            repeated.block.parameters);
        base += repeated.kBytes;
    };
    std::apply([&](const auto&... repeated) { (walkEntry(repeated), ...); }, layout.entries);
}

} // namespace knx::product::params
