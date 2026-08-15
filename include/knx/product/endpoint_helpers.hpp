// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_helpers.hpp
 * @brief Factory helpers for constructing endpoint bindings and runtime instances.
 */

#pragma once

#include "knx/product/endpoint_bindings.hpp"
#include "knx/product/endpoint_runtime.hpp"

#include <concepts>
#include <type_traits>
#include <utility>

namespace knx::product::endpoint {

template <size_t BindingCapacity = kDefaultBindingCapacity, typename DefinitionT>
constexpr auto makeEndpointBindings(const DefinitionT&) -> EndpointBindings<DefinitionT, BindingCapacity>
{
    return {};
}

template <typename DefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
auto makeEndpointRuntime(const DefinitionT& definition,
                         EndpointBindings<DefinitionT, BindingCapacity>&& bindings,
                         EndpointInstanceConfig config) -> EndpointRuntime<DefinitionT, BindingCapacity>
{
    return EndpointRuntime<DefinitionT, BindingCapacity>(definition, std::move(bindings), config);
}

template <typename PortIdEnum>
struct GroupAddressBinding {
    PortIdEnum logicalId{};
    GroupAddress address{};
};

template <typename PortIdEnum>
constexpr auto groupAddressBinding(PortIdEnum logicalId,
                                   GroupAddress address) -> GroupAddressBinding<PortIdEnum>
{
    return GroupAddressBinding<PortIdEnum>{
        .logicalId = logicalId,
        .address = address,
    };
}

template <typename RuntimeT, typename... Bindings>
    requires (sizeof...(Bindings) > 0u)
          && (std::same_as<std::remove_cvref_t<Bindings>,
                           GroupAddressBinding<typename RuntimeT::port_id_type>>
              && ...)
util::Result<void> bindGroupAddresses(RuntimeT& runtime, Bindings&&... bindings)
{
    util::Result<void> result = util::Result<void>::ok();
    const auto bindOne = [&](const auto& binding) {
        result = runtime.bindGroupAddress(binding.logicalId, binding.address);
        return result.isOk();
    };

    if ((bindOne(bindings) && ...)) {
        return util::Result<void>::ok();
    }

    return result;
}

} // namespace knx::product::endpoint