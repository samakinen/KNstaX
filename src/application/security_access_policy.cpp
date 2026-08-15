// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file security_access_policy.cpp
 * @brief KNX Access Policies for management services (03/4/1 §6.2).
 */

#include "knx/application/security_access_policy.hpp"

namespace knx {
namespace application {

namespace {

/// PIDs of the Security Interface Object (03/05/01 Table 99).
enum : uint16_t {
    kPidObjectType = 1,
    kPidObjectName = 2,
    kPidLoadStateControl = 5,
    kPidSecurityMode = 51,
    kPidP2PKeyTable = 52,
    kPidGroupKeyTable = 53,
    kPidSecurityIndividualAddressTable = 54,
    kPidSecurityFailuresLog = 55,
    kPidToolKey = 56,
    kPidSecurityReport = 57,
    kPidSecurityReportControl = 58,
    kPidSequenceNumberSending = 59,
    kPidZoneKeyTable = 60,
    kPidGoSecurityFlags = 61,
};

}  // namespace

bool securityObjectAccessPermitted(uint16_t propertyId,
                                   bool write,
                                   const RequestSecurity& security) noexcept
{
    // Role "Tool" with authentication and confidentiality: the single client
    // the 00C/00C and 04C policies of this object admit.
    if (security.toolSecured()) {
        return true;
    }

    // No unprotected write reaches any property of this object — not the key
    // tables, not the Tool Key, and above all not PID_SECURITY_MODE, whose
    // §6.3.5 wording holds "regardless of its value".
    if (write) {
        return false;
    }

    switch (propertyId) {
        // 3FF/0CC: the object's identity is readable by anyone, and ETS reads
        // it while enumerating interface objects, before any secure link
        // exists.
        case kPidObjectType:
        case kPidObjectName:
        // Reading the load state and the security mode leaks no key material,
        // and ETS reads both to decide whether the device is already secure —
        // in plain, because it has nothing to secure with yet. Only their
        // write/command direction is restricted, which the branch above does.
        case kPidLoadStateControl:
        case kPidSecurityMode:
        // The failure counters and the report bits exist to be observed; they
        // are diagnostics, not credentials.
        case kPidSecurityFailuresLog:
        case kPidSecurityReport:
        case kPidSecurityReportControl:
            return true;

        // Everything else is key material or the state that protects it:
        // PID_P2P_KEY_TABLE, PID_GRP_KEY_TABLE, PID_ZONE_KEY_TABLE and
        // PID_TOOL_KEY hold keys outright; the Security Individual Address
        // Table and PID_SEQUENCE_NUMBER_SENDING hold the sequence numbers that
        // make replay detectable; PID_GO_SECURITY_FLAGS says which group
        // objects are protected at all.
        case kPidP2PKeyTable:
        case kPidGroupKeyTable:
        case kPidSecurityIndividualAddressTable:
        case kPidToolKey:
        case kPidSequenceNumberSending:
        case kPidZoneKeyTable:
        case kPidGoSecurityFlags:
        default:
            // Default-deny: a property added to this object later is treated as
            // sensitive until someone decides otherwise.
            return false;
    }
}

bool isManagementWriteService(APCIService service) noexcept
{
    switch (service) {
        case APCIService::PropertyValueWrite:
        case APCIService::PropertyExtValueWriteCon:
        case APCIService::PropertyExtValueWriteUnCon:
        case APCIService::FunctionPropertyCommand:
        case APCIService::FunctionPropertyExtCommand:
        case APCIService::MemoryWrite:
        case APCIService::MemoryExtendedWrite:
        case APCIService::IndividualAddressWrite:
        case APCIService::IndividualAddressSerialNumberWrite:
        case APCIService::NetworkParameterWrite:
        case APCIService::SystemNetworkParameterWrite:
        case APCIService::AuthorizeRequest:
        case APCIService::KeyWrite:
        case APCIService::Restart:
            return true;
        default:
            return false;
    }
}

bool managementWritePermitted(APCIService service,
                              bool securityModeEnabled,
                              const RequestSecurity& security) noexcept
{
    if (!isManagementWriteService(service)) {
        return true;
    }
    if (!securityModeEnabled) {
        // Plain device, plain rules: this is the "Security Mode: Off" half of
        // every policy in Table 3, where the Unlisted client still has write
        // access. Without it a secure-capable device could never be
        // commissioned the first time.
        return true;
    }
    // "Full management access to the device is possible even in device security
    // mode if KNX data security is used together with the tool key" — the
    // converse is the point: without it, there is no management access.
    return security.toolSecured();
}

} // namespace application
} // namespace knx
