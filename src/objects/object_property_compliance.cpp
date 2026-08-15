// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file object_property_compliance.cpp
 * @brief Strict validation helpers for interface-object property registrations
 */

#include "knx/objects/object_property_compliance.hpp"
#include "knx/objects/object_property_manifest.hpp"

#include <map>
#include <set>

namespace knx {
namespace objects {

namespace {
using PidRaw = uint8_t;

PidRaw pidKey(application::PropertyID pid) {
    return static_cast<PidRaw>(pid);
}

bool matchesRegistration(const PropertyRegistrationInfo& actual, const PropertyRegistrationInfo& expected) {
    return actual.dataType == expected.dataType &&
           actual.access == expected.access &&
           actual.maxElements == expected.maxElements &&
           actual.readLevel == expected.readLevel &&
           actual.writeLevel == expected.writeLevel;
}

util::Result<void> validateStructural(std::span<const PropertyRegistrationInfo> registrations) {
    if (registrations.empty()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    std::set<PidRaw> seen;
    for (const auto& reg : registrations) {
        if (reg.propertyId == static_cast<application::PropertyID>(0)) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        if (reg.maxElements == 0) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        if (!seen.insert(pidKey(reg.propertyId)).second) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
    }

    if (seen.find(pidKey(application::PropertyID::ObjectType)) == seen.end()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    return util::Result<void>::ok();
}

} // namespace

util::Result<void> validateObjectPropertyRegistrations(
    InterfaceObjectType objectType,
    std::span<const PropertyRegistrationInfo> registrations)
{
    const auto structural = validateStructural(registrations);
    if (structural.isError()) {
        return structural;
    }

#if KNX_STRICT_OBJECT_COMPLIANCE
    const auto expectedEntries = objectPropertyManifestEntries(objectType);
    if (!expectedEntries.empty()) {

        std::map<PidRaw, PropertyRegistrationInfo> expectedByPid;
        for (const auto& entry : expectedEntries) {
            expectedByPid.emplace(pidKey(entry.registration.propertyId), entry.registration);
        }

        std::set<PidRaw> provided;
        for (const auto& reg : registrations) {
            const auto id = pidKey(reg.propertyId);
            const auto it = expectedByPid.find(id);
            if (it == expectedByPid.end()) {
                return util::Result<void>::err(util::ErrorCode::InvalidParameter);
            }

            const auto& exp = it->second;
            if (!matchesRegistration(reg, exp)) {
                return util::Result<void>::err(util::ErrorCode::InvalidParameter);
            }

            provided.insert(id);
        }

        for (const auto& exp : expectedByPid) {
            if (provided.find(exp.first) == provided.end()) {
                return util::Result<void>::err(util::ErrorCode::InvalidParameter);
            }
        }
    }
#else
    (void)objectType;
#endif

    return util::Result<void>::ok();
}

} // namespace objects
} // namespace knx
