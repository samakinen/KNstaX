// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_runtime.hpp
 * @brief Canonical public endpoint runtime surface.
 */

#pragma once

#include "knx/product/impl/runtime/endpoint_runtime.hpp"

namespace knx::product::endpoint {

using EndpointInstanceConfig = knx::product::EndpointInstanceConfig;
using PendingBusActionKind = knx::product::PendingBusActionKind;

template <typename PortIdEnum>
using PendingBusAction = knx::product::PendingBusAction<PortIdEnum>;

template <typename DefinitionT, size_t BindingCapacity = knx::product::kDefaultBindingCapacity>
using EndpointRuntime = knx::product::EndpointRuntime<DefinitionT, BindingCapacity>;

} // namespace knx::product::endpoint