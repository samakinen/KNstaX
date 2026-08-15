// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_bindings.hpp
 * @brief Canonical public endpoint business-binding surface.
 */

#pragma once

#include "knx/product/impl/bindings/endpoint_bindings.hpp"

namespace knx::product::endpoint {

template <typename PortSpec, size_t BindingCapacity = knx::product::kDefaultBindingCapacity>
using PortBindingSlot = knx::product::PortBindingSlot<PortSpec, BindingCapacity>;

using ProgModeCallback = knx::product::ProgModeCallback;
using FaultInfo = knx::product::FaultInfo;
using FaultCallback = knx::product::FaultCallback;

template <typename DefinitionT, size_t BindingCapacity = knx::product::kDefaultBindingCapacity>
using EndpointBindings = knx::product::EndpointBindings<DefinitionT, BindingCapacity>;

} // namespace knx::product::endpoint