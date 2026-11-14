// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file group_ack_filter.hpp
 * @brief The coupler's layer-2 acknowledge rule for group frames, in a form a
 *        receive ISR can evaluate.
 *
 * A coupler acknowledges a group-addressed frame it is going to route — or,
 * with GROUP_IACK_ROUT = 0, every multicast frame (03/05/01 §4.5.5, bit 5) —
 * in addition to the frames its own group address table subscribes to; the
 * acknowledge conditions are OR-ed (03/03/03 §2.4.2.4.4). On TP1 the
 * acknowledge is sent about 15 bit times after the frame, from the receive
 * interrupt, so the routing condition has to be reduced to something that can
 * be decided there: one mode per address range and, for GROUP_ROUT, a look-up
 * in the Filter Table bit field.
 *
 * The coupler owns the instances and re-derives them whenever its
 * configuration changes; the ISR only reads them. Every field is a single
 * octet or a pointer, so a concurrent update is never seen torn.
 */

#pragma once

#include <cstdint>

namespace knx {
namespace physical {

struct GroupAckFilter {
    enum class Mode : uint8_t {
        Never = 0,   ///< not routed (GROUP_LOCK, or the routing condition is false)
        Always = 1,  ///< routed without evaluation, or GROUP_IACK_ROUT = 0
        FilterTable = 2,  ///< routed iff the Filter Table bit is set
    };

    /// Filter Table Realisation Type 3 bit field (8 192 octets), or null.
    const uint8_t* filterTable{nullptr};
    /// Group addresses 0001h…6FFFh (GROUP_6FFF).
    Mode low{Mode::Never};
    /// Group addresses 7000h…FFFFh (GROUP_7000).
    Mode high{Mode::Never};

    /// Whether a group frame to @p address is acknowledged on the coupler's
    /// behalf. Broadcast (0000h) is outside this rule.
    [[gnu::always_inline]] bool acknowledges(uint16_t address) const noexcept
    {
        const Mode mode = address <= 0x6FFFu ? low : high;
        if (mode == Mode::Always) {
            return true;
        }
        if (mode == Mode::FilterTable && filterTable != nullptr) {
            return ((filterTable[address >> 3] >> (address & 0x07u)) & 0x01u) != 0u;
        }
        return false;
    }
};

} // namespace physical
} // namespace knx
