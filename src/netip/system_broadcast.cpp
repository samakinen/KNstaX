// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/system_broadcast.hpp"

#include "knx/application/apci_services.hpp"
#include "knx/security/data_secure.hpp"

namespace knx {
namespace netip {

namespace {

/// Security Control Field bit 3, "system broadcast" (03/03/07). When ETS
/// secures a broadcast it sets this, and §4.1.3 makes it the deciding field.
constexpr uint8_t kScfSystemBroadcast = 0x08;

/// A_SecureService, the ten-bit APCI that carries every Data Secure PDU.
constexpr uint16_t kApciSecure =
    (static_cast<uint16_t>(security::DataSecureSession::APCI_SEC_HIGH) << 8) |
    static_cast<uint16_t>(security::DataSecureSession::APCI_SEC_LOW);

} // namespace

bool isSystemBroadcastApdu(std::span<const uint8_t> tpdu) noexcept
{
    if (tpdu.size() < 2) return false;

    const uint16_t raw = static_cast<uint16_t>((static_cast<uint16_t>(tpdu[0] & 0x03) << 8) |
                                               static_cast<uint16_t>(tpdu[1]));
    const application::APCIField apci{raw};

    // A_SystemNetworkParameter_{Read,Response,Write} are system broadcasts by
    // definition — that is the whole point of the "system" in their name.
    if (apci.isSystemNetworkParameter()) {
        return true;
    }

    // A secure APDU carries the answer itself, in the SCF that follows the
    // two APCI octets. Compared against the assembled ten-bit APCI rather than
    // the raw first octet, whose top six bits are the TPCI.
    if (raw == kApciSecure) {
        if (tpdu.size() < 3) return false;
        return (tpdu[2] & kScfSystemBroadcast) != 0;
    }

    return false;
}

bool isSystemBroadcast(const datalink::LDataFrame& frame) noexcept
{
    // The domain distinction only exists for broadcasts. Everything else —
    // group traffic, point-to-point — has an unambiguous destination and
    // travels as an ordinary routing indication.
    if (!isGroupAddress(frame.destinationType) || frame.destination.raw != 0x0000u) {
        return false;
    }

    return isSystemBroadcastApdu(std::span<const uint8_t>(frame.tpdu.data(), frame.tpdu.size()));
}

} // namespace netip
} // namespace knx
