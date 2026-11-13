// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file reference_object_registry.hpp
 * @brief Property declarations of the reference interface objects
 */

#pragma once

#include "knx/application/property.hpp"
#include "knx/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace objects {

/**
 * @brief One property of a reference interface object
 *
 * A GenericInterfaceObject builds its property handlers from these, so for a
 * reference object this list is the only place a property's type, access and
 * size are stated. The core objects state theirs in their handler tables.
 */
struct PropertyDeclaration {
    application::PropertyID propertyId;
    application::PropertyDataType dataType;
    application::PropertyAccess access;
    uint16_t maxElements;
};

/// How many interface object types isReferenceObjectType() accepts. Sizes the
/// InterfaceObjectManager's object list; a static_assert beside the list of
/// types keeps the two in step.
inline constexpr size_t kReferenceObjectTypeCount = 13;

bool isReferenceObjectType(InterfaceObjectType type);
std::span<const PropertyDeclaration> referenceObjectProperties(InterfaceObjectType type);

} // namespace objects
} // namespace knx
