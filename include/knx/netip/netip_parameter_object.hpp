// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/types.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace knx {
namespace netip {

/**
 * @file netip_parameter_object.hpp
 * @brief The device's IP identity, as ETS reads it back.
 *
 * The KNXnet/IP Parameter Object (interface object type 11) is where ETS looks
 * for everything about a device that is specific to it living on IP: which
 * address it currently answers on, which multicast group it routes to, what it
 * is called in a device list. The stack registers the object itself, but only
 * the firmware knows the answers — the addresses come from DHCP, and none of it
 * exists until the interface is up.
 *
 * Seeding it is not decoration. A device that leaves these at zero reads to ETS
 * as one with no address and no name, which is worse than a device that has not
 * been found at all: it looks found and broken.
 */
struct KnxNetIpConfiguration {
    /// PID_CURRENT_IP_ADDRESS / _SUBNET_MASK / _DEFAULT_GATEWAY. The address in
    /// force now, whatever assigned it.
    IpAddress currentIpAddress{IpAddress(0)};
    IpAddress currentSubnetMask{IpAddress(0)};
    IpAddress currentDefaultGateway{IpAddress(0)};

    /// PID_ROUTING_MULTICAST_ADDRESS. 0.0.0.0 for a device that does not route.
    IpAddress routingMulticastAddress{IpAddress(0)};
    /// PID_SYSTEM_SETUP_MULTICAST_ADDRESS. Fixed at 224.0.23.12 by the
    /// standard — it is the address discovery happens on, so it is not the
    /// installation's to change even when routing has moved elsewhere.
    IpAddress systemSetupMulticastAddress{IpAddress::fromOctets(224, 0, 23, 12)};

    /// PID_TTL, the multicast hop limit.
    uint8_t timeToLive{16};

    /// PID_MAC_ADDRESS. The Ethernet/Wi-Fi address, which is not the same thing
    /// as the KNX serial number even where a device derives one from the other.
    std::array<uint8_t, 6> macAddress{};

    /// PID_FRIENDLY_NAME, up to 30 octets of ISO 8859-1. What ETS shows in a
    /// discovery list, so it should be the name a person gave the device.
    std::string friendlyName{};

    /**
     * PID_IP_ASSIGNMENT_METHOD / PID_CURRENT_IP_ASSIGNMENT_METHOD.
     * Bit 0 manual, bit 1 BootP, bit 2 DHCP, bit 3 AutoIP.
     */
    uint8_t ipAssignmentMethod{0x04};

    /**
     * PID_KNXNETIP_DEVICE_CAPABILITIES.
     * Bit 0 BootP, bit 1 DHCP, bit 2 AutoIP, bit 3 Tunnelling, bit 4 Object
     * Server. Claim only what the build contains: a device that advertises
     * tunnelling and then refuses a CONNECT_REQUEST fails commissioning in a
     * way that looks like a network fault.
     */
    uint16_t deviceCapabilities{0x0000};
};

namespace device_capability {
constexpr uint16_t kBootP = 0x0001;
constexpr uint16_t kDhcp = 0x0002;
constexpr uint16_t kAutoIp = 0x0004;
constexpr uint16_t kTunnelling = 0x0008;
constexpr uint16_t kObjectServer = 0x0010;
} // namespace device_capability

namespace ip_assignment {
constexpr uint8_t kManual = 0x01;
constexpr uint8_t kBootP = 0x02;
constexpr uint8_t kDhcp = 0x04;
constexpr uint8_t kAutoIp = 0x08;
} // namespace ip_assignment

} // namespace netip
} // namespace knx
