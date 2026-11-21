// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file port_table.hpp
 * @brief The port table: what the runtime knows about a product's ports, as rodata.
 *
 * Every port of an endpoint definition becomes one fixed-size PortDescriptor
 * in a `static constexpr` table derived from the definition's type
 * (kPortTable). A port's slot is its index in that table, which is also its
 * group-object index. The table carries no strings: keys and display names
 * live in a second table (kPortNames) that only the exporter and the tests
 * refer to, so a device image that never touches it does not carry them.
 *
 * The runtime machinery that turns a table entry into behaviour is not
 * templated on the port: see ProductPortCore (impl/runtime/product_port_core.hpp).
 * The descriptor is therefore free of group-object types, which keeps the
 * registration of group objects replaceable (small-target-profile.md 6.2).
 */

#pragma once

#include "knx/application/dpt.hpp"
#include "knx/product/impl/model/endpoint_definition.hpp"
#include "knx/product/product_api_types.hpp"
#include "knx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace knx::product {

/// Bits of PortDescriptor::flags.
namespace port_flags {
inline constexpr uint8_t kReadable = 0x01u;
inline constexpr uint8_t kWritable = 0x02u;
inline constexpr uint8_t kTransmit = 0x04u;
inline constexpr uint8_t kReceivable = 0x08u;
inline constexpr uint8_t kPersisted = 0x10u;
inline constexpr uint8_t kReadOnInit = 0x20u;
} // namespace port_flags

/// "No entry": the port cannot have a binding of this kind.
inline constexpr uint16_t kNoBindingRank = 0xFFFFu;

/**
 * @brief One port as the runtime sees it. 14 bytes, no pointers.
 *
 * receiveRank and stateRank index the binding entry arrays of the product's
 * EndpointBindings: command entries exist only for ports that can receive a
 * write, state providers only for readable ports, so a product pays for the
 * entries it can use. State-write entries exist only for the ports that have
 * a state to write, those that are readable and receive writes: they take the
 * first receive ranks (kStateWritePortCount of them), so the receive rank
 * indexes them too and the descriptor needs no third rank.
 */
struct PortDescriptor {
    uint16_t logicalId{0};
    application::DptId dpt{};
    application::DptValue::Type valueType{application::DptValue::Type::Unsupported};
    uint8_t flags{0};
    Priority priority{Priority::Low};
    PortDirection direction{PortDirection::CommandIn};
    uint16_t receiveRank{kNoBindingRank};
    uint16_t stateRank{kNoBindingRank};

    constexpr bool readable() const { return (flags & port_flags::kReadable) != 0u; }
    constexpr bool writable() const { return (flags & port_flags::kWritable) != 0u; }
    constexpr bool transmit() const { return (flags & port_flags::kTransmit) != 0u; }
    constexpr bool receivable() const { return (flags & port_flags::kReceivable) != 0u; }
    constexpr bool persisted() const { return (flags & port_flags::kPersisted) != 0u; }
    constexpr bool readOnInit() const { return (flags & port_flags::kReadOnInit) != 0u; }
    /// A write from the bus can reach this port's handlers.
    constexpr bool acceptsWrites() const { return receivable() || writable(); }
    /// A write from the bus changes a state the port also reports: it can take
    /// a state-write handler (onStateWrite) as well as a command handler.
    constexpr bool acceptsStateWrites() const { return acceptsWrites() && readable(); }
};

/// The strings of one port. Only the exporter and tests read these.
struct PortNames {
    std::string_view key{};
    std::string_view displayName{};
};

namespace detail {

template <typename PortSpecT>
constexpr PortDescriptor makePortDescriptor()
{
    static_assert(static_cast<unsigned long long>(PortSpecT::logicalId) <= 0xFFFFull,
                  "a logical port id must fit in 16 bits");

    uint8_t flags = 0u;
    if (PortSpecT::readable) flags |= port_flags::kReadable;
    if (PortSpecT::writable) flags |= port_flags::kWritable;
    if (PortSpecT::transmit) flags |= port_flags::kTransmit;
    if (PortSpecT::receivable) flags |= port_flags::kReceivable;
    if (PortSpecT::persisted) flags |= port_flags::kPersisted;
    if (PortSpecT::readOnInit) flags |= port_flags::kReadOnInit;

    return PortDescriptor{
        .logicalId = static_cast<uint16_t>(PortSpecT::logicalId),
        .dpt = PortSpecT::dpt,
        .valueType = application::dptValueTypeFor(PortSpecT::dpt),
        .flags = flags,
        .priority = PortSpecT::priority,
        .direction = PortSpecT::direction,
    };
}

template <typename PortsTuple>
struct PortTableOf;

template <typename... Ports>
struct PortTableOf<std::tuple<Ports...>> {
    static constexpr size_t kCount = sizeof...(Ports);

    static constexpr std::array<PortDescriptor, kCount> makeTable()
    {
        std::array<PortDescriptor, kCount> table{makePortDescriptor<Ports>()...};
        // Receive ranks: the ports that take state writes first, so that the
        // (smaller) state-write array is indexed by the same rank.
        uint16_t receive = 0u;
        for (auto& entry : table) {
            if (entry.acceptsStateWrites()) {
                entry.receiveRank = receive++;
            }
        }
        for (auto& entry : table) {
            if (entry.acceptsWrites() && !entry.acceptsStateWrites()) {
                entry.receiveRank = receive++;
            }
        }
        uint16_t state = 0u;
        for (auto& entry : table) {
            if (entry.readable()) {
                entry.stateRank = state++;
            }
        }
        return table;
    }

    static constexpr std::array<PortNames, kCount> makeNames()
    {
        return {PortNames{Ports::key.view(), Ports::displayName.view()}...};
    }

    static constexpr size_t countReceive()
    {
        size_t count = 0u;
        for (const auto& entry : makeTable()) {
            count += entry.receiveRank != kNoBindingRank ? 1u : 0u;
        }
        return count;
    }

    static constexpr size_t countState()
    {
        size_t count = 0u;
        for (const auto& entry : makeTable()) {
            count += entry.stateRank != kNoBindingRank ? 1u : 0u;
        }
        return count;
    }

    static constexpr size_t countStateWrite()
    {
        size_t count = 0u;
        for (const auto& entry : makeTable()) {
            count += entry.acceptsStateWrites() ? 1u : 0u;
        }
        return count;
    }

    static constexpr std::array<PortDescriptor, kCount> table = makeTable();
    static constexpr std::array<PortNames, kCount> names = makeNames();
    static constexpr size_t receiveCount = countReceive();
    static constexpr size_t stateCount = countState();
    static constexpr size_t stateWriteCount = countStateWrite();
};

} // namespace detail

/// The runtime table of a definition's ports: slot == index.
template <typename DefinitionT>
inline constexpr const auto& kPortTable =
    detail::PortTableOf<typename std::remove_cvref_t<DefinitionT>::ports_tuple>::table;

/// The keys and display names of a definition's ports, in slot order. Not
/// referenced by the runtime; the exporter and tests are its readers.
template <typename DefinitionT>
inline constexpr const auto& kPortNames =
    detail::PortTableOf<typename std::remove_cvref_t<DefinitionT>::ports_tuple>::names;

/// Ports that can receive a write (receive entries needed).
template <typename DefinitionT>
inline constexpr size_t kReceivePortCount =
    detail::PortTableOf<typename std::remove_cvref_t<DefinitionT>::ports_tuple>::receiveCount;

/// Ports that are readable and receive writes (state-write entries needed).
/// They hold receive ranks 0 .. kStateWritePortCount - 1.
template <typename DefinitionT>
inline constexpr size_t kStateWritePortCount =
    detail::PortTableOf<typename std::remove_cvref_t<DefinitionT>::ports_tuple>::stateWriteCount;

/// Readable ports (state provider entries needed).
template <typename DefinitionT>
inline constexpr size_t kStatePortCount =
    detail::PortTableOf<typename std::remove_cvref_t<DefinitionT>::ports_tuple>::stateCount;

/// Slot of the port with this logical id in @p table, or kNoSlot.
inline constexpr uint16_t kNoSlot = 0xFFFFu;

constexpr uint16_t findPortSlot(const PortDescriptor* table, size_t count, uint16_t logicalId)
{
    for (size_t slot = 0; slot < count; ++slot) {
        if (table[slot].logicalId == logicalId) {
            return static_cast<uint16_t>(slot);
        }
    }
    return kNoSlot;
}

/// Slot of a port known at compile time (its index in the table).
template <typename DefinitionT, auto LogicalId>
consteval uint16_t portSlotOf()
{
    return static_cast<uint16_t>(port_index_v<DefinitionT, LogicalId>);
}

} // namespace knx::product
