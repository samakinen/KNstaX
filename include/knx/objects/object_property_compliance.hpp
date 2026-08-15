// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file object_property_compliance.hpp
 * @brief Strict validation helpers for interface-object property registrations
 */

#pragma once

#include "knx/objects/interface_object.hpp"
#include "knx/util/result.hpp"

#include <span>

#ifndef KNX_STRICT_OBJECT_COMPLIANCE
#define KNX_STRICT_OBJECT_COMPLIANCE 1
#endif

namespace knx {
namespace objects {

/**
 * @brief Validate a property registration list for a specific object type.
 *
 * In strict mode, known core and reference object types are validated against
 * exact schemas (PID, PDT, access rights, max elements, access levels).
 */
util::Result<void> validateObjectPropertyRegistrations(
    InterfaceObjectType objectType,
    std::span<const PropertyRegistrationInfo> registrations);

} // namespace objects
} // namespace knx
