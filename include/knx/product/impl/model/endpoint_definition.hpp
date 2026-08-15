// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_definition.hpp
 * @brief Endpoint-definition model: typed port specs, persistence policy,
 *        product identity, and compile-time endpoint structure.
 *
 * Lives under knx/product/impl/model. The public surface is re-exported through
 * knx/product/endpoint_definition.hpp.
 */

#pragma once

#include "knx/application/dpt.hpp"
#include "knx/product/product_api_types.hpp"
#include "knx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace knx::product {

template <size_t N>
struct FixedString {
    char value[N]{};

    constexpr FixedString(const char (&text)[N])
    {
        for (size_t index = 0; index < N; ++index) {
            value[index] = text[index];
        }
    }

    constexpr std::string_view view() const
    {
        return std::string_view(value, N - 1u);
    }

    constexpr operator std::string_view() const
    {
        return view();
    }

    constexpr auto operator<=>(const FixedString&) const = default;
};

template <size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

struct ProductIdentity {
    std::string_view productKey{};
    std::string_view productDisplayName{};
    ManufacturerId manufacturerId{};
    Medium medium{Medium::TP1};
    uint16_t applicationNumber{0};
    uint16_t applicationVersion{0};
    uint8_t firmwareRevision{0};
    uint16_t maxApduLength{254};

    // Hardware identity. These feed both the device's PID_HARDWARE_TYPE /
    // PID_ORDER_INFO / PID_VERSION *and* the generated knxprod's
    // Hardware/@SerialNumber, Hardware/@VersionNumber and Product/@OrderNumber.
    // They live here precisely so the two cannot drift: ETS compares the
    // catalogue entry against what the device reports and refuses the download
    // when they disagree.
    uint16_t hardwareSerialNumber{1};
    uint8_t hardwareVersion{1};
    std::string_view orderNumber{};
};

struct PersistencePolicy {
    std::string_view namespacePrefix{};
    uint16_t schemaVersion{1};
    bool persistKnxState{true};
};

/// Minimum Data Secure level an individual group object insists on, mirroring
/// the KNX schema's ComObjectSecurityRequirements_t enumeration exactly.
///
/// Note this is a floor, not a switch: there is no "optional" value, because
/// whether Data Secure is used at all is a per-device decision the integrator
/// makes in ETS once ApplicationProgram/@IsSecureEnabled is set.  Leaving every
/// object at None is what lets that choice stay with the integrator.
enum class SecurityRequirement : uint8_t {
    None = 0,    ///< No floor; the device-level Secure setting governs.
    Auth,        ///< Object requires at least authentication.
    AuthAndConf, ///< Object requires authentication and encryption.
};

/// What the product *declares* to ETS about KNX Data Secure.
///
/// This is the catalogue-entry side only.  Whether the running device actually
/// performs Data Secure is decided when the stack is started (see
/// CommissionedProduct::applyEtsToolKey); the two must be kept consistent by
/// hand, because a device that advertises Data Secure but cannot honour it will
/// fail commissioning.
struct SecurityPolicy {
    /// Emits ApplicationProgram/@IsSecureEnabled.  ETS treats a missing or
    /// false attribute as "plain device" and hides all Secure options.
    bool dataSecureCapable{false};

    /// Applied to every ComObject/ComObjectRef as @SecurityRequired.  Keep at
    /// None unless an object is meaningless without protection — forcing a
    /// level here removes the integrator's ability to run the device plain.
    SecurityRequirement groupObjectRequirement{SecurityRequirement::None};

    /// Security individual-address table size (senders the device can hold
    /// keys for, tool access included).
    uint16_t individualAddressEntries{1};

    /// Group-key table size.  0 means "one key slot per group object", which
    /// is the worst case of every object sitting on its own secure GA.
    uint16_t groupKeyTableEntries{0};

    /// Point-to-point key table size; one slot covers the ETS tool key.
    uint16_t p2pKeyTableEntries{1};
};

enum class PortDirection : uint8_t {
    CommandIn = 0,
    StateOut,
    StateInOut,
    EventOut,
    Parameter,
};

template <auto LogicalId,
          typename ValueT,
          FixedString Key,
          FixedString DisplayName,
          application::DptId Dpt,
          PortDirection Direction,
          bool Readable,
          bool Writable,
          bool Transmit,
          bool Receivable,
          bool Persisted,
          // Trailing, defaulted: the remaining two KNX communication flags.
          // Communication enable is not a template parameter — a port that
          // exists is always communication-enabled at build time; ETS can
          // still clear the flag at commissioning time.
          bool ReadOnInit = false,
          Priority TxPriority = Priority::Low>
struct PortSpec {
    using logical_id_type = std::remove_cv_t<decltype(LogicalId)>;
    using value_type = ValueT;

    static constexpr auto logicalId = LogicalId;
    static constexpr auto key = Key;
    static constexpr auto displayName = DisplayName;
    static constexpr auto dpt = Dpt;
    static constexpr auto direction = Direction;
    static constexpr bool readable = Readable;
    static constexpr bool writable = Writable;
    static constexpr bool transmit = Transmit;
    static constexpr bool receivable = Receivable;
    static constexpr bool persisted = Persisted;
    static constexpr bool readOnInit = ReadOnInit;
    static constexpr Priority priority = TxPriority;
};

// ── Port modifiers ───────────────────────────────────────────────────────────
//
// Read-on-init and transmission priority are per-object KNX flags that only a
// minority of ports want, so they are trailing defaults on PortSpec rather than
// parameters on every semantic alias.  These transformers make them reachable
// without forcing every alias to grow two more arguments, and without asking a
// product author to spell out a thirteen-parameter PortSpec by hand:
//
//     semantics::ReadOnInit<semantics::TemperatureStateInOut<Port::Setpoint, "setpoint">>
//     semantics::WithPriority<semantics::AlarmState<Port::Alarm, "alarm">, Priority::Urgent>
//
// They compose, so a port can carry both.

namespace detail {

template <typename PortSpecT, bool NewReadOnInit>
struct OverrideReadOnInit;

template <auto LogicalId, typename ValueT, FixedString Key, FixedString DisplayName,
          application::DptId Dpt, PortDirection Direction, bool Readable, bool Writable,
          bool Transmit, bool Receivable, bool Persisted, bool ReadOnInit,
          Priority TxPriority, bool NewReadOnInit>
struct OverrideReadOnInit<PortSpec<LogicalId, ValueT, Key, DisplayName, Dpt, Direction,
                                   Readable, Writable, Transmit, Receivable, Persisted,
                                   ReadOnInit, TxPriority>,
                          NewReadOnInit> {
    using type = PortSpec<LogicalId, ValueT, Key, DisplayName, Dpt, Direction,
                          Readable, Writable, Transmit, Receivable, Persisted,
                          NewReadOnInit, TxPriority>;
};

template <typename PortSpecT, Priority NewPriority>
struct OverridePriority;

template <auto LogicalId, typename ValueT, FixedString Key, FixedString DisplayName,
          application::DptId Dpt, PortDirection Direction, bool Readable, bool Writable,
          bool Transmit, bool Receivable, bool Persisted, bool ReadOnInit,
          Priority TxPriority, Priority NewPriority>
struct OverridePriority<PortSpec<LogicalId, ValueT, Key, DisplayName, Dpt, Direction,
                                 Readable, Writable, Transmit, Receivable, Persisted,
                                 ReadOnInit, TxPriority>,
                        NewPriority> {
    using type = PortSpec<LogicalId, ValueT, Key, DisplayName, Dpt, Direction,
                          Readable, Writable, Transmit, Receivable, Persisted,
                          ReadOnInit, NewPriority>;
};

} // namespace detail

/**
 * @brief Give a port Value-Read-on-Initialisation (KNX flag I).
 *
 * After reset the device issues an A_GroupValue_Read to refresh this object.
 * Use sparingly: 03/05/01 §4.12.5.2.4.1.3 warns the feature multiplies bus load
 * after a whole-installation restart, and explicitly advises against enabling it
 * by default in an average application.  It earns its keep on objects the device
 * cannot derive locally — a setpoint or an operating mode owned by another
 * device — and not on sensor outputs the device produces itself.
 */
template <typename PortSpecT>
using ReadOnInit = typename detail::OverrideReadOnInit<PortSpecT, true>::type;

/**
 * @brief Set the transmission priority of telegrams a port originates.
 *
 * Default is Priority::Low, which is correct for routine sensor traffic. Raise
 * it only for genuinely time-critical objects such as alarms; a device that
 * sends everything at Urgent priority degrades the whole line for everyone.
 */
template <typename PortSpecT, Priority TxPriority>
using WithPriority = typename detail::OverridePriority<PortSpecT, TxPriority>::type;

template <auto LogicalId,
          typename ValueT,
          FixedString Key,
          FixedString DisplayName,
          application::DptId Dpt,
          bool Persisted = false>
using CommandPort = PortSpec<LogicalId,
                             ValueT,
                             Key,
                             DisplayName,
                             Dpt,
                             PortDirection::CommandIn,
                             false,
                             true,
                             false,
                             true,
                             Persisted>;

template <auto LogicalId,
          typename ValueT,
          FixedString Key,
          FixedString DisplayName,
          application::DptId Dpt,
          bool Persisted = true>
using StatePort = PortSpec<LogicalId,
                           ValueT,
                           Key,
                           DisplayName,
                           Dpt,
                           PortDirection::StateOut,
                           true,
                           false,
                           true,
                           false,
                           Persisted>;

template <auto LogicalId,
          typename ValueT,
          FixedString Key,
          FixedString DisplayName,
          application::DptId Dpt,
          bool Persisted = true>
using StateInOutPort = PortSpec<LogicalId,
                                ValueT,
                                Key,
                                DisplayName,
                                Dpt,
                                PortDirection::StateInOut,
                                true,
                                true,
                                true,
                                true,
                                Persisted>;

namespace detail {

template <auto... Ids>
consteval bool logicalIdsUnique()
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

template <typename PortIdEnum, typename Port>
inline constexpr bool kCompatiblePortIdType =
    std::same_as<std::remove_cv_t<typename Port::logical_id_type>, std::remove_cv_t<PortIdEnum>>;

template <auto LogicalId, typename... Ports>
consteval size_t findPortIndex()
{
    constexpr auto logicalIds = std::array{Ports::logicalId...};
    for (size_t index = 0; index < logicalIds.size(); ++index) {
        if (logicalIds[index] == LogicalId) {
            return index;
        }
    }

    return logicalIds.size();
}

} // namespace detail

template <typename PortIdEnum, typename... Ports>
struct EndpointDefinition {
    static_assert(sizeof...(Ports) > 0u, "EndpointDefinition requires at least one port");
    static_assert((detail::kCompatiblePortIdType<PortIdEnum, Ports> && ...),
                  "All ports in an EndpointDefinition must use the same logical port ID type");
    static_assert(detail::logicalIdsUnique<Ports::logicalId...>(),
                  "EndpointDefinition logical port IDs must be unique");

    using port_id_type = std::remove_cv_t<PortIdEnum>;
    using ports_tuple = std::tuple<Ports...>;

    static constexpr size_t kPortCount = sizeof...(Ports);

    ProductIdentity identity{};
    PersistencePolicy persistence{};
    SecurityPolicy security{};
};

template <typename PortIdEnum, typename... Ports>
constexpr auto makeEndpointDefinition(ProductIdentity identity,
                                      PersistencePolicy persistence,
                                      SecurityPolicy security = {})
    -> EndpointDefinition<PortIdEnum, Ports...>
{
    return EndpointDefinition<PortIdEnum, Ports...>{
        .identity = identity,
        .persistence = persistence,
        .security = security,
    };
}

template <typename DefinitionT, auto LogicalId>
struct PortSpecFor;

template <typename PortIdEnum, typename... Ports, auto LogicalId>
struct PortSpecFor<EndpointDefinition<PortIdEnum, Ports...>, LogicalId> {
    static constexpr size_t index = detail::findPortIndex<LogicalId, Ports...>();
    static_assert(index < sizeof...(Ports), "Logical port ID is not part of this endpoint definition");

    using type = std::tuple_element_t<index, std::tuple<Ports...>>;
};

template <typename DefinitionT, auto LogicalId>
using port_spec_t = typename PortSpecFor<std::remove_cvref_t<DefinitionT>, LogicalId>::type;

template <typename DefinitionT, auto LogicalId>
using port_value_t = typename port_spec_t<DefinitionT, LogicalId>::value_type;

template <typename DefinitionT, auto LogicalId>
inline constexpr size_t port_index_v = PortSpecFor<std::remove_cvref_t<DefinitionT>, LogicalId>::index;

} // namespace knx::product