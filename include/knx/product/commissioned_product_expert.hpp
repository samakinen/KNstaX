// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file commissioned_product_expert.hpp
 * @brief Explicit test and demo helpers for commissioned products.
 *
 * Include this header only in smoke tests, offline demo harnesses, and
 * commissioning import tools. Normal ETS-configured firmware should not
 * include it: both group addresses and parameters are programmed by ETS through
 * the KNX management protocol and are applied automatically by
 * CommissionedProductRuntime.
 *
 * These helpers wrap CommissionedProductRuntime::applyCommissionedGroupAddresses
 * and the loading of a whole parameter image. They exist to make the "this is
 * a test-only call" intent visible at the call site.
 */

#pragma once

#include "knx/product/commissioned_product.hpp"

#include <span>
#include <utility>

namespace knx::product::expert {

using endpoint::GroupAddressBinding;
using endpoint::groupAddressBinding;

template <typename ProductDefinitionT, size_t BindingCapacity, typename... Bindings>
auto bindDemoGroupAddresses(CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>& runtime,
                            Bindings&&... bindings) -> util::Result<void>
{
    return runtime.applyCommissionedGroupAddresses(std::forward<Bindings>(bindings)...);
}

/// Replace a product's whole parameter image the way a completed ETS download
/// does: the image is checked against its bounds, stored, and
/// onParametersChanged fires. False when the size is wrong or a value is
/// outside its bounds.
///
/// For tests and demo set-ups without ETS. To change one parameter, use
/// CommissionedProductRuntime::setParameter().
template <typename ProductDefinitionT, size_t BindingCapacity>
bool loadParameterImage(CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>& runtime,
                        std::span<const uint8_t> image)
{
    return runtime.loadParameterImageForExpertUse(image);
}

} // namespace knx::product::expert