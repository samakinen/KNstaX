// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

/**
 * @file core_discovery_codec.hpp
 * @brief The server half of KNXnet/IP discovery and self-description.
 *
 * KNstaX already spoke the client half — a device looking for a gateway. This
 * is the other direction: what a KNX IP device answers when ETS asks the
 * network who is there. Core is not optional for it; 03/08/01 Table 2 makes the
 * Core service family mandatory for Device Class C, which is every KNXnet/IP
 * device that is neither a router nor a tool.
 *
 * Encoding only — no sockets and no device state. What the device currently is
 * arrives as a `CoreDeviceDescription` snapshot, which keeps every field in the
 * response traceable to something the caller can see, and lets the whole wire
 * format be tested against the spec's own layouts at host speed.
 */

/// One entry of the Supported Service Families DIB (03/08/02 §7.5.4.3).
struct ServiceFamilySupport {
    uint8_t family{0};
    uint8_t version{0};
};

namespace service_family {
constexpr uint8_t kCore = 0x02;
constexpr uint8_t kDeviceManagement = 0x03;
constexpr uint8_t kTunnelling = 0x04;
constexpr uint8_t kRouting = 0x05;
constexpr uint8_t kRemoteLogging = 0x06;
constexpr uint8_t kRemoteConfiguration = 0x07;
constexpr uint8_t kObjectServer = 0x08;
/// Only ever announced in a SEARCH_RESPONSE_EXTENDED — Table 3 forbids it in
/// the plain SEARCH_RESPONSE and DESCRIPTION_RESPONSE.
constexpr uint8_t kSecurity = 0x09;
} // namespace service_family

/// Everything the Core responder can be asked about the device, sampled at the
/// moment a request arrives.
struct CoreDeviceDescription {
    /// DPT_Media, exactly one bit set. A KNX IP device is 0x20.
    uint8_t medium{static_cast<uint8_t>(MediumType::IP)};
    /// Bit 0 of the Device Status octet, and the reason ETS can find a device
    /// whose address it does not yet know.
    bool programmingMode{false};
    IndividualAddress individualAddress{};
    /// Assigned by ETS; 0 means "not part of a project installation".
    uint16_t projectInstallationId{0};
    std::array<uint8_t, 6> serialNumber{};
    /// 0.0.0.0 for a device that does not implement Routing.
    IpAddress routingMulticastAddress{IpAddress(0)};
    std::array<uint8_t, 6> macAddress{};
    /// ISO 8859-1, NUL-terminated, NUL-padded to 30 octets on the wire.
    std::array<char, 30> friendlyName{};

    // --- Extended Device Information DIB (§7.5.4.9) ------------------------
    /// PID_MEDIUM_STATUS. A device with no second medium reports
    /// COMMUNICATION_IMPOSSIBLE = false, i.e. 0.
    uint8_t mediumStatus{0};
    uint16_t maxLocalApduLength{254};
    /// PID_DEVICE_DESCRIPTOR (mask version), e.g. 0x07B0 for System 7.
    uint16_t deviceDescriptorType0{0};
};

class CoreDiscoveryCodec {
public:
    static constexpr NetIpServiceType kServiceSearchRequestExtended = NetIpServiceType(0x020B);
    static constexpr NetIpServiceType kServiceSearchResponseExtended = NetIpServiceType(0x020C);

    static constexpr uint8_t kDibDeviceInfo = 0x01;
    static constexpr uint8_t kDibSupportedServiceFamilies = 0x02;
    static constexpr uint8_t kDibIpConfig = 0x03;
    static constexpr uint8_t kDibIpCurrentConfig = 0x04;
    static constexpr uint8_t kDibKnxAddresses = 0x05;
    static constexpr uint8_t kDibSecuredServiceFamilies = 0x06;
    static constexpr uint8_t kDibExtendedDeviceInfo = 0x08;

    static constexpr uint8_t kDeviceInfoDibLength = 54;
    static constexpr uint8_t kExtendedDeviceInfoDibLength = 8;
    static constexpr size_t kFriendlyNameLength = 30;

    // --- DIB builders ------------------------------------------------------

    static util::Result<void> appendDeviceInfoDib(PacketWriter& writer, const CoreDeviceDescription& device);

    /// Every DIB is padded to an even length (§7.5.4.1), so an odd number of
    /// families gets a trailing 00h pair rather than an odd structure length.
    static util::Result<void> appendServiceFamiliesDib(PacketWriter& writer,
                                                       uint8_t dibType,
                                                       std::span<const ServiceFamilySupport> families);

    static util::Result<void> appendExtendedDeviceInfoDib(PacketWriter& writer,
                                                          const CoreDeviceDescription& device);

    // --- Response builders -------------------------------------------------

    /// SEARCH_RESPONSE carries the server's own control endpoint, then the
    /// device-information and supported-service-families DIBs.
    static util::Result<void> encodeSearchResponse(PacketWriter& writer,
                                                   const control_packet::HpaiEndpoint& controlEndpoint,
                                                   const CoreDeviceDescription& device,
                                                   std::span<const ServiceFamilySupport> families);

    /// `requestedDibs` is the Request DIBs SRP, empty when the client sent
    /// none. The basic set is always included whatever it asks for, because
    /// §7.6.3.3.6 makes the SRP a floor rather than a filter.
    static util::Result<void> encodeSearchResponseExtended(PacketWriter& writer,
                                                           const control_packet::HpaiEndpoint& controlEndpoint,
                                                           const CoreDeviceDescription& device,
                                                           std::span<const ServiceFamilySupport> families,
                                                           std::span<const uint8_t> requestedDibs);

    /// DESCRIPTION_RESPONSE carries DIBs only — no HPAI, because the client
    /// already knows the endpoint it addressed.
    static util::Result<void> encodeDescriptionResponse(PacketWriter& writer,
                                                        const CoreDeviceDescription& device,
                                                        std::span<const ServiceFamilySupport> families);

    // --- Request parsing ---------------------------------------------------

    /// The return endpoint of a SEARCH_REQUEST or DESCRIPTION_REQUEST.
    static util::Result<control_packet::HpaiEndpoint> parseReturnEndpoint(std::span<const uint8_t> datagram,
                                                                          NetIpServiceType expectedServiceType);
};

} // namespace netip
} // namespace knx
