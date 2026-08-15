// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_export.hpp
 * @brief Canonical public endpoint export-bridge surface.
 */

#pragma once

#include "knx/product/impl/export/endpoint_export.hpp"

namespace knx::product::endpoint {

template <typename DefinitionT>
using CompiledEndpointDefinition = knx::product::CompiledEndpointDefinition<DefinitionT>;

using knx::product::compileEndpointDefinition;
using knx::product::makeExportDescriptor;
using knx::product::makeEndpointExportDescriptor;
using knx::product::exportEndpointToJson;
using knx::product::exportEndpointToKaenxXml;

} // namespace knx::product::endpoint