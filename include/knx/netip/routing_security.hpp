// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/netip_security.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

/**
 * @brief NetIpSecurity for the routing multicast group.
 *
 * Secure Routing is more than wrapping datagrams: the group shares a
 * synchronised multicast timer, kept aligned by TIMER_NOTIFY frames that are
 * neither routing indications nor wrapped (03_08_09 §2.2.2.3–§2.2.2.4). The
 * routing endpoint owns the socket and the receive loop, so it is the one that
 * hands TIMER_NOTIFY frames in and sends the ones that fall due; the security
 * context owns the key and the timer and does everything else.
 *
 * Kept as an interface so the endpoint builds without KNX_SECURE_ENABLED,
 * where there is simply never a context installed.
 */
class RoutingSecurity : public NetIpSecurity {
public:
    /// Authenticate a received TIMER_NOTIFY (0955h) and run it through the
    /// timer synchronisation. An error means the frame was discarded.
    virtual util::Result<void> handleTimerNotify(std::span<const uint8_t> datagram) = 0;

    /// When a TIMER_NOTIFY has fallen due, encode it into `out` and return its
    /// length. Returns 0 when none is due.
    virtual util::Result<size_t> takeDueTimerNotify(std::span<uint8_t> out) = 0;

    /// How long the caller may block before takeDueTimerNotify() next has
    /// something to send.
    virtual uint32_t millisecondsUntilTimerNotify() = 0;
};

} // namespace netip
} // namespace knx
