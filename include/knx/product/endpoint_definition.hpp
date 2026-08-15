// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_definition.hpp
 * @brief Canonical public endpoint-definition surface.
 */

#pragma once

#include "knx/product/impl/model/endpoint_definition.hpp"

namespace knx::product::endpoint {

template <size_t N>
using FixedString = knx::product::FixedString<N>;

using ProductIdentity = knx::product::ProductIdentity;
using PersistencePolicy = knx::product::PersistencePolicy;
using Medium = knx::product::Medium;
using PortDirection = knx::product::PortDirection;



// Port modifiers (see impl/model/endpoint_definition.hpp): the ergonomic way to
// reach the trailing read-on-init and priority PortSpec parameters.
template <typename PortSpecT>
using ReadOnInit = knx::product::ReadOnInit<PortSpecT>;

template <typename PortSpecT, Priority TxPriority>
using WithPriority = knx::product::WithPriority<PortSpecT, TxPriority>;

template <auto LogicalId,
          typename ValueT,
          knx::product::FixedString Key,
          knx::product::FixedString DisplayName,
          application::DptId Dpt,
          PortDirection Direction,
          bool Readable,
          bool Writable,
          bool Transmit,
          bool Receivable,
          bool Persisted>
using PortSpec = knx::product::PortSpec<LogicalId,
                                           ValueT,
                                           Key,
                                           DisplayName,
                                           Dpt,
                                           Direction,
                                           Readable,
                                           Writable,
                                           Transmit,
                                           Receivable,
                                           Persisted>;

template <auto LogicalId,
          typename ValueT,
          knx::product::FixedString Key,
          knx::product::FixedString DisplayName,
          application::DptId Dpt,
          bool Persisted = false>
using CommandPort = knx::product::CommandPort<LogicalId, ValueT, Key, DisplayName, Dpt, Persisted>;

template <auto LogicalId,
          typename ValueT,
          knx::product::FixedString Key,
          knx::product::FixedString DisplayName,
          application::DptId Dpt,
          bool Persisted = true>
using StatePort = knx::product::StatePort<LogicalId, ValueT, Key, DisplayName, Dpt, Persisted>;

template <auto LogicalId,
          typename ValueT,
          knx::product::FixedString Key,
          knx::product::FixedString DisplayName,
          application::DptId Dpt,
          bool Persisted = true>
using StateInOutPort =
    knx::product::StateInOutPort<LogicalId, ValueT, Key, DisplayName, Dpt, Persisted>;

template <typename PortIdEnum, typename... Ports>
using EndpointDefinition = knx::product::EndpointDefinition<PortIdEnum, Ports...>;

using knx::product::makeEndpointDefinition;

template <typename DefinitionT, auto LogicalId>
using port_spec_t = knx::product::port_spec_t<DefinitionT, LogicalId>;

template <typename DefinitionT, auto LogicalId>
using port_value_t = knx::product::port_value_t<DefinitionT, LogicalId>;

template <typename DefinitionT, auto LogicalId>
inline constexpr size_t port_index_v = knx::product::port_index_v<DefinitionT, LogicalId>;

} // namespace knx::product::endpoint