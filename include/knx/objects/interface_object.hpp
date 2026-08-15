// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file interface_object.hpp
 * @brief Base class for KNX Interface Objects
 * 
 * Provides common interface for all KNX Interface Objects.
 * Each object knows its type and can register its own properties.
 */

#pragma once

#include "knx/types.hpp"
#include "knx/application/property.hpp"
#include "knx/objects/property_kernel.hpp"
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace application {
class PropertyStore;  // Forward declaration
}

namespace objects {

/**
 * @brief Property registration info for an interface object
 * 
 * Contains all metadata needed to register a property with the PropertyStore.
 * This allows each object to be the single source of truth for its properties.
 */
struct PropertyRegistrationInfo {
    application::PropertyID propertyId;
    application::PropertyDataType dataType;
    application::PropertyAccess access;
    uint16_t maxElements;
    uint8_t readLevel;
    uint8_t writeLevel;
};

/**
 * @brief Base class for all KNX Interface Objects
 * 
 * Each interface object:
 * - Has a standardized object type (per KNX spec 03_07_03)
 * - Declares its own properties (per relevant object specification)
 * - Can register properties with a PropertyStore
 * 
 * This design follows the KNX specification where each object type
 * defines its own set of properties with specific semantics.
 */
class InterfaceObject {
public:
    static constexpr size_t kMaxPropertyRegistrations = 64u;

    virtual ~InterfaceObject() = default;

    /**
     * @brief Get the interface object type
     * 
     * Returns the standardized KNX object type identifier.
     * Values 0-99 are system interface objects (Table 1, 03_07_03).
     * Values 100-50000 are application interface objects.
    * Values 50001-65535 are manufacturer-specific.
     * 
     * @return InterfaceObjectType per KNX specification
     */
    virtual InterfaceObjectType objectType() const = 0;

    /**
     * @brief Get property registration info for this object
     * 
     * Returns a list of all properties this object supports.
     * Each object is the single source of truth for its own properties.
     * 
     * @return Vector of PropertyRegistrationInfo
     */
    // Non-allocating: if `out` is empty, implementation should return the required
    // number of registrations. Otherwise, it should write up to `out.size()`
    // entries and return the number written.
    // Default: delegates to coreObjectPropertyManifestEntries(objectType()).
    // Override only for objects with custom (non-manifest) property schemas.
    virtual size_t getPropertyRegistrations(std::span<PropertyRegistrationInfo> out) const;

    /**
     * @brief Property Kernel binding for this object
     *
     * All interface objects must expose their property handlers via
     * Property Kernel binding.
     */
    virtual KernelBinding kernelBinding() const = 0;

    /**
     * @brief Register this object's properties with a PropertyStore
     * 
     * Uses getPropertyRegistrations() to populate the store.
     * 
     * @param store PropertyStore to register with
     */
    void registerProperties(application::PropertyStore& store) const;
};

} // namespace objects
} // namespace knx
