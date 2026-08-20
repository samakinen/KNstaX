// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/datalink/tp1_data_link_layer.hpp"

#include <cstdint>
#include <span>

namespace knx {
namespace netip {

/**
 * @file system_broadcast.hpp
 * @brief Which broadcasts belong to the system-broadcast domain on KNX IP.
 *
 * On an open medium the two broadcast domains are one control-field bit apart.
 * IP has no control field of its own, so KNXnet/IP splits them across two
 * service types instead: ROUTING_INDICATION and ROUTING_SYSTEM_BROADCAST
 * (03/02/06 §4.1.3, from AN183). A device that sends every broadcast as a
 * routing indication puts its S-A_Sync_Response in the wrong domain, and ETS —
 * which sends the request as a system broadcast — never sees the answer. That
 * is the failure this exists to prevent, and it is the last step of secure
 * commissioning, so it does not show up until the interesting part.
 *
 * The classification is by APDU rather than by anything the frame carries,
 * because the frame no longer carries it: §4.1.3 defines the domain in terms of
 * the service inside, and the stack's own LDataFrame drops the control-field
 * bit on the way through TP1's codec.
 */

/// True when a broadcast frame belongs to the system-broadcast domain.
///
/// False for anything that is not a broadcast at all, so this is safe to ask
/// about every outbound frame. Notably false for A_IndividualAddress_Write and
/// its siblings: those are ordinary broadcasts, and sending them as system
/// broadcasts would break plain commissioning to fix secure commissioning.
[[nodiscard]] bool isSystemBroadcast(const datalink::LDataFrame& frame) noexcept;

/// Same question asked of a raw TPDU plus its addressing, for callers that have
/// not built an LDataFrame.
[[nodiscard]] bool isSystemBroadcastApdu(std::span<const uint8_t> tpdu) noexcept;

} // namespace netip
} // namespace knx
