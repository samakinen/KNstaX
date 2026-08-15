// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property_store.cpp
 * @brief Property storage implementation
 */

#include "knx/application/property_store.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include <algorithm>

static const char* TAG = "KNX.App.PropStore";

namespace knx {
namespace application {

namespace {

/// Interface object type names as the spec and ETS use them.
///
/// Log-only: a bare "type=17" tells a reader nothing, and the numbering is
/// sparse enough that looking it up is a detour into the spec.
const char* objectTypeName(uint16_t type) {
    switch (type) {
        case 0:  return "Device";
        case 1:  return "Address Table";
        case 2:  return "Association Table";
        case 3:  return "Application Program";
        case 4:  return "Interface Program";
        case 5:  return "EIB Object Association Table";
        case 6:  return "Router";
        case 7:  return "LTE Address Routing Table";
        case 8:  return "cEMI Server";
        case 9:  return "Group Object Table";
        case 10: return "Polling Master";
        case 11: return "KNXnet/IP Parameter";
        case 13: return "File Server";
        case 17: return "Security";
        case 19: return "RF Medium";
        default: return "Unknown";
    }
}

} // namespace

// ============================================================================
// PropertyStore Implementation
// ============================================================================

PropertyStore::PropertyStore(InterfaceObjectType objectType, InterfaceObjectIndex objectIndex)
    : _objectType(objectType)
    , _objectIndex(objectIndex)
{
    KNX_LOGD(TAG, "Created property store for object type=%d, index=%d",
             static_cast<uint16_t>(objectType.value()), objectIndex.value());
}

util::Result<void> PropertyStore::registerProperty(
    const PropertyDescriptor& descriptor,
    const PropertyValue& initialValue
) {
    if (_descriptors.find(descriptor.id) != _descriptors.end()) {
        KNX_LOGW(TAG, "Property %d already registered", static_cast<int>(descriptor.id));
        return util::Result<void>::err(util::ErrorCode::AlreadyInitialized);
    }
    
    _descriptors[descriptor.id] = descriptor;
    _properties[descriptor.id] = initialValue;
    
    KNX_LOGD(TAG, "Registered property %d, type=%d, elements=%d",
             static_cast<int>(descriptor.id),
             static_cast<int>(descriptor.type),
             descriptor.maxElements);
    
    return util::Result<void>::ok();
}

util::Result<size_t> PropertyStore::readProperty(
    PropertyID propertyId,
    uint16_t startIndex,
    uint8_t elementCount,
    std::span<uint8_t> out
) const {
    auto it = _properties.find(propertyId);
    if (it == _properties.end()) {
        KNX_LOGW(TAG, "Property %d not found", static_cast<int>(propertyId));
        return util::ErrorCode::InvalidParameter;
    }
    
    auto descIt = _descriptors.find(propertyId);
    if (descIt == _descriptors.end()) {
        return util::ErrorCode::InvalidParameter;
    }
    
    const auto& value = it->second;
    const auto& descriptor = descIt->second;
    
    // Validate index range (1-based indexing in KNX)
    if (startIndex == 0 || startIndex > descriptor.maxElements) {
        KNX_LOGW(TAG, "Invalid start index %d for property %d",
                 startIndex, static_cast<int>(propertyId));
        return util::ErrorCode::InvalidParameter;
    }
    
    // Calculate byte offset and size
    uint8_t elementSize = descriptor.getElementSize();
    if (elementSize == 0) {
        KNX_LOGW(TAG, "Variable-sized property %d is not supported by span read", static_cast<int>(propertyId));
        return util::ErrorCode::InvalidParameter;
    }
    size_t byteOffset = (startIndex - 1) * elementSize;
    size_t byteCount = elementCount * elementSize;
    
    if (byteOffset + byteCount > value.data.size()) {
        KNX_LOGW(TAG, "Read beyond property data: offset=%zu, count=%zu, size=%zu",
                 byteOffset, byteCount, value.data.size());
        return util::ErrorCode::OutOfRange;
    }

    if (out.size() < byteCount) {
        KNX_LOGW(TAG, "Output buffer too small: needed=%zu, got=%zu", byteCount, out.size());
        return util::ErrorCode::BufferTooSmall;
    }

    std::copy_n(value.data.begin() + byteOffset, byteCount, out.begin());
    
    KNX_LOGD(TAG, "Read property %d: index=%d, count=%d, bytes=%zu",
             static_cast<int>(propertyId), startIndex, elementCount, byteCount);

    return byteCount;
}

util::Result<void> PropertyStore::writeProperty(
    PropertyID propertyId,
    uint16_t startIndex,
    uint8_t elementCount,
    std::span<const uint8_t> data
) {
    auto it = _properties.find(propertyId);
    if (it == _properties.end()) {
        KNX_LOGW(TAG, "Property %d not found", static_cast<int>(propertyId));
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    auto descIt = _descriptors.find(propertyId);
    if (descIt == _descriptors.end()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    auto& value = it->second;
    const auto& descriptor = descIt->second;
    
    // Check write access
    if (descriptor.access == PropertyAccess::ReadOnly) {
        KNX_LOGW(TAG, "Property %d is read-only", static_cast<int>(propertyId));
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }
    
    // Validate index range
    if (startIndex == 0 || startIndex > descriptor.maxElements) {
        KNX_LOGW(TAG, "Invalid start index %d for property %d",
                 startIndex, static_cast<int>(propertyId));
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    // Calculate byte offset and validate size
    uint8_t elementSize = descriptor.getElementSize();
    size_t expectedSize = elementCount * elementSize;
    
    if (data.size() != expectedSize) {
        KNX_LOGW(TAG, "Data size mismatch: expected %zu, got %zu",
                 expectedSize, data.size());
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    size_t byteOffset = (startIndex - 1) * elementSize;
    
    // Ensure property data is large enough
    size_t requiredSize = byteOffset + data.size();
    if (value.data.size() < requiredSize) {
        value.data.resize(requiredSize, 0);
    }
    
    // Write data
    std::copy(data.begin(), data.end(), value.data.begin() + byteOffset);
    
    KNX_LOGD(TAG, "Wrote property %d: index=%d, count=%d, bytes=%zu",
             static_cast<int>(propertyId), startIndex, elementCount, data.size());
    
    return util::Result<void>::ok();
}

std::optional<PropertyDescriptor> PropertyStore::getDescriptor(PropertyID propertyId) const {
    auto it = _descriptors.find(propertyId);
    if (it == _descriptors.end()) {
        return std::nullopt;
    }
    return it->second;
}

util::Result<size_t> PropertyStore::getAllPropertyIds(std::span<PropertyID> out) const {
    const size_t required = _properties.size();
    if (out.empty()) {
        return required;
    }

    if (out.size() < required) {
        return util::ErrorCode::BufferTooSmall;
    }

    size_t written = 0;
    for (const auto& pair : _properties) {
        out[written++] = pair.first;
    }
    return written;
}

bool PropertyStore::hasProperty(PropertyID propertyId) const {
    return _properties.find(propertyId) != _properties.end();
}

util::Result<void> PropertyStore::validateAccess(PropertyID propertyId, AccessType accessType, uint8_t accessLevel) const {
    auto it = _descriptors.find(propertyId);
    if (it == _descriptors.end()) {
        return util::ErrorCode::InvalidParameter;
    }
    
    const auto& descriptor = it->second;
    
    if (accessType == AccessType::Write) {
        if (descriptor.access == PropertyAccess::ReadOnly) {
            return util::ErrorCode::OperationNotSupported;
        }
        return (accessLevel >= descriptor.writeLevel)
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    } else {
        return (accessLevel >= descriptor.readLevel)
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }
}


// ============================================================================
// PropertyStoreManager Implementation
// ============================================================================

PropertyStore* PropertyStoreManager::addObject(InterfaceObjectType objectType, InterfaceObjectIndex objectIndex) {
    if (_objects.find(objectIndex) != _objects.end()) {
        KNX_LOGW(TAG, "Object index %d already exists", objectIndex.value());
        return nullptr;
    }
    
    _objects.emplace(objectIndex, PropertyStore(objectType, objectIndex));
    
    KNX_LOGD(TAG, "Interface object %d: %s (object type %d)",
             objectIndex.value(),
             objectTypeName(static_cast<uint16_t>(objectType.value())),
             static_cast<uint16_t>(objectType.value()));
    
    return &_objects.at(objectIndex);
}

PropertyStore* PropertyStoreManager::getObject(InterfaceObjectIndex objectIndex) {
    auto it = _objects.find(objectIndex);
    return (it != _objects.end()) ? &it->second : nullptr;
}

const PropertyStore* PropertyStoreManager::getObject(InterfaceObjectIndex objectIndex) const {
    auto it = _objects.find(objectIndex);
    return (it != _objects.end()) ? &it->second : nullptr;
}

util::Result<size_t> PropertyStoreManager::getAllObjectIndices(std::span<InterfaceObjectIndex> out) const {
    const size_t required = _objects.size();
    if (out.empty()) {
        return required;
    }

    if (out.size() < required) {
        return util::ErrorCode::BufferTooSmall;
    }

    size_t written = 0;
    for (const auto& pair : _objects) {
        out[written++] = pair.first;
    }
    return written;
}

util::Result<size_t> PropertyStoreManager::readProperty(
    InterfaceObjectIndex objectIndex,
    PropertyID propertyId,
    uint16_t startIndex,
    uint8_t elementCount,
    std::span<uint8_t> out
) const {
    auto obj = getObject(objectIndex);
    if (!obj) {
        KNX_LOGW(TAG, "Object %d not found", objectIndex.value());
        return util::ErrorCode::InvalidParameter;
    }
    
    return obj->readProperty(propertyId, startIndex, elementCount, out);
}

util::Result<void> PropertyStoreManager::writeProperty(
    InterfaceObjectIndex objectIndex,
    PropertyID propertyId,
    uint16_t startIndex,
    uint8_t elementCount,
    std::span<const uint8_t> data
) {
    auto obj = getObject(objectIndex);
    if (!obj) {
        KNX_LOGW(TAG, "Object %d not found", objectIndex.value());
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    return obj->writeProperty(propertyId, startIndex, elementCount, data);
}

} // namespace application
} // namespace knx
