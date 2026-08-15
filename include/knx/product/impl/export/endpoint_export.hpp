// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_export.hpp
 * @brief Export bridge: serialises an endpoint definition to JSON and XML product-catalog formats.
 */

#pragma once

#include "knx/product/exporter.hpp"
#include "knx/product/impl/compiler/endpoint_compiler.hpp"

#include <string>

namespace knx::product {

template <typename DefinitionT>
constexpr auto makeEndpointExportDescriptor(const DefinitionT& definition)
{
    return makeExportDescriptor(definition);
}

template <typename DefinitionT>
constexpr auto makeEndpointExportDescriptor(const CompiledEndpointDefinition<DefinitionT>& compiled)
{
    return makeExportDescriptor(compiled);
}

template <typename DefinitionT>
std::string exportEndpointToJson(const DefinitionT& definition)
{
    return exportDescriptorToJson(makeEndpointExportDescriptor(definition));
}

template <typename DefinitionT>
std::string exportEndpointToJson(const CompiledEndpointDefinition<DefinitionT>& compiled)
{
    return exportDescriptorToJson(makeEndpointExportDescriptor(compiled));
}

template <typename DefinitionT>
std::string exportEndpointToKaenxXml(const DefinitionT& definition)
{
    return exportDescriptorToKaenxXml(makeEndpointExportDescriptor(definition));
}

template <typename DefinitionT>
std::string exportEndpointToKaenxXml(const CompiledEndpointDefinition<DefinitionT>& compiled)
{
    return exportDescriptorToKaenxXml(makeEndpointExportDescriptor(compiled));
}

} // namespace knx::product