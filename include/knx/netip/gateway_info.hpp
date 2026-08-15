// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace knx {
namespace netip {

struct GatewayInfo {
    IpAddress ipAddress{};
    NetIpPort port{NetIpPort::invalid()};
    std::string friendlyName;
    uint8_t macAddress[6]{};
    std::vector<uint8_t> deviceDIB;
    std::vector<uint8_t> supportedServices;     // DIB type 0x02: plain service families
    std::vector<uint8_t> securedServiceFamilies; // DIB type 0x06: families requiring secure communication

    /// Returns true if the given service family (e.g. 0x04=Tunnelling, 0x05=Routing) requires
    /// secure communication according to PID_SECURED_SERVICE_FAMILIES (DIB 0x06).
    bool requiresSecure(uint8_t family) const noexcept {
        // Each DIB 0x06 entry is 2 bytes: {family_id, security_version}.
        // security_version > 0 means secure is required.
        constexpr size_t kDibHeaderLen = 2; // len + type
        for (size_t i = kDibHeaderLen; i + 1 < securedServiceFamilies.size(); i += 2) {
            if (securedServiceFamilies[i] == family) {
                return securedServiceFamilies[i + 1] > 0;
            }
        }
        return false;
    }
};

} // namespace netip
} // namespace knx