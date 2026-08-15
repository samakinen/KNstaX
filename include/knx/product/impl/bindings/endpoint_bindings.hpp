// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_bindings.hpp
 * @brief Business-logic binding layer for the KNstaX endpoint product model.
 */

#pragma once

#include "knx/product/fault_model.hpp"
#include "knx/product/impl/model/endpoint_definition.hpp"
#include "knx/util/inplace_function.hpp"

#include <concepts>
#include <functional>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace knx::product {

using ProgModeCallback = util::InplaceFunction<void(bool active), 32>;

/// Default binding slot capacity (0 → use std::function; >0 → use InplaceFunction<..., N>).
static constexpr size_t kDefaultBindingCapacity = 0;

namespace detail {

/// Select the callable storage type based on the binding capacity.
/// Capacity==0 → std::function (heap-backed, unlimited size).
/// Capacity >0 → InplaceFunction<Sig, Capacity> (stack-backed, bounded size).
template <typename Signature, size_t Capacity>
using binding_fn_t = std::conditional_t<
    (Capacity > 0),
    knx::util::InplaceFunction<Signature, Capacity>,
    std::function<Signature>>;

} // namespace detail

template <typename PortSpec, size_t BindingCapacity = kDefaultBindingCapacity>
struct PortBindingSlot {
    using value_type = typename PortSpec::value_type;

    detail::binding_fn_t<void(value_type), BindingCapacity> commandHandler;
    detail::binding_fn_t<value_type(), BindingCapacity>     stateProvider;
    detail::binding_fn_t<void(value_type), BindingCapacity> stateWriteHandler;
};

namespace detail {

template <typename TupleT, size_t BindingCapacity = kDefaultBindingCapacity>
struct SlotTupleFromPorts;

template <size_t BindingCapacity, typename... Ports>
struct SlotTupleFromPorts<std::tuple<Ports...>, BindingCapacity> {
    using type = std::tuple<PortBindingSlot<Ports, BindingCapacity>...>;
};

} // namespace detail

template <typename DefinitionT, size_t BindingCapacity = kDefaultBindingCapacity>
class EndpointBindings {
public:
    using definition_type = std::remove_cvref_t<DefinitionT>;
    using port_id_type = typename definition_type::port_id_type;
    using slot_tuple_type = typename detail::SlotTupleFromPorts<typename definition_type::ports_tuple, BindingCapacity>::type;

    EndpointBindings() = default;

    template <auto LogicalId, typename Fn>
        requires std::invocable<Fn&, port_value_t<definition_type, LogicalId>>
    EndpointBindings& onCommand(Fn&& fn)
    {
        using value_type = port_value_t<definition_type, LogicalId>;
        auto& bindingSlot = slot<LogicalId>();
        bindingSlot.commandHandler = [handler = std::forward<Fn>(fn)](value_type value) mutable {
            std::invoke(handler, value);
        };
        return *this;
    }

    template <auto LogicalId, typename Fn>
        requires std::invocable<Fn&> && std::convertible_to<std::invoke_result_t<Fn&>, port_value_t<definition_type, LogicalId>>
    EndpointBindings& provideState(Fn&& fn)
    {
        using value_type = port_value_t<definition_type, LogicalId>;
        auto& bindingSlot = slot<LogicalId>();
        bindingSlot.stateProvider = [provider = std::forward<Fn>(fn)]() mutable -> value_type {
            return static_cast<value_type>(std::invoke(provider));
        };
        return *this;
    }

    template <auto LogicalId, typename Fn>
        requires std::invocable<Fn&, port_value_t<definition_type, LogicalId>>
    EndpointBindings& onStateWrite(Fn&& fn)
    {
        using value_type = port_value_t<definition_type, LogicalId>;
        auto& bindingSlot = slot<LogicalId>();
        bindingSlot.stateWriteHandler = [handler = std::forward<Fn>(fn)](value_type value) mutable {
            std::invoke(handler, value);
        };
        return *this;
    }

    EndpointBindings& onProgrammingModeChanged(ProgModeCallback callback)
    {
        _progModeCallback = std::move(callback);
        return *this;
    }

    EndpointBindings& onFault(FaultCallback callback)
    {
        _faultCallback = std::move(callback);
        return *this;
    }

    template <auto LogicalId>
    bool hasCommandBinding() const
    {
        return static_cast<bool>(slot<LogicalId>().commandHandler);
    }

    template <auto LogicalId>
    bool hasStateProvider() const
    {
        return static_cast<bool>(slot<LogicalId>().stateProvider);
    }

    template <auto LogicalId>
    bool hasStateWriteBinding() const
    {
        return static_cast<bool>(slot<LogicalId>().stateWriteHandler);
    }

    template <auto LogicalId>
    bool dispatchCommand(port_value_t<definition_type, LogicalId> value)
    {
        auto& bindingSlot = slot<LogicalId>();
        if (!bindingSlot.commandHandler) {
            return false;
        }

        bindingSlot.commandHandler(value);
        return true;
    }

    template <auto LogicalId>
    bool dispatchStateWrite(port_value_t<definition_type, LogicalId> value)
    {
        auto& bindingSlot = slot<LogicalId>();
        if (!bindingSlot.stateWriteHandler) {
            return false;
        }

        bindingSlot.stateWriteHandler(value);
        return true;
    }

    template <auto LogicalId>
    std::optional<port_value_t<definition_type, LogicalId>> readState() const
    {
        const auto& bindingSlot = slot<LogicalId>();
        if (!bindingSlot.stateProvider) {
            return std::nullopt;
        }

        return bindingSlot.stateProvider();
    }

    void notifyProgrammingModeChanged(bool active) const
    {
        if (_progModeCallback) {
            _progModeCallback(active);
        }
    }

    void notifyFault(FaultInfo info) const
    {
        if (_faultCallback) {
            _faultCallback(info);
        }
    }

private:
    template <auto LogicalId>
    auto& slot()
    {
        using slot_type = PortBindingSlot<port_spec_t<definition_type, LogicalId>, BindingCapacity>;
        return std::get<slot_type>(_slots);
    }

    template <auto LogicalId>
    const auto& slot() const
    {
        using slot_type = PortBindingSlot<port_spec_t<definition_type, LogicalId>, BindingCapacity>;
        return std::get<slot_type>(_slots);
    }

    slot_tuple_type _slots{};
    ProgModeCallback _progModeCallback;
    FaultCallback _faultCallback;
};

} // namespace knx::product