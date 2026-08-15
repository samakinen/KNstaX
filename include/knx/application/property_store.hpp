// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property_store.hpp
 * @brief Property storage and management
 * 
 * Provides storage and access to interface object properties.
 * Manages property values, descriptors, and access control.
 */

#pragma once

#include "knx/application/property.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace knx {
namespace application {

/**
 * @brief Property store for a single interface object
 * 
 * Stores properties and provides read/write access with validation.
 */
class PropertyStore {
public:
    /**
     * @brief Constructor
     * @param objectType Interface object type
     * @param objectIndex Object index (0-255)
     */
    PropertyStore(InterfaceObjectType objectType, InterfaceObjectIndex objectIndex);
    
    /**
     * @brief Register a property with descriptor
     * 
     * @param descriptor Property descriptor
     * @param initialValue Initial property value
    * @return Result<void> indicating success or error
     */
    util::Result<void> registerProperty(const PropertyDescriptor& descriptor,
                    const PropertyValue& initialValue);
    
    /**
     * @brief Read property value
     * 
     * @param propertyId Property ID
     * @param startIndex Element start index (1-based)
     * @param elementCount Number of elements to read
     * @param out Caller-managed output buffer
     * @return Number of bytes written, or an error code
     */
    util::Result<size_t> readProperty(
        PropertyID propertyId,
        uint16_t startIndex,
        uint8_t elementCount,
        std::span<uint8_t> out
    ) const;
    
    /**
     * @brief Write property value
     * 
     * @param propertyId Property ID
     * @param startIndex Element start index (1-based)
     * @param elementCount Number of elements to write
     * @param data Property data
     * @return Result<void> indicating success or error
     */
    util::Result<void> writeProperty(
        PropertyID propertyId,
        uint16_t startIndex,
        uint8_t elementCount,
        std::span<const uint8_t> data
    );
    
    /**
     * @brief Get property descriptor
     * 
     * @param propertyId Property ID
     * @return Property descriptor, or nullopt if not found
     */
    std::optional<PropertyDescriptor> getDescriptor(PropertyID propertyId) const;
    
    /**
     * @brief Get all property IDs in this object
     * 
     * Query-then-fill pattern: call with an empty span to get required count,
     * then provide a large enough span to fill. Returns the required count on
     * success, or BufferTooSmall if the provided span is undersized.
     */
    util::Result<size_t> getAllPropertyIds(std::span<PropertyID> out) const;
    
    /**
     * @brief Get property count
     * 
     * @return Number of properties in this object
     */
    size_t getPropertyCount() const { return _properties.size(); }
    
    /**
     * @brief Get object type
     * 
     * @return Interface object type
     */
    InterfaceObjectType getObjectType() const { return _objectType; }
    
    /**
     * @brief Get object index
     * 
     * @return Object index
     */
    InterfaceObjectIndex getObjectIndex() const { return _objectIndex; }
    
    /**
     * @brief Check if property exists
     * 
     * @param propertyId Property ID
     * @return true if property exists
     */
    bool hasProperty(PropertyID propertyId) const;
    
    /**
     * @brief Validate property access
     * 
     * @param propertyId Property ID
    * @param accessType Requested access type
     * @param accessLevel User access level (0-15)
    * @return Result<void> indicating success or error
     */
    util::Result<void> validateAccess(PropertyID propertyId, AccessType accessType, uint8_t accessLevel = 0) const;


private:
    InterfaceObjectType _objectType;
    InterfaceObjectIndex _objectIndex;
    
    /// Property descriptors (metadata)
    std::map<PropertyID, PropertyDescriptor> _descriptors;
    
    /// Property values (actual data)
    std::map<PropertyID, PropertyValue> _properties;
};

/**
 * @brief Property store manager
 * 
 * Manages multiple interface objects and their property stores.
 */
class PropertyStoreManager {
public:
    /**
     * @brief Add an interface object
     * 
     * @param objectType Object type
     * @param objectIndex Object index
     * @return Pointer to property store, or nullptr on error
     */
    PropertyStore* addObject(InterfaceObjectType objectType, InterfaceObjectIndex objectIndex);
    
    /**
     * @brief Get property store for object
     * 
     * @param objectIndex Object index
     * @return Pointer to property store, or nullptr if not found
     */
    PropertyStore* getObject(InterfaceObjectIndex objectIndex);
    const PropertyStore* getObject(InterfaceObjectIndex objectIndex) const;
    
    /**
     * @brief Get all object indices
     *
     * Query-then-fill pattern: call with an empty span to get required count,
     * then provide a large enough span to fill. Returns the required count on
     * success, or BufferTooSmall if the provided span is undersized.
     */
    util::Result<size_t> getAllObjectIndices(std::span<InterfaceObjectIndex> out) const;
    
    /**
     * @brief Get object count
     * 
     * @return Number of objects
     */
    size_t getObjectCount() const { return _objects.size(); }
    
    /**
     * @brief Read property from any object
     * 
     * @param objectIndex Object index
     * @param propertyId Property ID
     * @param startIndex Element start index
     * @param elementCount Element count
     * @param out Caller-managed output buffer
     * @return Number of bytes written, or an error code
     */
    util::Result<size_t> readProperty(
        InterfaceObjectIndex objectIndex,
        PropertyID propertyId,
        uint16_t startIndex,
        uint8_t elementCount,
        std::span<uint8_t> out
    ) const;
    
    /**
     * @brief Write property to any object
     * 
     * @param objectIndex Object index
     * @param propertyId Property ID
     * @param startIndex Element start index
     * @param elementCount Element count
     * @param data Property data
     * @return Result<void> indicating success or error
     */
    util::Result<void> writeProperty(
        InterfaceObjectIndex objectIndex,
        PropertyID propertyId,
        uint16_t startIndex,
        uint8_t elementCount,
        std::span<const uint8_t> data
    );

private:
    std::map<InterfaceObjectIndex, PropertyStore> _objects;
};

} // namespace application
} // namespace knx
