// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen
/**
 * @file commissioned_product_export.hpp
 * @brief A commissioned product as the JSON the knxprod exporter reads (host tools only)
 *
 * One call for both parameter models:
 *
 *   const std::string json = knx::product::exportCommissionedProductToJson(kProduct);
 *
 * A product built with makeCommissionedProduct<kParameters>() is written with
 * its parameter blocks (parameter_layout_export.hpp); a product with a
 * parameter schema with the flat "parameters" list.
 */

#pragma once

#include "knx/product/commissioned_product.hpp"
#include "knx/product/exporter.hpp"
#include "knx/product/parameter_layout_export.hpp"

#include <string>

namespace knx::product {

template <typename EndpointDefinitionT, typename ParameterSchemaT>
std::string exportCommissionedProductToJson(
    const CommissionedProductDefinition<EndpointDefinitionT, ParameterSchemaT>& definition)
{
    const auto descriptor = makeCommissionedExportDescriptor(definition);
    if constexpr (detail::kIsLayoutParameters<std::remove_cvref_t<ParameterSchemaT>>) {
        return exportDescriptorToJson(descriptor,
                                      parameterLayoutToJson(std::remove_cvref_t<ParameterSchemaT>::layout));
    } else {
        return exportDescriptorToJson(descriptor);
    }
}

} // namespace knx::product
