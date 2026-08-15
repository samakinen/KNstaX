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
 * and applyCommissionedParameter. They are aliases that exist to make the
 * "this is a test-only call" intent visible at the call site.
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

template <auto ParameterId, typename ProductDefinitionT, size_t BindingCapacity>
auto applyDemoParameterValue(
    CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>& runtime,
    typename ProductDefinitionT::template ParameterValueType<ParameterId> value) -> util::Result<void>
{
    return runtime.template applyCommissionedParameter<ParameterId>(std::move(value));
}

/// Apply raw ETS ProgramData bytes to a commissioned product's parameter state.
///
/// Use in unit/integration tests to simulate what ETS does when it downloads
/// device parameters over TP1 or KNXnet/IP.  The byte layout is sequential
/// big-endian: bool→1 byte, uint8_t→1 byte, uint16_t/int16_t/Enum→2 bytes,
/// float→4 bytes.  Parameters whose kind is None or Text consume no bytes and
/// are skipped.
template <typename ProductDefinitionT, size_t BindingCapacity>
void applyParameterDataBytes(CommissionedProductRuntime<ProductDefinitionT, BindingCapacity>& runtime,
                             std::span<const uint8_t> data)
{
    runtime.applyParameterDataBytesForExpertUse(data);
}

} // namespace knx::product::expert