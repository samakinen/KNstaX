// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/objects/interface_object.hpp"

#include <span>

namespace knx {
namespace objects {

enum class PropertySupportLevel : uint8_t {
    Required,    ///< Declared in spec; must be implemented and accessible.
    Optional,    ///< Declared in spec; implementation optional; may return InvalidProperty.
    Unsupported, ///< Explicitly not supported; returns InvalidProperty on any access.
    Deferred,    ///< Planned for future implementation; currently returns InvalidProperty.
};

enum class PropertyPersistenceMode : uint8_t {
    None = 0,
    Persist,
};

struct PropertyManifestEntry {
    PropertyRegistrationInfo registration{};
    PropertyPersistenceMode persistence{PropertyPersistenceMode::None};
    PropertySupportLevel support{PropertySupportLevel::Required};
};

bool isCoreObjectType(InterfaceObjectType type);
std::span<const PropertyManifestEntry> coreObjectPropertyManifestEntries(InterfaceObjectType type);
std::span<const PropertyManifestEntry> objectPropertyManifestEntries(InterfaceObjectType type);

inline bool isPersistedProperty(const PropertyManifestEntry& entry) noexcept
{
    return entry.persistence == PropertyPersistenceMode::Persist;
}

inline bool isSupportedProperty(const PropertyManifestEntry& entry) noexcept
{
    return entry.support == PropertySupportLevel::Required ||
           entry.support == PropertySupportLevel::Optional;
}

inline bool isRequiredProperty(const PropertyManifestEntry& entry) noexcept
{
    return entry.support == PropertySupportLevel::Required;
}

inline size_t copyPropertyRegistrations(std::span<const PropertyManifestEntry> entries,
                                        std::span<PropertyRegistrationInfo> out)
{
    const size_t count = entries.size();
    if (out.empty()) return count;

    const size_t toCopy = count < out.size() ? count : out.size();
    for (size_t index = 0; index < toCopy; ++index) {
        out[index] = entries[index].registration;
    }
    return toCopy;
}

} // namespace objects
} // namespace knx
