// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file reference_object_registry.hpp
 * @brief Property registration lists for reference interface objects
 */

#pragma once

#include "knx/objects/object_property_manifest.hpp"

#include <span>

namespace knx {
namespace objects {

bool isReferenceObjectType(InterfaceObjectType type);
std::span<const PropertyManifestEntry> referenceObjectPropertyManifestEntries(InterfaceObjectType type);

} // namespace objects
} // namespace knx
