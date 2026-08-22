// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file secured_service_families.hpp
 * @brief PID_SECURED_SERVICE_FAMILIES wire format (03/08/09 §2.3.1.5)
 *
 * ETS uses this Function Property of the KNXnet/IP Parameter Object to say
 * which KNXnet/IP service families must be encrypted. The decoding lives here
 * rather than inside the BAU handler because it is where all the judgement is:
 * every branch below is a Return Code the standard names, and a device that
 * gets one wrong tells a management client something untrue about how the
 * installation is secured.
 */

#pragma once

#include "knx/application/function_property_services.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

/// §2.3.1.5.4.1: the only families that may be secured. Core discovery is
/// deliberately absent — a client has to be able to find a device and read its
/// security requirements before it can meet them.
inline constexpr uint8_t kServiceFamilyDeviceManagement = 0x03u;
inline constexpr uint8_t kServiceFamilyTunnelling = 0x04u;
inline constexpr uint8_t kServiceFamilyRouting = 0x05u;
inline constexpr std::array<uint8_t, 3> kSecurableServiceFamilies{
    kServiceFamilyDeviceManagement, kServiceFamilyTunnelling, kServiceFamilyRouting};

/// The only Read-/WriteServiceID the property defines (Tables 2 and 3).
inline constexpr uint8_t kSecuredServiceFamiliesServiceId = 0x00u;

/// Security Version 00h: plain communication allowed. 01h: KNX IP Secure
/// required. Anything above is "Reserved. For future use."
inline constexpr uint8_t kSecurityVersionPlain = 0x00u;
inline constexpr uint8_t kSecurityVersionRequired = 0x01u;

[[nodiscard]] inline bool isSecurableServiceFamily(uint8_t serviceFamilyId)
{
    return std::find(kSecurableServiceFamilies.begin(),
                     kSecurableServiceFamilies.end(),
                     serviceFamilyId) != kSecurableServiceFamilies.end();
}

/// Index of a securable family, for storing one value per family.
[[nodiscard]] inline size_t securableServiceFamilyIndex(uint8_t serviceFamilyId)
{
    return static_cast<size_t>(std::find(kSecurableServiceFamilies.begin(),
                                         kSecurableServiceFamilies.end(),
                                         serviceFamilyId)
                               - kSecurableServiceFamilies.begin());
}

/**
 * @brief A decoded request, or the Return Code to refuse it with.
 *
 * Every answer repeats the ServiceID (§2.3.1.5.6 Figure 28), including the
 * refusals, so a client with several commands outstanding can tell which one
 * was refused. A request too short to carry one has nothing to repeat, which is
 * what `hasServiceId` distinguishes.
 */
struct SecuredServiceFamiliesRequest {
    application::FunctionPropertyReturnCode returnCode{
        application::FunctionPropertyReturnCode::Success};
    bool hasServiceId{false};
    uint8_t serviceId{0};
    uint8_t serviceFamilyId{0};
    /// Only meaningful for a command; a state read carries no version.
    uint8_t securityVersion{0};

    [[nodiscard]] bool ok() const
    {
        return returnCode == application::FunctionPropertyReturnCode::Success;
    }
};

/**
 * @brief Decode a command (write) or state-read payload.
 *
 * Write: Reserved | WriteServiceID | ServiceFamilyID | SecurityVersion.
 * Read:  Reserved | ReadServiceID  | ServiceFamilyID.
 *
 * @param isCommand true for A_FunctionPropertyCommand, false for a state read.
 */
[[nodiscard]] inline SecuredServiceFamiliesRequest decodeSecuredServiceFamiliesRequest(
    std::span<const uint8_t> data, bool isCommand)
{
    using RC = application::FunctionPropertyReturnCode;
    SecuredServiceFamiliesRequest out;

    if (data.size() < 2u) {
        // Nothing to echo, so Table 4's catch-all rather than a code that would
        // claim to name the problem.
        out.returnCode = RC::Error;
        return out;
    }

    out.hasServiceId = true;
    out.serviceId = data[1];

    // Table 4, in the order the standard states the rules.
    if (data[0] != 0x00u) {
        out.returnCode = RC::DataVoid;  // "octet 10 in the request does not equal 00h"
        return out;
    }
    if (out.serviceId != kSecuredServiceFamiliesServiceId) {
        out.returnCode = RC::CommandInvalid;  // not a defined Read-/WriteServiceID
        return out;
    }

    const size_t expected = isCommand ? 4u : 3u;
    if (data.size() < expected) {
        out.returnCode = RC::DataVoid;
        return out;
    }

    out.serviceFamilyId = data[2];
    if (!isSecurableServiceFamily(out.serviceFamilyId)) {
        out.returnCode = RC::DataVoid;  // "not supported/allowed Service Family ID"
        return out;
    }

    if (isCommand) {
        out.securityVersion = data[3];
        if (out.securityVersion > kSecurityVersionRequired) {
            out.returnCode = RC::DataVoid;  // "> 01h: Reserved. For future use."
            return out;
        }
    }

    return out;
}

} // namespace netip
} // namespace knx
