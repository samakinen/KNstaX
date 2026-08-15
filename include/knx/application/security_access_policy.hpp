// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file security_access_policy.hpp
 * @brief KNX Access Policies for management services (03/4/1 §6.2).
 *
 * KNX Data Secure is not only a transport wrapper: a device that can be
 * secured has to *refuse* the management services that would undo that
 * security when they arrive unprotected.  Without this, the whole feature is
 * decorative — anybody on the bus can switch Data Secure off with a single
 * plain telegram and then talk to the device in the clear.
 *
 * The Access Policies are specified per communication partner (Role) and per
 * protection (none / A / A+C).  This stack does not implement configurable
 * Roles, so the policy is evaluated against the two facts the S-AL reports:
 * whether the request was secured at all, and whether it used the Tool Key
 * (which is what grants the Role "Tool").  Everything else is "Unlisted".
 */

#pragma once

#include "knx/application/apci_services.hpp"
#include "knx/types.hpp"

#include <cstdint>

namespace knx {
namespace application {

/// Interface Object Type of the Security Interface Object (03/05/01 §6.3).
inline constexpr uint16_t kSecurityInterfaceObjectType = 17u;

/**
 * @brief May this request touch a property of the Security Interface Object?
 *
 * 03/05/01 §6.3 gives the key tables, the Tool Key and the Security Individual
 * Address Table an Access Policy of 00C/00C and adds, for each of them, that
 * "these requirements are exclusive: other Roles, security features or
 * services shall not have access to this Property".  §1.3 states the intent
 * plainly: a device must protect keys "from any access".
 *
 * PID_SECURITY_MODE (§6.3.5) is the one property with an explicit extra
 * sentence, and it is the reason this function exists: "This Property shall
 * only be writeable using Secure Communication, regardless of its value. […]
 * Even if Security Mode is disabled, it shall only be possible to enable it by
 * using secure communication."  Hence the policy does not consult the current
 * security mode at all — a disabled device is exactly the case the sentence
 * calls out.
 *
 * @param propertyId PID within the Security Interface Object.
 * @param write      true for a write/command, false for a read/state read.
 */
[[nodiscard]] bool securityObjectAccessPermitted(uint16_t propertyId,
                                                 bool write,
                                                 const RequestSecurity& security) noexcept;

/**
 * @brief Does this service modify the device's configuration?
 *
 * The "W" side of the Access Policy tables (03/4/1 §6.2.2, Table 4) — the
 * services whose indication a Permission has to authorise before it is
 * executed.  Reads are deliberately not listed: their policies are permissive
 * (3FF) except for the Security Interface Object, which
 * securityObjectAccessPermitted() covers separately.
 */
[[nodiscard]] bool isManagementWriteService(APCIService service) noexcept;

/**
 * @brief May this management write run, given how it was secured?
 *
 * With Security Mode enabled, unsecured management access is not foreseen: the
 * write policies of the services that reconfigure a device are 00C or 0CC,
 * which admit no unprotected client.  With Security Mode disabled the device is
 * an ordinary plain device and the same services are open — that is how it gets
 * commissioned in the first place.
 *
 * @param securityModeEnabled PID_SECURITY_MODE of the Security Interface Object.
 */
[[nodiscard]] bool managementWritePermitted(APCIService service,
                                            bool securityModeEnabled,
                                            const RequestSecurity& security) noexcept;

} // namespace application
} // namespace knx
