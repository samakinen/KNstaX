// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen
/**
 * @file commissioned_product_export.hpp
 * @brief A commissioned product as the JSON the knxprod exporter reads (host tools only)
 *
 *   const std::string json = knx::product::exportCommissionedProductToJson(kProduct);
 *
 * The parameters of a product built with makeCommissionedProduct<kParameters>()
 * are written as blocks (parameter_layout_export.hpp).
 */

#pragma once

#include "knx/product/commissioned_product.hpp"
#include "knx/product/exporter.hpp"
#include "knx/product/parameter_layout_export.hpp"

#include <string>

namespace knx::product {

template <typename EndpointDefinitionT, typename ParametersT>
std::string exportCommissionedProductToJson(
    const CommissionedProductDefinition<EndpointDefinitionT, ParametersT>& definition)
{
    const auto descriptor = makeCommissionedExportDescriptor(definition);
    if constexpr (detail::kHasParameters<std::remove_cvref_t<ParametersT>>) {
        return exportDescriptorToJson(descriptor, parameterLayoutToJson(std::remove_cvref_t<ParametersT>::layout));
    } else {
        return exportDescriptorToJson(descriptor);
    }
}

} // namespace knx::product
