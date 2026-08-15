// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file interface_object.cpp
 * @brief InterfaceObject base class implementation
 */

#include "knx/objects/interface_object.hpp"
#include "knx/objects/object_property_compliance.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/application/property_store.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/log.hpp"

#include <span>

static const char* TAG = "KNX.InterfaceObject";

namespace knx {
namespace objects {

size_t InterfaceObject::getPropertyRegistrations(std::span<PropertyRegistrationInfo> out) const
{
    return copyPropertyRegistrations(coreObjectPropertyManifestEntries(objectType()), out);
}

void InterfaceObject::registerProperties(application::PropertyStore& store) const {
    const size_t needed = getPropertyRegistrations(std::span<PropertyRegistrationInfo>{});
    if (needed > kMaxPropertyRegistrations) {
        KNX_LOGE(TAG,
                 "Property registration count %u exceeds fixed bound %u for object type %u",
                 static_cast<unsigned>(needed),
                 static_cast<unsigned>(kMaxPropertyRegistrations),
                 static_cast<unsigned>(objectType().value()));
        return;
    }

    util::FixedVector<PropertyRegistrationInfo, kMaxPropertyRegistrations> registrations;
    registrations.resize(needed);
    const size_t got = getPropertyRegistrations(registrations.span());
    registrations.resize(got);
    const auto validation = validateObjectPropertyRegistrations(objectType(), registrations.span());
    if (validation.isError()) {
        KNX_LOGE(TAG,
                 "Property registration validation failed for object type %u (err=%d)",
                 static_cast<unsigned>(objectType().value()),
                 static_cast<int>(validation.error()));
        return;
    }
    
    for (const auto& reg : registrations.span()) {
        application::PropertyDescriptor desc{
            reg.propertyId,
            reg.dataType,
            reg.access,
            reg.maxElements,
            reg.readLevel,
            reg.writeLevel
        };
        
        // Create empty initial value; live reads are served via providers
        application::PropertyValue value = application::PropertyValue::create(
            reg.propertyId, reg.dataType, std::span<const uint8_t>{});
        
        (void)store.registerProperty(desc, value);
    }
}

} // namespace objects
} // namespace knx
