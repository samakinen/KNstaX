// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file commissioned_product.hpp
 * @brief High-level developer surface for KNX endpoint products: bindings builder, parameter
 *        management, and transport-independent runtime start.
 */

#pragma once

#include "knx/config.hpp"
#include "knx/netip/ip_secure/mc_timer_store.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/physical/ip_routing_physical.hpp"
#if KNX_IP_TUNNELING
#include "knx/physical/ip_tunneling_physical.hpp"
#endif
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/platform/platform.hpp"
#include "knx/product/endpoint.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/network/two_port_coupler.hpp"
#include "knx/util/result.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace knx::product {

// ============================================================================
// HOW TO USE THIS HEADER — firmware developer quick start
// ============================================================================
//
// Most firmware targets need exactly one call:
//
//   auto runtime = knx::product::startCommissionedProduct(
//       myProductDefinition,
//       std::move(options));
//
// That call returns a CommissionedProductHandle<> — a unique_ptr owning the
// CommissionedProductRuntime<> that drives the full KNX stack lifecycle.  The
// runtime is built directly on the heap because it holds the endpoint and
// parameter state inline and is far larger than an MCU task stack frame can
// carry.  See docs/reference/product_authoring_guide.md for the canonical
// step-by-step guide.
//
// Transport paths and their support tier:
//   • Tp1MediumBackend (CommissionedSupportTier::ProductGrade) — recommended
//     for all new TP1 designs.  Fully ETS-programmable and KNX-catalogued.
//   • IpTunnelingManaged / IpRoutingManaged (Functional) — stack-managed IP
//     paths; functional for deployment.
//   • Tp1MacPhysical / IpTunneling / IpRouting (AdvancedManual) — raw
//     physical pointer; for expert or legacy integration only.
//
// For test/demo use, see the `expert` namespace below.  It is NOT part of the
// normal firmware integration path.
// ============================================================================

using ProductIdentity = endpoint::ProductIdentity;
using PersistencePolicy = endpoint::PersistencePolicy;

/**
 * Explicit lifecycle states for a commissioned KNX product.
 *
 * Progression under normal operation:
 *   Uncommissioned → (ETS assigns IA) → Operational
 *   Any state       → (prog button)   → Commissioning
 *   Commissioning   → (IA assigned)   → Operational
 *
 * The firmware dev observes these states via onLifecycleChanged() and
 * CommissionedProductRuntime::lifecycleState(). Zero KNX protocol knowledge
 * is required.
 */
enum class DeviceLifecycleState : uint8_t {
    Uncommissioned, ///< No individual address assigned yet (device fresh from factory or reset).
    Commissioning,  ///< Programming mode active; ETS is writing parameters/addresses.
    Operational,    ///< Individual address assigned and KNX bus operational.
};

using endpoint::GroupAddressBinding;
using endpoint::groupAddressBinding;
using endpoint::makeEndpointDefinition;
namespace semantics = endpoint::semantics;

template <typename ProductDefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
class CommissionedBindingsBuilder;

template <typename ProductDefinitionT>
class CommissionedParametersView;

template <typename ProductDefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
class CommissionedProductRuntime;

/// Owning handle to a started commissioned product.
///
/// `startCommissionedProduct()` hands back the runtime through this handle
/// rather than by value. The runtime carries the full endpoint and parameter
/// state inline and is tens of kilobytes for a product of any size — far too
/// large to sit in an MCU task's stack frame, let alone in the two frames a
/// by-value return would need it in at once. Constructing it directly in its
/// final storage also removes the move that a by-value return would perform,
/// and with it the callback re-wiring that move has to do.
///
/// Nothing about the product's compile-time model changes: the runtime type is
/// still fully static in the product definition and binding capacity, and all
/// port, datapoint and parameter checking still happens at build time.
template <typename ProductDefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
using CommissionedProductHandle =
    std::unique_ptr<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>;

using LifecycleChangedCallback = util::InplaceFunction<void(DeviceLifecycleState), 32>;

namespace expert {

// ============================================================================
// EXPERT / TEST NAMESPACE — not for normal firmware integration
// ============================================================================
//
// Helpers in this namespace are intended exclusively for:
//   • Smoke tests and integration tests that need to exercise runtime
//     behavior without a live ETS session.
//   • Demo setups and developer boards that apply commissioning state
//     programmatically (e.g. hard-coded group addresses in a main.cpp).
//
// Do NOT call these from production firmware.  They bypass the KNX management
// model and do not persist state through the KNX stack.
// ============================================================================

template <typename ProductDefinitionT, size_t BindingCapacity, typename... Bindings>
auto bindDemoGroupAddresses(CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>& runtime,
                            Bindings&&... bindings) -> util::Result<void>;

template <auto ParameterId, typename ProductDefinitionT, size_t BindingCapacity>
auto applyDemoParameterValue(
    CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>& runtime,
    typename ProductDefinitionT::template ParameterValueType<ParameterId> value) -> util::Result<void>;

template <typename ProductDefinitionT, size_t BindingCapacity>
void applyParameterDataBytes(CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>& runtime,
                             std::span<const uint8_t> data);

} // namespace expert

/// Sentinel for ParameterDescriptor::visibleWhenParameterId meaning
/// "always visible".  0xFFFF is not a usable parameter id.
inline constexpr uint16_t kNoVisibilityCondition = 0xFFFFu;

/// One selectable value of an enumerated parameter.
struct ParameterOption {
    int64_t value{0};
    /// Label ETS puts in the drop-down.
    std::string_view label{};
};

inline constexpr size_t kMaxParameterOptions = 24;

/// Fixed-capacity option list carried in a constexpr product definition.
struct ParameterOptions {
    std::array<ParameterOption, kMaxParameterOptions> entries{};
    size_t count{0};

    constexpr bool empty() const { return count == 0; }
    constexpr const ParameterOption* begin() const { return entries.data(); }
    constexpr const ParameterOption* end() const { return entries.data() + count; }
};

/**
 * @brief Declare the selectable values of an enumerated parameter.
 *
 * Without options an enum parameter exports as a bare number box and the
 * integrator has to know that 2 means Standby.  With them ETS renders a
 * drop-down, which is the difference between a product an integrator can
 * configure and one they have to be handed a table for.
 *
 *   .options = parameterOptions({0, "Auto"}, {1, "Comfort"}, {2, "Standby"})
 */
template <typename... OptionTs>
constexpr ParameterOptions parameterOptions(OptionTs... options)
{
    static_assert(sizeof...(OptionTs) <= kMaxParameterOptions,
                  "Too many parameter options; raise kMaxParameterOptions");
    return ParameterOptions{{ParameterOption{options}...}, sizeof...(OptionTs)};
}

template <auto ParameterId, typename ValueT>
struct ParameterDescriptor {
    using id_type = std::remove_cv_t<decltype(ParameterId)>;
    using value_type = ValueT;

    static constexpr auto id = ParameterId;

    std::string_view key{};
    /// Human-readable label shown by ETS/Kaenx (knxprod Parameter Text).
    /// Empty falls back to the technical key.
    std::string_view displayName{};
    ValueT defaultValue{};
    /// Selectable values.  Only meaningful for enumerated/boolean parameters;
    /// leaving it empty exports a plain numeric field.
    ParameterOptions options{};
    /// Inclusive numeric bounds for free-form numeric parameters.  When both
    /// are zero the exporter falls back to the full range of the value type.
    double minValue{0.0};
    double maxValue{0.0};
    /// Optional unit suffix ETS appends to the input field (e.g. "°C", "s").
    std::string_view unit{};
    /// Section heading this parameter appears under in ETS.  Parameters sharing
    /// a group are rendered together; leaving it empty puts the parameter in
    /// the product's default block.  A product with thirty parameters in one
    /// flat list is materially harder to configure than the same thirty split
    /// into "Temperature", "Ventilation" and "Heating".
    std::string_view group{};
    /// Show this parameter only when parameter `visibleWhenParameterId` equals
    /// `visibleWhenValue`.  Lets a product hide the cooling PID settings when
    /// cooling is switched off, instead of showing settings that do nothing.
    /// Ignored when `visibleWhenParameterId` is left at its sentinel.
    uint16_t visibleWhenParameterId{kNoVisibilityCondition};
    int64_t visibleWhenValue{0};
    /// Show the whole `group` section only when parameter
    /// `groupVisibleWhenParameterId` equals `groupVisibleWhenValue`.  Every
    /// parameter of the group must declare the same pair; the exporter refuses
    /// a partially gated group.  Use this instead of repeating the same
    /// per-parameter condition on every member: that variant leaves ETS showing
    /// a section heading with nothing under it, which reads as a broken
    /// product.  The controlling parameter must live in a different group, or
    /// switching the section off would hide the switch itself.  The two
    /// conditions compose — a section gated on "Heating sequence = Used" can
    /// still hide its PI terms while the algorithm is two-point.
    uint16_t groupVisibleWhenParameterId{kNoVisibilityCondition};
    int64_t groupVisibleWhenValue{0};
};

template <typename... ParameterTs>
struct StaticParameterSchema {
    using descriptors_tuple = std::tuple<ParameterTs...>;

    static constexpr size_t kParameterCount = sizeof...(ParameterTs);

    descriptors_tuple descriptors{};
};

struct KnxPersistenceIdentity {
    // Advanced override for KNX-managed commissioned-state persistence.
    // Normal firmware should usually leave this empty and rely on the product
    // definition's persistence namespace.
    std::string_view instanceKey{};
    std::string_view storageNamespace{};
};

struct ResolvedKnxPersistenceIdentity {
    std::string instanceKey{};
    std::string storageNamespace{};
    // Derived from PersistencePolicy::schemaVersion and treated as the
    // compatibility boundary for stack-owned KNX commissioned state.
    uint16_t schemaVersion{1};
};

namespace detail {

#if KNX_FEATURE_NETIP && KNX_SECURE_ENABLED
/**
 * @brief The Secure Routing multicast timer, kept beside the commissioned state.
 *
 * One record under its own key in the storage namespace the stack's interface
 * objects use, opened through the same platform persistence. A second handle
 * on that namespace rather than a hook into the object manager: the record is
 * not a property, is written on its own schedule (at most once per hour of
 * timer time, 03_08_09 §2.2.4.2), and must not be rewritten with every
 * property download.
 *
 * Opened on first use, which is when the first backbone key is applied.
 */
class PersistentMcTimerStore final : public netip::ip_secure::McTimerStore {
public:
    explicit PersistentMcTimerStore(std::string storageNamespace)
        : _namespace(std::move(storageNamespace))
    {
    }

    std::optional<Record> load() override
    {
        if (!open()) {
            return std::nullopt;
        }
        std::vector<uint8_t> data;
        if (_persistence->load(kRecordKey, data) != objects::PersistenceResult::Success) {
            return std::nullopt;
        }
        return decode(data);
    }

    util::Result<void> save(const Record& record) override
    {
        if (!open()) {
            return util::ErrorCode::NotInitialized;
        }
        const auto encoded = encode(record);
        if (_persistence->save(kRecordKey, encoded) != objects::PersistenceResult::Success) {
            return util::ErrorCode::OperationFailed;
        }
        return util::Result<void>::ok();
    }

private:
    /// Short enough for an NVS key (15 characters), and not of the dXXXX
    /// form the interface-object records use.
    static constexpr const char* kRecordKey = "mc_timer";

    bool open()
    {
        if (_persistence) {
            return true;
        }
        auto persistence = objects::createPersistence();
        if (!persistence || persistence->init(_namespace).isError()) {
            return false;
        }
        _persistence = std::move(persistence);
        return true;
    }

    std::string _namespace;
    std::unique_ptr<objects::ObjectPersistence> _persistence;
};
#endif

template <auto... Ids>
consteval bool parameterIdsUnique()
{
    if constexpr (sizeof...(Ids) <= 1u) {
        return true;
    } else {
        constexpr auto ids = std::array{Ids...};
        for (size_t outer = 0; outer < ids.size(); ++outer) {
            for (size_t inner = outer + 1u; inner < ids.size(); ++inner) {
                if (ids[outer] == ids[inner]) {
                    return false;
                }
            }
        }

        return true;
    }
}

template <auto ParameterId, typename... DescriptorTs>
consteval size_t findParameterIndex()
{
    constexpr auto ids = std::array{DescriptorTs::id...};
    for (size_t index = 0; index < ids.size(); ++index) {
        if (ids[index] == ParameterId) {
            return index;
        }
    }

    return ids.size();
}

template <typename SchemaT>
struct ParameterDescriptorFor;

template <typename... DescriptorTs>
struct ParameterDescriptorFor<StaticParameterSchema<DescriptorTs...>> {
    static_assert(parameterIdsUnique<DescriptorTs::id...>(),
                  "StaticParameterSchema parameter IDs must be unique");

    template <auto ParameterId>
    struct At {
        static constexpr size_t index = findParameterIndex<ParameterId, DescriptorTs...>();
        static_assert(index < sizeof...(DescriptorTs),
                      "Parameter ID is not part of this product parameter schema");

        using type = std::tuple_element_t<index, std::tuple<DescriptorTs...>>;
    };
};

template <typename SchemaT>
struct ParameterCallbacks;

template <typename... DescriptorTs>
struct ParameterCallbacks<StaticParameterSchema<DescriptorTs...>> {
    template <typename ValueT>
    using callback_type_for = util::InplaceFunction<void(ValueT), 64>;

    using callbacks_tuple = std::tuple<callback_type_for<typename DescriptorTs::value_type>...>;

    callbacks_tuple callbacks{};

    template <auto ParameterId, typename Callback>
    void set(Callback&& callback)
    {
        constexpr size_t index = ParameterDescriptorFor<StaticParameterSchema<DescriptorTs...>>
                                     ::template At<ParameterId>::index;
        using callback_type = callback_type_for<
            typename ParameterDescriptorFor<StaticParameterSchema<DescriptorTs...>>
                ::template At<ParameterId>::type::value_type>;

        std::get<index>(callbacks) = callback_type(std::forward<Callback>(callback));
    }
};

template <typename SchemaT>
class ParameterState;

// ---------------------------------------------------------------------------
// Byte-level encoding/decoding helpers for ETS ProgramData round-trip
// ---------------------------------------------------------------------------
namespace byte_io {

// Runtime helpers (not constexpr): the Dpt9Float branches delegate to the
// shared application::Dpt9 codec, which is a runtime function.
template <typename ValueT>
inline ValueT fromBigEndianBytes(std::span<const uint8_t> bytes) noexcept
{
    if constexpr (std::is_same_v<ValueT, bool>) {
        return bytes[0] != 0u;
    } else if constexpr (std::is_same_v<ValueT, uint8_t>) {
        return bytes[0];
    } else if constexpr (std::is_same_v<ValueT, uint16_t>) {
        return static_cast<uint16_t>(
            (static_cast<uint16_t>(bytes[0]) << 8u) | bytes[1]);
    } else if constexpr (std::is_same_v<ValueT, int16_t>) {
        return static_cast<int16_t>(
            (static_cast<uint16_t>(bytes[0]) << 8u) | bytes[1]);
    } else if constexpr (std::is_same_v<ValueT, float>) {
        uint32_t raw =
            (static_cast<uint32_t>(bytes[0]) << 24u) |
            (static_cast<uint32_t>(bytes[1]) << 16u) |
            (static_cast<uint32_t>(bytes[2]) << 8u) |
             static_cast<uint32_t>(bytes[3]);
        float value;
        std::memcpy(&value, &raw, sizeof(float));
        return value;
    } else if constexpr (std::is_same_v<ValueT, Dpt9Float>) {
        // KNX DPT9 2-byte half-float — the format ETS writes into parameter
        // memory for TypeFloat Encoding="DPT 9". Decoded by the shared codec.
        float value = 0.0f;
        if (application::Dpt9::decode(bytes.first(2), value).isError()) {
            return Dpt9Float{0.0f};
        }
        return Dpt9Float{value};
    } else if constexpr (std::is_enum_v<ValueT>) {
        using U = std::underlying_type_t<ValueT>;
        if constexpr (sizeof(U) == 1u) {
            return static_cast<ValueT>(bytes[0]);
        } else {
            return static_cast<ValueT>(static_cast<U>(
                (static_cast<uint16_t>(bytes[0]) << 8u) | bytes[1]));
        }
    } else {
        return ValueT{};
    }
}

template <typename ValueT>
inline void toBigEndianBytes(ValueT value, std::span<uint8_t> out) noexcept
{
    if constexpr (std::is_same_v<ValueT, bool>) {
        out[0] = value ? 0x01u : 0x00u;
    } else if constexpr (std::is_same_v<ValueT, uint8_t>) {
        out[0] = value;
    } else if constexpr (std::is_same_v<ValueT, uint16_t>) {
        out[0] = static_cast<uint8_t>(value >> 8u);
        out[1] = static_cast<uint8_t>(value & 0xFFu);
    } else if constexpr (std::is_same_v<ValueT, int16_t>) {
        const auto uv = static_cast<uint16_t>(value);
        out[0] = static_cast<uint8_t>(uv >> 8u);
        out[1] = static_cast<uint8_t>(uv & 0xFFu);
    } else if constexpr (std::is_same_v<ValueT, float>) {
        uint32_t raw;
        std::memcpy(&raw, &value, sizeof(float));
        out[0] = static_cast<uint8_t>((raw >> 24u) & 0xFFu);
        out[1] = static_cast<uint8_t>((raw >> 16u) & 0xFFu);
        out[2] = static_cast<uint8_t>((raw >>  8u) & 0xFFu);
        out[3] = static_cast<uint8_t>( raw          & 0xFFu);
    } else if constexpr (std::is_same_v<ValueT, Dpt9Float>) {
        out[0] = 0u;
        out[1] = 0u;
        (void)application::Dpt9::encode(value.value, out.first(2));
    } else if constexpr (std::is_enum_v<ValueT>) {
        using U = std::underlying_type_t<ValueT>;
        toBigEndianBytes(static_cast<U>(value), out);
    }
    // Text / None: no-op
}

} // namespace byte_io


template <typename... DescriptorTs>
class ParameterState<StaticParameterSchema<DescriptorTs...>> {
public:
    using schema_type = StaticParameterSchema<DescriptorTs...>;

    // Callbacks by rvalue reference: the tuple holds one InplaceFunction per
    // parameter, so a by-value sink would cost a full copy of it on the
    // constructing frame's stack.
    explicit ParameterState(const schema_type& schema,
                            ParameterCallbacks<schema_type>&& callbacks = {})
        : _schema(&schema)
        , _callbacks(std::move(callbacks))
    {
    }

    template <auto ParameterId>
    auto get() const
        -> typename ParameterDescriptorFor<schema_type>::template At<ParameterId>::type::value_type
    {
        constexpr size_t index = ParameterDescriptorFor<schema_type>::template At<ParameterId>::index;
        using value_type = typename ParameterDescriptorFor<schema_type>::template At<ParameterId>::type::value_type;

        const auto& overrideValue = std::get<index>(_values);
        if (overrideValue.has_value()) {
            return overrideValue.value();
        }

        return static_cast<value_type>(std::get<index>(_schema->descriptors).defaultValue);
    }

    template <auto ParameterId>
    bool hasCustomValue() const
    {
        constexpr size_t index = ParameterDescriptorFor<schema_type>::template At<ParameterId>::index;
        return std::get<index>(_values).has_value();
    }

    template <auto ParameterId>
    auto apply(typename ParameterDescriptorFor<schema_type>::template At<ParameterId>::type::value_type value)
        -> util::Result<void>
    {
        constexpr size_t index = ParameterDescriptorFor<schema_type>::template At<ParameterId>::index;
        std::get<index>(_values) = value;

        auto& callback = std::get<index>(_callbacks.callbacks);
        if (callback) {
            callback(value);
        }

        return util::Result<void>::ok();
    }

    /// Deserialise parameter values from a raw KNX ProgramData byte block and
    /// apply each recognised value to this ParameterState, firing the
    /// corresponding `onParameterChanged` callback.  Parameters with
    /// non-serialisable types (Text, None) are skipped.  Bytes are read
    /// sequentially in schema declaration order, big-endian.
    void applyFromBytes(std::span<const uint8_t> data)
    {
        size_t offset = 0;
        applyFromBytesAt<0>(data, offset);
    }

    /// Serialise all current parameter values (or their defaults) into a raw
    /// KNX ProgramData byte block.  The layout is consistent with applyFromBytes.
    std::vector<uint8_t> toBytes() const
    {
        std::vector<uint8_t> result;
        appendBytesAt<0>(result);
        return result;
    }

private:
    template <size_t I>
    void applyFromBytesAt(std::span<const uint8_t> data, size_t& offset)
    {
        if constexpr (I < sizeof...(DescriptorTs)) {
            using Descriptor = std::tuple_element_t<I, std::tuple<DescriptorTs...>>;
            using ValueT = typename Descriptor::value_type;
            constexpr auto kind = detail::exportParameterValueKind<ValueT>();
            constexpr size_t width = detail::exportParameterValueByteWidth(kind);
            if constexpr (width > 0u) {
                if (offset + width <= data.size()) {
                    const auto value = byte_io::fromBigEndianBytes<ValueT>(data.subspan(offset, width));
                    std::get<I>(_values) = value;
                    auto& cb = std::get<I>(_callbacks.callbacks);
                    if (cb) { cb(value); }
                    offset += width;
                }
            }
            applyFromBytesAt<I + 1>(data, offset);
        }
    }

    template <size_t I>
    void appendBytesAt(std::vector<uint8_t>& out) const
    {
        if constexpr (I < sizeof...(DescriptorTs)) {
            using Descriptor = std::tuple_element_t<I, std::tuple<DescriptorTs...>>;
            using ValueT = typename Descriptor::value_type;
            constexpr auto kind = detail::exportParameterValueKind<ValueT>();
            constexpr size_t width = detail::exportParameterValueByteWidth(kind);
            if constexpr (width > 0u) {
                const auto& optVal = std::get<I>(_values);
                const ValueT v = optVal.has_value() ? *optVal
                                                    : static_cast<ValueT>(std::get<I>(_schema->descriptors).defaultValue);
                const size_t base = out.size();
                out.resize(base + width, 0u);
                byte_io::toBigEndianBytes(v, std::span<uint8_t>(out.data() + base, width));
            }
            appendBytesAt<I + 1>(out);
        }
    }

    const schema_type* _schema;
    std::tuple<std::optional<typename DescriptorTs::value_type>...> _values{};
    ParameterCallbacks<schema_type> _callbacks{};
};

template <typename ProductDefinitionT>
ResolvedKnxPersistenceIdentity resolveKnxPersistenceIdentity(const ProductDefinitionT& definition,
                                                            const KnxPersistenceIdentity& requested)
{
    ResolvedKnxPersistenceIdentity resolved{};
    resolved.schemaVersion = definition.endpointDefinition.persistence.schemaVersion;
    const bool hasExplicitInstanceKey = !requested.instanceKey.empty();

    if (hasExplicitInstanceKey) {
        resolved.instanceKey = requested.instanceKey;
    } else if (!definition.endpointDefinition.identity.productKey.empty()) {
        resolved.instanceKey = std::string(definition.endpointDefinition.identity.productKey);
    } else {
        resolved.instanceKey = "commissioned_product";
    }

    if (!requested.storageNamespace.empty()) {
        resolved.storageNamespace = requested.storageNamespace;
        return resolved;
    }

    const auto prefix = definition.endpointDefinition.persistence.namespacePrefix;
    if (hasExplicitInstanceKey && !prefix.empty() && std::string_view(prefix) != resolved.instanceKey) {
        resolved.storageNamespace = std::string(prefix) + "_" + resolved.instanceKey;
        return resolved;
    }
    if (!prefix.empty()) {
        resolved.storageNamespace = std::string(prefix);
        return resolved;
    }

    resolved.storageNamespace = resolved.instanceKey;
    return resolved;
}

} // namespace detail

template <typename SchemaT, auto ParameterId>
using parameter_descriptor_t =
    typename detail::ParameterDescriptorFor<std::remove_cvref_t<SchemaT>>::template At<ParameterId>::type;

template <typename SchemaT, auto ParameterId>
inline constexpr size_t parameter_index_v =
    detail::ParameterDescriptorFor<std::remove_cvref_t<SchemaT>>::template At<ParameterId>::index;

template <auto ParameterId, typename ValueT>
constexpr auto parameter(std::string_view key,
                         ValueT defaultValue) -> ParameterDescriptor<ParameterId, std::remove_cvref_t<ValueT>>
{
    return ParameterDescriptor<ParameterId, std::remove_cvref_t<ValueT>>{
        .key = key,
        .displayName = key,
        .defaultValue = std::forward<ValueT>(defaultValue),
    };
}

/// Overload with a human-readable ETS label distinct from the technical key.
template <auto ParameterId, typename ValueT>
constexpr auto parameter(std::string_view key,
                         std::string_view displayName,
                         ValueT defaultValue) -> ParameterDescriptor<ParameterId, std::remove_cvref_t<ValueT>>
{
    return ParameterDescriptor<ParameterId, std::remove_cvref_t<ValueT>>{
        .key = key,
        .displayName = displayName,
        .defaultValue = std::forward<ValueT>(defaultValue),
    };
}

template <typename... ParameterTs>
constexpr auto makeParameterSchema(ParameterTs... parameters)
    -> StaticParameterSchema<std::remove_cvref_t<ParameterTs>...>
{
    return StaticParameterSchema<std::remove_cvref_t<ParameterTs>...>{
        std::tuple<std::remove_cvref_t<ParameterTs>...>{std::forward<ParameterTs>(parameters)...},
    };
}

template <typename EndpointDefinitionT, typename ParameterSchemaT = StaticParameterSchema<>>
struct CommissionedProductDefinition {
    using endpoint_definition_type = std::remove_cvref_t<EndpointDefinitionT>;
    using parameter_schema_type = std::remove_cvref_t<ParameterSchemaT>;
    using port_id_type = typename endpoint_definition_type::port_id_type;

    template <auto ParameterId>
    using ParameterValueType = typename parameter_descriptor_t<parameter_schema_type, ParameterId>::value_type;

    endpoint_definition_type endpointDefinition{};
    parameter_schema_type parameterSchema{};
};

template <typename EndpointDefinitionT>
constexpr auto makeCommissionedProduct(EndpointDefinitionT endpointDefinition)
    -> CommissionedProductDefinition<std::remove_cvref_t<EndpointDefinitionT>, StaticParameterSchema<>>
{
    return CommissionedProductDefinition<std::remove_cvref_t<EndpointDefinitionT>, StaticParameterSchema<>>{
        .endpointDefinition = std::move(endpointDefinition),
        .parameterSchema = makeParameterSchema(),
    };
}

template <typename EndpointDefinitionT, typename ParameterSchemaT>
constexpr auto makeCommissionedProduct(EndpointDefinitionT endpointDefinition,
                                       ParameterSchemaT parameterSchema)
    -> CommissionedProductDefinition<std::remove_cvref_t<EndpointDefinitionT>,
                                     std::remove_cvref_t<ParameterSchemaT>>
{
    return CommissionedProductDefinition<std::remove_cvref_t<EndpointDefinitionT>,
                                         std::remove_cvref_t<ParameterSchemaT>>{
        .endpointDefinition = std::move(endpointDefinition),
        .parameterSchema = std::move(parameterSchema),
    };
}

/// Options for enabling KNX Data Secure on this device.
///
/// All fields default to off/empty — existing non-secure products require no
/// change.  Security is only activated when `enabled` is true.
struct SecureCommissioningOptions {
    /// Enable KNX Data Secure mode.  When false (default) the Security
    /// Interface Object is left in its factory-default disabled state and no
    /// key material is written.  Non-secure products must leave this false.
    bool enabled{false};

    /// ETS tool key (16-byte AES-128).  Written to the SecurityInterfaceObject
    /// on start() when `enabled` is true.  Allows ETS to open a secure
    /// management session without the device needing a pre-provisioned key.
    std::optional<std::array<uint8_t, 16>> toolKey{};
};

struct CommissionedProductOptions {
    KnxPersistenceIdentity persistence{};
    // Standalone smoke tests may set this to exercise normal process traffic
    // without ETS commissioning. Real products should leave it unset so KNstaX
    // keeps the KNX initial device address until a unique IA is assigned.
    std::optional<IndividualAddress> standaloneDemoIndividualAddress{};
    std::unique_ptr<physical::Tp1MediumBackend> mediumBackend;
    bool restoreKnxStateOnBoot{true};
    bool startInProgrammingMode{false};
    /// Secure commissioning options.  Defaults are behavior-safe: security
    /// disabled, no keys applied.  Non-secure products require no change here.
    SecureCommissioningOptions secure{};
};

enum class CommissionedTransportPath : uint8_t {
    NotStarted = 0,
    Tp1MediumBackend,    ///< TP1 medium backend (product-grade, managed by stack)
    Tp1MacPhysical,      ///< TP1 MAC physical (advanced manual)
    IpTunneling,         ///< IP tunneling — raw physical pointer (advanced manual)
    IpRouting,           ///< IP routing — raw physical pointer (advanced manual)
    IpTunnelingManaged,  ///< IP tunneling via IpTunnelingOptions (functional, stack-managed)
    IpRoutingManaged,    ///< IP routing via IpRoutingOptions (functional, stack-managed)
    Tp1Coupler,          ///< Two TP1 ports with spec routing between them (product-grade)
};

/// Support tier classification for the active transport path.
enum class CommissionedSupportTier : uint8_t {
    ProductGrade = 0, ///< Fully validated ETS-programmable production path.
    Functional,       ///< Stack-managed setup; functional for deployment, not yet ETS-catalogued.
    AdvancedManual,   ///< Raw physical pointer; requires manual wiring and expert knowledge.
};

constexpr CommissionedSupportTier supportTierForTransportPath(CommissionedTransportPath path)
{
    switch (path) {
        case CommissionedTransportPath::Tp1MediumBackend:
        case CommissionedTransportPath::Tp1Coupler:
            return CommissionedSupportTier::ProductGrade;
        case CommissionedTransportPath::IpTunnelingManaged:
        case CommissionedTransportPath::IpRoutingManaged:
            return CommissionedSupportTier::Functional;
        case CommissionedTransportPath::Tp1MacPhysical:
        case CommissionedTransportPath::IpTunneling:
        case CommissionedTransportPath::IpRouting:
        case CommissionedTransportPath::NotStarted:
            return CommissionedSupportTier::AdvancedManual;
    }

    return CommissionedSupportTier::AdvancedManual;
}

#if KNX_FEATURE_NETIP
#if KNX_IP_TUNNELING
/// Options for starting a commissioned product over KNXnet/IP tunneling.
///
/// The network interface is taken from the `platform::Platform` argument
/// passed to `startCommissionedProduct` — firmware does not need to extract
/// it manually.
struct IpTunnelingOptions {
    /// Required: KNXnet/IP gateway host address.
    IpAddress host{};
    /// KNXnet/IP gateway port (default 3671).
    NetIpPort port{NetIpPort(3671)};
    /// Device-level KNX Data Secure options (same as for TP1).  Default is
    /// behavior-safe (security disabled, no keys).
    SecureCommissioningOptions secure{};
};
#endif  // KNX_IP_TUNNELING

/// Optional KNX/IP Secure Routing configuration (network-level AES encryption).
/// Distinct from device-level Data Secure (`SecureCommissioningOptions`).
struct IpRoutingSecureOptions {
#if KNX_SECURE_ENABLED
    /// Enable KNX/IP Secure Routing.  When false (default) plain multicast is used.
    bool enabled{false};
    /// 16-byte AES-128 group key for multicast encryption. Used only while
    /// ETS has not downloaded a backbone key of its own, which takes precedence.
    ///
    /// Nothing else is configured: the serial is the device's KNX serial number,
    /// the tag is random per frame and the sequence information is the
    /// multicast timer, which the product persists (03_08_09 §2.2.2).
    std::array<uint8_t, 16> groupKey{};
#endif
};

/// Options for starting a commissioned product over KNXnet/IP Routing (multicast).
struct IpRoutingOptions {
    /// Required: KNX multicast group address.  Standard KNX default: 224.0.23.12.
    IpAddress multicastGroup{};
    /// UDP port (default 3671).
    NetIpPort port{NetIpPort(3671)};
    /// Egress/join interface address.  Use IpAddress(0) for the system default.
    IpAddress interfaceAddress{};
    /// Multicast hop limit.  1 confines KNX traffic to the local subnet; the
    /// default of 16 matches what KNXnet/IP routers ship with.
    uint8_t timeToLive{16};

    /**
     * Deliver this device's own multicast back to its own socket.
     *
     * Off, because a device has nothing to learn from its own echo and a good
     * deal to lose. The looped-back copy re-enters the stack as an ordinary
     * inbound frame, so a Data Secure device sees its own responses arriving
     * from its own individual address with sequence numbers it has already
     * spent, records each as a security failure against itself, and reports
     * that count to ETS.
     *
     * Worth having as a switch rather than a constant: two endpoints sharing
     * one host only reach each other when the sender loops back, which is how
     * the host tests put two devices on one machine.
     */
    Toggle multicastLoopback{Toggle::Disable};

    /**
     * Answer KNXnet/IP discovery and self-description on the routing socket.
     *
     * On by default, because Core is mandatory for a KNXnet/IP device
     * (03/08/01 Table 2) and a device that stays silent is one ETS cannot list
     * even while it is sitting on the multicast group. Everything the response
     * carries — individual address, serial number, programming mode, medium —
     * is read from the running device when a request arrives, so the answer is
     * never a snapshot from boot.
     */
    bool announceOnNetwork{true};

    /**
     * The name ETS shows for this device in a discovery list.
     *
     * Up to 30 octets of ISO 8859-1. Empty falls back to the product's order
     * number, which identifies the model but not the unit — set this to
     * whatever the installation calls this particular device.
     */
    std::string friendlyName{};

    /// Optional KNX/IP Secure Routing settings (network-level, not device-level).
    IpRoutingSecureOptions secureRouting{};
    /// Device-level KNX Data Secure options.  Default is behavior-safe.
    SecureCommissioningOptions secure{};
};
#endif  // KNX_FEATURE_NETIP

/**
 * @brief Options for starting a commissioned product as a KNX coupler.
 *
 * The device is a normal commissioned product — same endpoints, parameters,
 * ETS identity and persistence — that additionally routes between two TP1
 * ports according to 03/03/03 §2.4.2.4.
 *
 * The coupler's role (line coupler or backbone coupler) is derived from the
 * individual address ETS assigns: x.y.0 makes it a line coupler for line x.y,
 * x.0.0 a backbone coupler for area x. Until it has one it behaves as a
 * repeater, which is what keeps an uncommissioned coupler from severing the
 * installation it was just plugged into.
 */
struct CouplerOptions {
    /// Required: the upstream side. Main line for a line coupler, backbone for
    /// a backbone coupler.
    std::unique_ptr<physical::Tp1MediumBackend> primary;

    /// Required: the downstream subnetwork.
    std::unique_ptr<physical::Tp1MediumBackend> secondary;

    /**
     * Publish the Router Object (interface object type 6).
     *
     * On by default: a coupler that does not publish one cannot receive an ETS
     * filter-table download, so it would have to route everything. Turning it
     * off is only sensible for a device that routes by a policy the firmware
     * sets itself and that ETS should not try to configure.
     */
    bool publishRouterObject{true};

    /// PID_MEDIUM for the secondary side. 0 = TP1, 1 = PL110, 2 = RF, 5 = IP.
    uint8_t subMedium{0};

    /// Device-level KNX Data Secure options.
    SecureCommissioningOptions secure{};
};

template <typename ProductDefinitionT, size_t BindingCapacity>
class CommissionedBindingsBuilder {
public:
    using product_definition_type = std::remove_cvref_t<ProductDefinitionT>;
    using endpoint_definition_type = typename product_definition_type::endpoint_definition_type;
    using parameter_schema_type = typename product_definition_type::parameter_schema_type;

    explicit CommissionedBindingsBuilder(endpoint::EndpointBindings<endpoint_definition_type, BindingCapacity> endpointBindings)
        : _endpointBindings(std::move(endpointBindings))
    {
    }

    // Every registration method comes in an lvalue- and an rvalue-qualified
    // form. The rvalue form keeps a chain that started from a temporary —
    // `makeCommissionedBindings(kProduct).onCommand<...>(...)...` — an rvalue
    // all the way to startCommissionedProduct(), which takes the builder by
    // rvalue reference so it is never copied into the caller's stack frame.
    // The builder is several kilobytes for a product of any size, so that copy
    // is the difference between fitting the service task's stack and not.

    /**
     * Register a command handler invoked from CommissionedProductRuntime::loop().
     * Keep the callback non-blocking and treat it as owner-context code.
     */
    template <auto LogicalId, typename Callback>
    auto& onCommand(Callback&& callback) &
    {
        _endpointBindings.template onCommand<LogicalId>(std::forward<Callback>(callback));
        return *this;
    }

    template <auto LogicalId, typename Callback>
    auto onCommand(Callback&& callback) && -> CommissionedBindingsBuilder&&
    {
        onCommand<LogicalId>(std::forward<Callback>(callback));
        return std::move(*this);
    }

    /**
     * Register a writable-state handler invoked from CommissionedProductRuntime::loop().
     */
    template <auto LogicalId, typename Callback>
    auto& onStateWrite(Callback&& callback) &
    {
        _endpointBindings.template onStateWrite<LogicalId>(std::forward<Callback>(callback));
        return *this;
    }

    template <auto LogicalId, typename Callback>
    auto onStateWrite(Callback&& callback) && -> CommissionedBindingsBuilder&&
    {
        onStateWrite<LogicalId>(std::forward<Callback>(callback));
        return std::move(*this);
    }

    /**
     * Register a state provider read from CommissionedProductRuntime::loop().
     */
    template <auto LogicalId, typename Provider>
    auto& provideState(Provider&& provider) &
    {
        _endpointBindings.template provideState<LogicalId>(std::forward<Provider>(provider));
        return *this;
    }

    template <auto LogicalId, typename Provider>
    auto provideState(Provider&& provider) && -> CommissionedBindingsBuilder&&
    {
        provideState<LogicalId>(std::forward<Provider>(provider));
        return std::move(*this);
    }

    auto& onProgrammingModeChanged(endpoint::ProgModeCallback callback) &
    {
        _endpointBindings.onProgrammingModeChanged(std::move(callback));
        return *this;
    }

    auto onProgrammingModeChanged(endpoint::ProgModeCallback callback) && -> CommissionedBindingsBuilder&&
    {
        onProgrammingModeChanged(std::move(callback));
        return std::move(*this);
    }

    auto& onFault(endpoint::FaultCallback callback) &
    {
        _endpointBindings.onFault(std::move(callback));
        return *this;
    }

    auto onFault(endpoint::FaultCallback callback) && -> CommissionedBindingsBuilder&&
    {
        onFault(std::move(callback));
        return std::move(*this);
    }

    template <auto ParameterId, typename Callback>
    auto& onParameterChanged(Callback&& callback) &
    {
        _parameterCallbacks.template set<ParameterId>(std::forward<Callback>(callback));
        return *this;
    }

    template <auto ParameterId, typename Callback>
    auto onParameterChanged(Callback&& callback) && -> CommissionedBindingsBuilder&&
    {
        onParameterChanged<ParameterId>(std::forward<Callback>(callback));
        return std::move(*this);
    }

    /**
     * Register a callback that fires every time the device lifecycle state changes.
     *
     * Called at most once per loop() invocation, only on actual state transitions:
     *   Uncommissioned → Commissioning → Operational (and back)
     *
     * Example:
     * @code
     *   bindings.onLifecycleChanged([](DeviceLifecycleState s) {
     *       if (s == DeviceLifecycleState::Operational) {
     *           indicator_led_set(LED_SOLID_GREEN);
     *       }
     *   });
     * @endcode
     */
    auto& onLifecycleChanged(LifecycleChangedCallback callback) &
    {
        _lifecycleCallback = std::move(callback);
        return *this;
    }

    auto onLifecycleChanged(LifecycleChangedCallback callback) && -> CommissionedBindingsBuilder&&
    {
        onLifecycleChanged(std::move(callback));
        return std::move(*this);
    }

private:
    endpoint::EndpointBindings<endpoint_definition_type, BindingCapacity> _endpointBindings;
    detail::ParameterCallbacks<parameter_schema_type> _parameterCallbacks{};
    LifecycleChangedCallback _lifecycleCallback;

    template <typename DefinitionT, size_t Cap>
    friend class CommissionedProductRuntime;

    template <typename DefinitionT, size_t Cap>
    friend auto makeCommissionedBindings(const DefinitionT& definition)
        -> CommissionedBindingsBuilder<DefinitionT, Cap>;

    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         CommissionedProductOptions options)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;

    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         std::unique_ptr<physical::Tp1MacPhysical> physical)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;

    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         CouplerOptions options)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;

#if KNX_IP_TUNNELING
    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         std::unique_ptr<physical::IpTunnelingPhysical> physical)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;

    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         IpTunnelingOptions options)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;
#endif  // KNX_IP_TUNNELING

#if KNX_FEATURE_NETIP
    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         std::unique_ptr<physical::IpRoutingPhysical> physical)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;

    template <typename DefinitionT, size_t Cap>
    friend auto startCommissionedProduct(platform::Platform& platform,
                                         const DefinitionT& definition,
                                         CommissionedBindingsBuilder<DefinitionT, Cap>&& bindings,
                                         IpRoutingOptions options)
        -> util::Result<CommissionedProductHandle<DefinitionT, Cap>>;
#endif  // KNX_FEATURE_NETIP
};

template <size_t BindingCapacity = kDefaultBindingCapacity, typename ProductDefinitionT>
auto makeCommissionedBindings(const ProductDefinitionT& definition)
    -> CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>
{
    return CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>(
        endpoint::makeEndpointBindings<BindingCapacity>(definition.endpointDefinition));
}

template <typename ProductDefinitionT>
class CommissionedParametersView {
public:
    using product_definition_type = std::remove_cvref_t<ProductDefinitionT>;
    using parameter_schema_type = typename product_definition_type::parameter_schema_type;

    template <auto ParameterId>
    auto get() const -> typename product_definition_type::template ParameterValueType<ParameterId>
    {
        return _state->template get<ParameterId>();
    }

    template <auto ParameterId>
    bool hasCustomValue() const
    {
        return _state->template hasCustomValue<ParameterId>();
    }

private:
    explicit CommissionedParametersView(detail::ParameterState<parameter_schema_type>* state)
        : _state(state)
    {
    }

    detail::ParameterState<parameter_schema_type>* _state;

    template <typename DefinitionT, size_t Cap>
    friend class CommissionedProductRuntime;
};

template <typename ProductDefinitionT, size_t BindingCapacity>
class CommissionedProductRuntime {
public:
    using product_definition_type = std::remove_cvref_t<ProductDefinitionT>;
    using endpoint_definition_type = typename product_definition_type::endpoint_definition_type;
    using parameter_schema_type = typename product_definition_type::parameter_schema_type;
    using port_id_type = typename product_definition_type::port_id_type;
    using endpoint_runtime_type = endpoint::EndpointRuntime<endpoint_definition_type, BindingCapacity>;
    using OwnerWorkHint = typename endpoint_runtime_type::OwnerWorkHint;
    using WorkAvailableCallback = typename endpoint_runtime_type::WorkAvailableCallback;

    // The bindings and the parameter callbacks are taken by rvalue reference,
    // not by value: both are sized by the product's port and parameter counts
    // and run to several kilobytes each, so a by-value parameter would have the
    // caller materialise a full copy in its own stack frame purely to hand it
    // straight into a member. They are consumed here either way.
    CommissionedProductRuntime(const product_definition_type& definition,
                               endpoint::EndpointBindings<endpoint_definition_type, BindingCapacity>&& endpointBindings,
                               detail::ParameterCallbacks<parameter_schema_type>&& parameterCallbacks,
                               LifecycleChangedCallback lifecycleCallback,
                               CommissionedProductOptions options)
        : _definition(definition)
        , _knxPersistenceIdentity(detail::resolveKnxPersistenceIdentity(definition, options.persistence))
        , _parametersState(_definition.parameterSchema, std::move(parameterCallbacks))
        , _lifecycleCallback(std::move(lifecycleCallback))
        , _runtime(
              _definition.endpointDefinition,
              std::move(endpointBindings),
              endpoint::EndpointInstanceConfig{
                  .defaultIndividualAddress =
                      options.standaloneDemoIndividualAddress.value_or(initialIndividualAddress()),
                  .persistenceNamespace = _knxPersistenceIdentity.storageNamespace,
                  .persistenceSchemaVersion = _knxPersistenceIdentity.schemaVersion,
                  .restoreKnxStateOnBoot = options.restoreKnxStateOnBoot,
              })
        , _startInProgrammingMode(options.startInProgrammingMode)
        , _secureOptions(options.secure)
    {
    }

    // Move constructor: re-wire the parameter data callback so it references
    // the new _parametersState address rather than the moved-from one.
    CommissionedProductRuntime(CommissionedProductRuntime&& other) noexcept
        : _definition(std::move(other._definition))
        , _knxPersistenceIdentity(std::move(other._knxPersistenceIdentity))
        , _parametersState(std::move(other._parametersState))
        , _lifecycleCallback(std::move(other._lifecycleCallback))
        , _lastLifecycleState(other._lastLifecycleState)
        , _runtime(std::move(other._runtime))
        , _startInProgrammingMode(other._startInProgrammingMode)
        , _transportPath(other._transportPath)
        , _secureOptions(other._secureOptions)
#if KNX_FEATURE_NETIP && KNX_SECURE_ENABLED
        , _mcTimerStore(std::move(other._mcTimerStore))
#endif
    {
        // Update the APO callback to point at this object's _parametersState.
        // Use the no-reapply variant so existing data (already carried over via
        // the move of _parametersState) is not re-decoded and callbacks don't
        // fire spuriously during construction.
        _runtime.rewireParameterDataCallbackNoReapply([this](std::span<const uint8_t> data) {
            _parametersState.applyFromBytes(data);
        });
        _runtime.rewireCallbacksAfterMove();
    }

    CommissionedProductRuntime& operator=(CommissionedProductRuntime&&) = delete;

    auto start(platform::Platform& platform,
               std::unique_ptr<physical::Tp1MediumBackend> mediumBackend) -> util::Result<void>
    {
        if (!mediumBackend) {
            return util::ErrorCode::InvalidParameter;
        }

        auto startResult = _runtime.start(platform, std::move(mediumBackend));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::Tp1MediumBackend;
        applySecureOptions();

        _runtime.wireParameterDataCallback([this](std::span<const uint8_t> data) {
            _parametersState.applyFromBytes(data);
        });

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }

    /**
     * @brief Start as a KNX coupler over two TP1 ports.
     *
     * Everything the single-port path does still happens — endpoints,
     * parameters, persistence, ETS commissioning — and forwarding between the
     * two ports is added on top.
     */
#if KNX_FEATURE_TP1
    auto start(platform::Platform& platform, CouplerOptions options) -> util::Result<void>
    {
        if (!options.primary || !options.secondary) {
            return util::ErrorCode::InvalidParameter;
        }

        auto stackPort = bau::createTp1CouplerStackPort(
            platform,
            std::make_unique<physical::Tp1MacPhysical>(std::move(options.primary)),
            std::make_unique<physical::Tp1MacPhysical>(std::move(options.secondary)));

        auto startResult = _runtime.start(platform, std::move(stackPort));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::Tp1Coupler;
        _secureOptions = options.secure;
        applySecureOptions();

        if (options.publishRouterObject) {
            // The filter table and routing policy are left null on purpose:
            // configureRouterRole() takes them from the coupler the stack port
            // already built, so there is only ever one of each.
            bau::BusAccessUnit::RouterRoleConfig routerConfig;
            routerConfig.subMedium = options.subMedium;
            auto routerResult = _runtime.configureRouterRole(routerConfig);
            if (routerResult.isError()) {
                return routerResult.error();
            }
        }

        _runtime.wireParameterDataCallback([this](std::span<const uint8_t> data) {
            _parametersState.applyFromBytes(data);
        });

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }

    auto start(platform::Platform& platform,
               std::unique_ptr<physical::Tp1MacPhysical> physical) -> util::Result<void>
    {
        if (!physical) {
            return util::ErrorCode::InvalidParameter;
        }


        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::Tp1MacPhysical;
        applySecureOptions();

        _runtime.wireParameterDataCallback([this](std::span<const uint8_t> data) {
            _parametersState.applyFromBytes(data);
        });

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }
#endif  // KNX_FEATURE_TP1

#if KNX_IP_TUNNELING
    auto start(platform::Platform& platform,
               std::unique_ptr<physical::IpTunnelingPhysical> physical) -> util::Result<void>
    {
        if (!physical) {
            return util::ErrorCode::InvalidParameter;
        }

        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::IpTunneling;
        applySecureOptions();

        _runtime.wireParameterDataCallback([this](std::span<const uint8_t> data) {
            _parametersState.applyFromBytes(data);
        });

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }
#endif  // KNX_IP_TUNNELING

#if KNX_FEATURE_NETIP
    auto start(platform::Platform& platform,
               std::unique_ptr<physical::IpRoutingPhysical> physical) -> util::Result<void>
    {
        if (!physical) {
            return util::ErrorCode::InvalidParameter;
        }

        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::IpRouting;
        applySecureOptions();

        _runtime.wireParameterDataCallback([this](std::span<const uint8_t> data) {
            _parametersState.applyFromBytes(data);
        });

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }

    // -----------------------------------------------------------------------
    // Managed IP transport overloads
    //
    // These overloads accept option structs and handle physical-layer assembly
    // internally, keeping firmware code free of physical_factory.hpp details.
    // The network interface is taken from the supplied Platform.
    // -----------------------------------------------------------------------

#if KNX_IP_TUNNELING
    auto start(platform::Platform& platform, IpTunnelingOptions options) -> util::Result<void>
    {
        if (options.host.raw == 0) {
            return util::ErrorCode::InvalidParameter;
        }

        auto* network = platform.network();
        if (!network) {
            return util::ErrorCode::ResourceUnavailable;
        }

        auto physical = std::make_unique<physical::IpTunnelingPhysical>();
        physical->setNetworkInterface(network);
        physical->setGateway(options.host, options.port);

        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::IpTunnelingManaged;
        _secureOptions = options.secure;
        applySecureOptions();

        _runtime.wireParameterDataCallback([this](std::span<const uint8_t> data) {
            _parametersState.applyFromBytes(data);
        });

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }
#endif  // KNX_IP_TUNNELING

    /// Install the description a Core search response is built from.
    ///
    /// Every field is read when the request arrives rather than captured now:
    /// the individual address changes at commissioning, programming mode
    /// changes at the press of a button, and both decide whether ETS finds this
    /// device at all.
    void configureCoreDiscovery(physical::IpRoutingPhysical& physical,
                                platform::NetworkInterface& network,
                                const IpRoutingOptions& options)
    {
        static constexpr netip::ServiceFamilySupport kFamilies[] = {
            {netip::service_family::kCore, 2},
            {netip::service_family::kRouting, 2},
        };

        const IpAddress multicastGroup = options.multicastGroup;
        std::string friendlyName = options.friendlyName;

        physical.setCoreDiscovery(
            [this, &network, multicastGroup, friendlyName]() {
                netip::CoreDeviceDescription description{};
                description.medium = static_cast<uint8_t>(MediumType::IP);
                description.programmingMode = _runtime.isProgrammingModeActive();
                description.individualAddress = _runtime.individualAddress();
                description.routingMulticastAddress = multicastGroup;
                description.maxLocalApduLength = _runtime.compiledDefinition().identity.maxApduLength;
                // The same mask version setDeviceMedium() puts in the Device
                // Descriptor, from the same table — ETS cross-checks the two.
                description.deviceDescriptorType0 =
                    application::systemBMaskVersion(MediumType::IP);

                network.macAddress(description.macAddress);

                const auto serial = _runtime.deviceObject().getSerialNumber();
                const size_t serialLength = std::min(serial.size(), description.serialNumber.size());
                std::copy_n(serial.begin(), serialLength, description.serialNumber.begin());

                // A name identifies the unit, the order number only the model.
                // Falling back to the model still beats an empty field, which
                // ETS shows as a blank row.
                const std::string_view name =
                    friendlyName.empty() ? std::string_view(_runtime.compiledDefinition().identity.orderNumber)
                                         : std::string_view(friendlyName);
                const size_t nameLength = std::min(name.size(), description.friendlyName.size());
                std::copy_n(name.begin(), nameLength, description.friendlyName.begin());

                return description;
            },
            std::span<const netip::ServiceFamilySupport>(kFamilies));
    }

    auto start(platform::Platform& platform, IpRoutingOptions options) -> util::Result<void>
    {
        if (options.multicastGroup.raw == 0) {
            return util::ErrorCode::InvalidParameter;
        }

        auto* network = platform.network();
        if (!network) {
            return util::ErrorCode::ResourceUnavailable;
        }

        auto physical = std::make_unique<physical::IpRoutingPhysical>();
        physical->setNetworkInterface(network);
        physical->setMulticast(options.multicastGroup, options.port, options.interfaceAddress);
        physical->setMulticastSocketOptions(options.timeToLive, options.multicastLoopback);

        if (options.announceOnNetwork) {
            configureCoreDiscovery(*physical, *network, options);
        }

        // The platform RNG for the ROUTING_BUSY back-off and, under Secure
        // Routing, for message tags and TIMER_NOTIFY delays.
        physical->setRandomSource([&platform](std::span<uint8_t> out) { platform.randomBytes(out); });

#if KNX_SECURE_ENABLED
        _mcTimerStore = std::make_unique<detail::PersistentMcTimerStore>(_knxPersistenceIdentity.storageNamespace);
        physical->setMcTimerStore(_mcTimerStore.get());
        physical->setLatencySettingsSource([this]() { return secureRoutingLatencySettings(); });
#endif

        // Kept across the move: the stack port owns the physical for as long
        // as the runtime lives, and the backbone-key observer below has to
        // reach it after start().
        auto* routing = physical.get();

        auto startResult = _runtime.start(platform, std::move(physical));
        if (startResult.isError()) {
            return startResult.error();
        }

        _transportPath = CommissionedTransportPath::IpRoutingManaged;
        _secureOptions = options.secure;
        applySecureOptions();

#if KNX_SECURE_ENABLED
        // ETS enables KNX/IP Secure Routing by downloading the backbone key as
        // an ordinary property write, and expects it to take effect at once.
        // Wired here rather than left to the firmware because there is exactly
        // one correct thing to do with that key, and a device that stores it
        // and keeps multicasting in the clear looks secured in the project and
        // is not on the wire.
        _runtime.setBackboneKeyCallback([routing](std::span<const uint8_t> key) {
            (void)routing->applySecureRouting(key, physical::IpRoutingPhysical::KeySource::Download);
        });

        // The nonce carries the KNX serial number. It is known once the runtime
        // has started, and has to be in place before any key is applied.
        {
            std::array<uint8_t, 6> serial{};
            const auto deviceSerial = _runtime.deviceObject().getSerialNumber();
            std::copy_n(deviceSerial.begin(), std::min(serial.size(), deviceSerial.size()), serial.begin());
            routing->setSecureRoutingSerial(serial);
        }
        seedSecureRoutingLatencyDefaults();

        // A backbone key ETS downloaded before the last restart came back with
        // the KNXnet/IP Parameter Object while the runtime started, but through
        // the object store rather than a property write, so the callback above
        // never saw it. Without this a secured device reboots into plain
        // multicast and stays there until ETS downloads again.
        const auto storedKey = _runtime.referenceObjectPropertyValue(
            InterfaceObjectType::knxNetIpParameter(), kPidBackboneKey);
        const bool storedKeySet = std::any_of(storedKey.begin(), storedKey.end(),
                                              [](uint8_t b) { return b != 0; });
        if (storedKeySet) {
            (void)routing->applySecureRouting(storedKey, physical::IpRoutingPhysical::KeySource::Restored);
        } else if (options.secureRouting.enabled) {
            (void)routing->applySecureRouting(options.secureRouting.groupKey,
                                              physical::IpRoutingPhysical::KeySource::Restored);
        }
#else
        (void)routing;
#endif

        _runtime.wireParameterDataCallback([this](std::span<const uint8_t> data) {
            _parametersState.applyFromBytes(data);
        });

        if (_startInProgrammingMode && !_runtime.isProgrammingModeActive()) {
            _runtime.toggleProgrammingMode();
        }

        return util::Result<void>::ok();
    }
#endif  // KNX_FEATURE_NETIP

#if KNX_FEATURE_NETIP
    /**
     * @brief Publish this device's live IP identity to the KNXnet/IP Parameter Object.
     *
     * ETS reads a KNX IP device's address, multicast group, MAC and friendly
     * name out of interface object 11. The stack registers the object; only the
     * firmware knows the values, and not until the network interface is up.
     * Call this once it is, and again on every address change.
     */
    util::Result<void> applyKnxNetIpConfiguration(const netip::KnxNetIpConfiguration& config)
    {
        return _runtime.applyKnxNetIpConfiguration(config);
    }

    /**
     * @brief Re-join the routing multicast group after an interface change.
     *
     * The membership is an interface-local thing: it is established when the
     * routing socket opens, and it does not follow the interface afterwards.
     * A device that started before its network was up never joined at all, and
     * one whose lease moved it into a different subnet holds a membership on a
     * route that no longer carries the group. Both are silent — the socket is
     * open, sends succeed, and nothing arrives.
     *
     * So the firmware, which is the only party that learns the interface
     * changed, says so here. Call it whenever the interface gains or changes an
     * address; it is idempotent, and cheap enough that calling it on every such
     * event is the intended use.
     *
     * Returns OperationNotSupported on a product that is not running KNXnet/IP
     * routing, and NotInitialized before start().
     */
    util::Result<void> refreshIpMulticastMembership()
    {
        return _runtime.refreshMulticastMembership();
    }

    /**
     * @brief Decide whether ETS may move this device to another routing group.
     *
     * On KNX IP the Domain Address is the routing multicast address, and ETS
     * writes it by serial number during commissioning (03/03/07 §3.3.7). Only
     * the firmware knows whether it can re-join a different group, so returning
     * an error refuses the change and the device keeps answering with the
     * address it is actually listening on. Without a callback the write is
     * refused, which is the safe default: a recorded address the device is not
     * reachable on is worse than a refusal.
     */
    void onDomainAddressWrite(application::DomainAddressWriteCallback callback)
    {
        _runtime.setDomainAddressWriteCallback(std::move(callback));
    }
#endif

    /**
     * @brief Counters for the port this product is running on.
     *
     * For the question a capture taken on the network cannot answer: when the
     * device stops responding, did the telegram reach it at all? On a KNXnet/IP
     * routing port, `mediumDatagramsReceived` standing still while a client is
     * plainly transmitting means the group is no longer being delivered here,
     * and nothing above the socket can fix that.
     *
     * Zero-filled before start() and on a port that keeps no such counter.
     */
    bau::LinkDiagnostics linkDiagnostics() const
    {
        return _runtime.linkDiagnostics();
    }

    /**
     * @brief The coupler, when the product was started with `CouplerOptions`.
     *
     * nullptr for an ordinary end device. Use it to reach the filter table, the
     * routing policy and the observability callbacks.
     */
    network::TwoPortCoupler* coupler() { return _runtime.coupler(); }

    /**
     * @brief Apply a downloaded coupler configuration to the live routing policy.
     *
     * `PID_MAIN_LCCONFIG` and its siblings are ordinary properties: ETS writes
     * them into the Router Object, and they change nothing until this is
     * called. The filter table is separate — it applies live through
     * `PID_ROUTETABLE_CONTROL`. Call this after a download completes.
     */
    util::Result<void> syncRouterRoutingConfig() { return _runtime.syncRouterRoutingConfig(); }

    /**
     * Progress the runtime and deliver queued firmware callbacks.
     *
     * Call from a single owner context. `onCommand`, `onStateWrite`,
     * `onProgrammingModeChanged`, and `onLifecycleChanged` callbacks run from
     * this method.
     */
    void loop()
    {
        _runtime.loop();

        if (_lifecycleCallback) {
            const auto current = lifecycleState();
            if (current != _lastLifecycleState) {
                _lastLifecycleState = current;
                _lifecycleCallback(current);
            }
        }
    }

    OwnerWorkHint ownerWorkHint() const
    {
        return _runtime.ownerWorkHint();
    }

    void setWorkAvailableCallback(WorkAvailableCallback callback)
    {
        _runtime.setWorkAvailableCallback(std::move(callback));
    }

    /**
     * Returns the current lifecycle state derived from the KNX stack.
     *
     * Equivalent manual check:
     *   isProgrammingModeActive() → Commissioning
     *   isOperationalIndividualAddress(individualAddress()) → Operational
     *   otherwise → Uncommissioned
     */
    DeviceLifecycleState lifecycleState() const
    {
        if (isProgrammingModeActive()) {
            return DeviceLifecycleState::Commissioning;
        }
        if (isOperationalIndividualAddress(individualAddress())) {
            return DeviceLifecycleState::Operational;
        }
        return DeviceLifecycleState::Uncommissioned;
    }

    template <auto LogicalId, typename ValueT>
    auto publish(ValueT&& value) -> util::Result<void>
    {
        return _runtime.template publish<LogicalId>(std::forward<ValueT>(value));
    }

    template <typename ValueT>
    auto publish(port_id_type logicalId, ValueT&& value) -> util::Result<void>
    {
        return _runtime.publish(logicalId, std::forward<ValueT>(value));
    }

    /// True when the ETS project linked a group address to this port. Publishing
    /// on an unlinked port succeeds without emitting a telegram, so query this
    /// when you want to report the unused datapoints of a project.
    bool isPortLinked(port_id_type logicalId) const
    {
        return _runtime.isPortLinked(logicalId);
    }

    template <auto LogicalId>
    bool isPortLinked() const
    {
        return _runtime.template isPortLinked<LogicalId>();
    }

    // ── Outbound transmit shaping (see knx/application/group_object.hpp) ──────
    // Provide a monotonic millisecond clock; required for cyclic sends, per-
    // object min-interval floors, and the telegram rate limiter to take effect.
    void setTimeSource(bau::BusAccessUnit::TimeSourceFn timeSource)
    {
        _runtime.setTimeSource(std::move(timeSource));
    }

    /// Global rate limit for this device's own unsolicited group sends.
    void setTelegramRateLimit(const application::TelegramRateLimitConfig& config)
    {
        _runtime.setTelegramRateLimit(config);
    }

    /// Set/read a port's send-on-change / cyclic / min-interval policy. Typically
    /// driven from ETS parameters via onParameterChanged + endpoint::TransmitPolicyBinder.
    template <auto LogicalId>
    auto setTransmitPolicy(const application::GroupObjectTransmitPolicy& policy) -> util::Result<void>
    {
        return _runtime.setTransmitPolicy(static_cast<port_id_type>(LogicalId), policy);
    }

    auto setTransmitPolicy(port_id_type logicalId,
                           const application::GroupObjectTransmitPolicy& policy) -> util::Result<void>
    {
        return _runtime.setTransmitPolicy(logicalId, policy);
    }

    template <auto LogicalId>
    auto transmitPolicy() const -> util::Result<application::GroupObjectTransmitPolicy>
    {
        return _runtime.transmitPolicy(static_cast<port_id_type>(LogicalId));
    }

    // -----------------------------------------------------------------------
    // Commissioning import helpers
    //
    // These methods are provided for two explicit scenarios:
    //   1. Importing commissioned state from an external source into a running
    //      device (e.g. an ETS-compatible commissioning tool that writes state
    //      through a non-KNX path such as a cloud API or BLE provisioning).
    //   2. Smoke tests and demo setups that need to exercise runtime behavior
    //      without a connected ETS instance.
    //
    // They are NOT needed for normal ETS-managed products. When a real ETS
    // session programs the device over the KNX bus, the stack writes group
    // addresses into the address table and association table through the KNX
    // management protocol and restores them from persistence on reboot.
    // Firmware does not call these methods at all in that flow.
    //
    // Parameter values written via applyCommissionedParameter are persisted
    // through ApplicationProgramObject (PID_PROGRAM_DATA) and are restored
    // on the next boot through the standard KNX management persistence path.
    // -----------------------------------------------------------------------

    template <auto LogicalId>
    auto applyCommissionedGroupAddress(GroupAddress address) -> util::Result<void>
    {
        return _runtime.template bindGroupAddress<LogicalId>(address);
    }

    auto applyCommissionedGroupAddress(port_id_type logicalId, GroupAddress address) -> util::Result<void>
    {
        return _runtime.bindGroupAddress(logicalId, address);
    }

    template <typename... Bindings>
    auto applyCommissionedGroupAddresses(Bindings&&... bindings) -> util::Result<void>
    {
        return endpoint::bindGroupAddresses(_runtime, std::forward<Bindings>(bindings)...);
    }

    // Inject a parameter value. The value is applied immediately to the
    // in-memory ParameterState (firing onParameterChanged) and the full
    // encoded parameter block is written to the ApplicationProgramObject so
    // the value is persisted across reboots and visible to the KNX management
    // model via A_PropertyValue_Read on PID_PROGRAM_DATA.
    template <auto ParameterId>
    auto applyCommissionedParameter(
        typename product_definition_type::template ParameterValueType<ParameterId> value)
        -> util::Result<void>
    {
        const auto result = _parametersState.template apply<ParameterId>(std::move(value));
        if (!result.isError()) {
            const auto bytes = _parametersState.toBytes();
            _runtime.persistParameterBytesOnly(bytes);
        }
        return result;
    }

    /// Device object access for host-specific identity (e.g. serial number
    /// from a factory-programmed MAC).
    auto& deviceObject() { return _runtime.deviceObject(); }

    auto parameters() -> CommissionedParametersView<product_definition_type>
    {
        return CommissionedParametersView<product_definition_type>(&_parametersState);
    }

    auto parameters() const -> CommissionedParametersView<product_definition_type>
    {
        return CommissionedParametersView<product_definition_type>(
            const_cast<detail::ParameterState<parameter_schema_type>*>(&_parametersState));
    }

    auto individualAddress() const -> IndividualAddress
    {
        return _runtime.individualAddress();
    }

    void reportFault(FaultInfo info) const
    {
        _runtime.reportFault(info);
    }

    size_t pendingBusActionCount() const
    {
        return _runtime.pendingActionCount();
    }

    const endpoint::PendingBusAction<port_id_type>* pendingBusActionAt(size_t index) const
    {
        return _runtime.pendingActionAt(index);
    }

    /// Number of group objects registered after start().  Always equals the
    /// number of ports in the product definition (compile-time fixed).
    size_t registeredGroupObjectCount() const
    {
        return _runtime.registeredGroupObjectCount();
    }

    static constexpr size_t kPortCount = product_definition_type::endpoint_definition_type::kPortCount;

    CommissionedTransportPath transportPath() const
    {
        return _transportPath;
    }

    CommissionedSupportTier supportTier() const
    {
        return supportTierForTransportPath(_transportPath);
    }

    bool isProductGradePath() const
    {
        return supportTier() == CommissionedSupportTier::ProductGrade;
    }

    // -----------------------------------------------------------------------
    // KNX Data Secure helpers (Phase C key provisioning API)
    //
    // These convenience methods remove the need for firmware code to know
    // about SecurityInterfaceObject internals.  They must be called after a
    // successful start().
    // -----------------------------------------------------------------------

    /// Apply an ETS tool key (16-byte AES-128) to this device.
    ///
    /// The tool key is stored in the Security Interface Object and allows ETS
    /// to open a secure management session.  If KNX Data Secure mode is not
    /// yet enabled, it is enabled automatically.
    ///
    /// Must be called after start().
    util::Result<void> applyEtsToolKey(const std::array<uint8_t, 16>& key)
    {
        _runtime.enableSecurityMode();
        _runtime.applyToolKey(key);
        return util::Result<void>::ok();
    }

    /// Apply a group key for a specific group address (16-byte AES-128).
    ///
    /// The key is stored in the Security Interface Object and is used to
    /// authenticate and encrypt group communication for that address.
    ///
    /// Must be called after start().
    util::Result<void> applyEtsGroupKey(GroupAddress address, const std::array<uint8_t, 16>& key)
    {
        _runtime.applyGroupKey(address, key);
        return util::Result<void>::ok();
    }

    /// Install the factory Tool Key (FDSK) without asserting Security Mode.
    ///
    /// This is what a device should do at power-up. PID_SECURITY_MODE belongs
    /// to ETS: a device leaves the factory with it off, is commissioned over
    /// plain management by a project that does not use security, and has the
    /// mode switched on by ETS at the end of a secure download. The property
    /// is persisted, so that decision survives a restart -- and firmware that
    /// re-asserts the mode on every boot overwrites it, leaving a device no
    /// unsecured project can ever commission.
    ///
    /// The key is still installed, so ETS can open a secured exchange whenever
    /// it chooses to. Capability without enforcement; see
    /// SecureApplicationLayer::securityUsable().
    ///
    /// Must be called after start().
    util::Result<void> installFactoryToolKey(const std::array<uint8_t, 16>& key)
    {
        _runtime.applyToolKey(key);
        return util::Result<void>::ok();
    }

    /// Turn on KNX Data Secure without touching the Tool Key.
    ///
    /// Needed by firmware that provisions a factory key once and must not
    /// re-apply it on later boots: ETS replaces the Tool Key during secure
    /// commissioning, and that key is restored from persistence.
    ///
    /// Must be called after start().
    util::Result<void> enableSecurityMode()
    {
        _runtime.enableSecurityMode();
        return util::Result<void>::ok();
    }

    /// Returns true if KNX Data Secure mode is currently enabled on this device.
    bool isSecurityEnabled() const
    {
        return _runtime.isSecurityEnabled();
    }

    /// Returns true if an ETS tool key has been applied to this device.
    bool hasEtsToolKey() const
    {
        return _runtime.hasToolKey();
    }

    /// Returns true if the Tool Key in force is @p key.
    ///
    /// Use this, not hasEtsToolKey(), to decide whether a device certificate
    /// built from @p key is still worth printing: hasEtsToolKey() is true for
    /// any key at all, including one ETS installed over the factory key.
    bool toolKeyInForceIs(const std::array<uint8_t, 16>& key) const
    {
        return _runtime.toolKeyMatches(key);
    }

    const auto& knxPersistenceIdentity() const
    {
        return _knxPersistenceIdentity;
    }

    std::string_view knxPersistenceNamespace() const
    {
        return _knxPersistenceIdentity.storageNamespace;
    }

    bool isProgrammingModeActive() const
    {
        return _runtime.isProgrammingModeActive();
    }

    void toggleProgrammingMode()
    {
        _runtime.toggleProgrammingMode();
    }

    void requestProgrammingMode(bool active)
    {
        if (_runtime.isProgrammingModeActive() != active) {
            _runtime.toggleProgrammingMode();
        }
    }

    const auto& definition() const
    {
        return _definition;
    }

    /// Compiled endpoint view: runtime communication-object descriptors plus the
    /// export descriptors that carry each port's key and display name. Useful
    /// for diagnostics such as reporting which ports a project left unlinked.
    const auto& compiledEndpoint() const
    {
        return _runtime.compiledDefinition();
    }

private:
    template <typename DefinitionT, size_t Cap, typename... Bindings>
    friend auto expert::bindDemoGroupAddresses(CommissionedProductRuntime<DefinitionT, Cap>& runtime,
                                               Bindings&&... bindings) -> util::Result<void>;

    template <auto ParameterId, typename DefinitionT, size_t Cap>
    friend auto expert::applyDemoParameterValue(
        CommissionedProductRuntime<DefinitionT, Cap>& runtime,
        typename DefinitionT::template ParameterValueType<ParameterId> value) -> util::Result<void>;

    template <typename DefinitionT, size_t Cap>
    friend void expert::applyParameterDataBytes(CommissionedProductRuntime<DefinitionT, Cap>& runtime,
                                                std::span<const uint8_t> data);

    template <typename... Bindings>
    auto bindCommissionedGroupAddressesForExpertUse(Bindings&&... bindings) -> util::Result<void>
    {
        return endpoint::bindGroupAddresses(_runtime, std::forward<Bindings>(bindings)...);
    }

    template <auto ParameterId>
    auto applyCommissionedParameterForExpertUse(
        typename product_definition_type::template ParameterValueType<ParameterId> value) -> util::Result<void>
    {
        return _parametersState.template apply<ParameterId>(std::move(value));
    }

    void applyParameterDataBytesForExpertUse(std::span<const uint8_t> data)
    {
        // Route through the ApplicationProgramObject so the full KNX management-model
        // write chain (wireParameterDataCallback → APO.notifyProgramDataChanged →
        // ParameterState::applyFromBytes) is exercised.  This is the same path ETS
        // uses when it downloads program data over the KNX bus.
        _runtime.simulateEtsProgramDataWriteForExpertUse(data);
    }

    product_definition_type _definition;
    ResolvedKnxPersistenceIdentity _knxPersistenceIdentity;
    detail::ParameterState<parameter_schema_type> _parametersState;
    LifecycleChangedCallback _lifecycleCallback;
    DeviceLifecycleState _lastLifecycleState{DeviceLifecycleState::Uncommissioned};
    endpoint_runtime_type _runtime;
    bool _startInProgrammingMode{false};
    CommissionedTransportPath _transportPath{CommissionedTransportPath::NotStarted};
    SecureCommissioningOptions _secureOptions{};
#if KNX_FEATURE_NETIP && KNX_SECURE_ENABLED
    /// Held by pointer in the routing physical, hence on the heap.
    std::unique_ptr<detail::PersistentMcTimerStore> _mcTimerStore;

    // KNXnet/IP Parameter Object, 03_08_09 §2.3.1.
    static constexpr application::PropertyID kPidBackboneKey = static_cast<application::PropertyID>(91);
    static constexpr application::PropertyID kPidMulticastLatencyTolerance = static_cast<application::PropertyID>(95);
    static constexpr application::PropertyID kPidSyncLatencyFraction = static_cast<application::PropertyID>(96);

    /// The ex-factory values (§2.3.1.6.3, §2.3.1.7.3) where nothing is stored,
    /// so that what ETS reads is what the device uses.
    void seedSecureRoutingLatencyDefaults()
    {
        const physical::IpRoutingPhysical::LatencySettings defaults{};
        if (_runtime.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(),
                                                  kPidMulticastLatencyTolerance).empty()) {
            const std::array<uint8_t, 2> value{static_cast<uint8_t>(defaults.latencyToleranceMs >> 8),
                                               static_cast<uint8_t>(defaults.latencyToleranceMs & 0xFFu)};
            (void)_runtime.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                                      kPidMulticastLatencyTolerance, value);
        }
        if (_runtime.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(),
                                                  kPidSyncLatencyFraction).empty()) {
            const std::array<uint8_t, 1> value{defaults.syncLatencyFraction};
            (void)_runtime.setReferenceObjectProperty(InterfaceObjectType::knxNetIpParameter(),
                                                      kPidSyncLatencyFraction, value);
        }
    }

    /// Read on the routing receive task once a second; see
    /// IpRoutingPhysical::setLatencySettingsSource.
    physical::IpRoutingPhysical::LatencySettings secureRoutingLatencySettings() const
    {
        physical::IpRoutingPhysical::LatencySettings settings{};
        const auto tolerance = _runtime.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(),
                                                                     kPidMulticastLatencyTolerance);
        if (tolerance.size() == 2) {
            const uint16_t value = static_cast<uint16_t>((tolerance[0] << 8) | tolerance[1]);
            // Zero would discard every frame not from the future; read it as unset.
            if (value != 0) {
                settings.latencyToleranceMs = value;
            }
        }
        const auto fraction = _runtime.referenceObjectPropertyValue(InterfaceObjectType::knxNetIpParameter(),
                                                                    kPidSyncLatencyFraction);
        if (fraction.size() == 1) {
            settings.syncLatencyFraction = fraction[0];
        }
        return settings;
    }
#endif

    void applySecureOptions()
    {
        if (!_secureOptions.enabled) {
            return;
        }
        _runtime.enableSecurityMode();
        if (_secureOptions.toolKey.has_value()) {
            _runtime.applyToolKey(_secureOptions.toolKey.value());
        }
    }
};

template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              CommissionedProductOptions options)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and constructing it here also spares it the
    // move (and the callback re-wiring that move performs) that a by-value
    // return would impose.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{
            .persistence = options.persistence,
            .standaloneDemoIndividualAddress = options.standaloneDemoIndividualAddress,
            .mediumBackend = nullptr,
            .restoreKnxStateOnBoot = options.restoreKnxStateOnBoot,
            .startInProgrammingMode = options.startInProgrammingMode,
            .secure = options.secure,
        });

    auto startResult = runtime->start(platform, std::move(options.mediumBackend));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}

template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    return startCommissionedProduct(
        platform,
        definition,
        std::move(bindings),
        CommissionedProductOptions{
            .mediumBackend = std::move(mediumBackend),
        });
}

/**
 * @brief Overload: start the product as a KNX coupler over two TP1 ports.
 *
 * The result is an ordinary commissioned product that also routes. Endpoints,
 * parameters, ETS export and persistence work exactly as for a single-port
 * device; the Router Object and the filter table are published and bound to
 * the forwarding path automatically.
 *
 * @code
 * auto runtime = startCommissionedProduct(
 *     platform, kCouplerProduct, std::move(bindings),
 *     CouplerOptions{
 *         .primary   = createTp1Backend(mainLinePins),
 *         .secondary = createTp1Backend(subLinePins),
 *     });
 * @endcode
 */
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              CouplerOptions options)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and constructing it here also spares it the
    // move (and the callback re-wiring that move performs) that a by-value
    // return would impose.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{
            .mediumBackend = nullptr,
            .secure = options.secure,
        });

    auto startResult = runtime->start(platform, std::move(options));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}

/// Overload: assemble with a TP1 MAC physical layer (the bit-bang backend).
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<physical::Tp1MacPhysical> physical)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and constructing it here also spares it the
    // move (and the callback re-wiring that move performs) that a by-value
    // return would impose.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{});

    auto startResult = runtime->start(platform, std::move(physical));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}

#if KNX_IP_TUNNELING
/// Overload: assemble with an IP tunneling physical layer (KNXnet/IP tunneling).
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<physical::IpTunnelingPhysical> physical)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and constructing it here also spares it the
    // move (and the callback re-wiring that move performs) that a by-value
    // return would impose.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{});

    auto startResult = runtime->start(platform, std::move(physical));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}
#endif  // KNX_IP_TUNNELING

#if KNX_FEATURE_NETIP
/// Overload: assemble with an IP routing physical layer (KNXnet/IP routing).
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              std::unique_ptr<physical::IpRoutingPhysical> physical)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and constructing it here also spares it the
    // move (and the callback re-wiring that move performs) that a by-value
    // return would impose.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{});

    auto startResult = runtime->start(platform, std::move(physical));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}

#if KNX_IP_TUNNELING
/// Overload: assemble with a KNXnet/IP tunneling managed options struct.
///
/// The platform's `NetworkInterface` is used automatically. Set `options.host`
/// to the KNXnet/IP gateway IP address before calling.
///
/// Transport path: `CommissionedTransportPath::IpTunnelingManaged`
/// Support tier:   `CommissionedSupportTier::Functional`
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              IpTunnelingOptions options)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and constructing it here also spares it the
    // move (and the callback re-wiring that move performs) that a by-value
    // return would impose.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{});

    auto startResult = runtime->start(platform, std::move(options));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}
#endif  // KNX_IP_TUNNELING

/// Overload: assemble with a KNXnet/IP routing managed options struct.
///
/// The platform's `NetworkInterface` is used automatically. Set
/// `options.multicastGroup` to the desired KNX multicast group
/// (standard: 224.0.23.12) before calling.
///
/// Transport path: `CommissionedTransportPath::IpRoutingManaged`
/// Support tier:   `CommissionedSupportTier::Functional`
template <typename ProductDefinitionT, size_t BindingCapacity>
auto startCommissionedProduct(platform::Platform& platform,
                              const ProductDefinitionT& definition,
                              CommissionedBindingsBuilder<ProductDefinitionT, BindingCapacity>&& bindings,
                              IpRoutingOptions options)
    -> util::Result<CommissionedProductHandle<ProductDefinitionT, BindingCapacity>>
{
    // Built directly in its final heap storage: the runtime is far too large to
    // pass through a stack frame, and constructing it here also spares it the
    // move (and the callback re-wiring that move performs) that a by-value
    // return would impose.
    auto runtime = std::make_unique<CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>>(
        definition,
        std::move(bindings)._endpointBindings,
        std::move(bindings)._parameterCallbacks,
        std::move(bindings)._lifecycleCallback,
        CommissionedProductOptions{});

    auto startResult = runtime->start(platform, std::move(options));
    if (startResult.isError()) {
        return startResult.error();
    }

    return runtime;
}
#endif  // KNX_FEATURE_NETIP

/// Build a StaticExportDescriptor that includes both communication objects and
/// parameter descriptors from a CommissionedProductDefinition.
///
/// Use this instead of makeExportDescriptor() when your product has parameters
/// that should be visible in the ETS catalogue.
///
/// @code
///   constexpr auto kExport = makeCommissionedExportDescriptor(kMyProduct);
///   const auto json = exportDescriptorToJson(kExport);
/// @endcode
template <typename EndpointDefinitionT, typename ParameterSchemaT>
constexpr auto makeCommissionedExportDescriptor(
    const CommissionedProductDefinition<EndpointDefinitionT, ParameterSchemaT>& definition)
{
    using endpoint_def_t = std::remove_cvref_t<EndpointDefinitionT>;
    using schema_t = std::remove_cvref_t<ParameterSchemaT>;

    constexpr size_t kParamCount = schema_t::kParameterCount;

    const auto compiled = compileEndpointDefinition(definition.endpointDefinition);
    const auto paramDescriptors = detail::makeExportParameterDescriptors(definition.parameterSchema);

    StaticExportDescriptor<endpoint_def_t::kPortCount, kParamCount> result{
        .identity = compiled.exportDescriptor.identity,
        .features = compiled.exportDescriptor.features,
        .security = compiled.exportDescriptor.security,
        .capacities = compiled.exportDescriptor.capacities,
        .communicationObjects = compiled.exportDescriptor.communicationObjects,
        .parameters = paramDescriptors,
    };

    return result;
}

} // namespace knx::product