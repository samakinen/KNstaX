// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file filter_table.hpp
 * @brief A coupler's group-address Filter Table (03/05/01 §5.3.1).
 *
 * The table is the bit field of Filter Table Realisation Type 3 (03/05/01
 * §5.3.1.1): 65 536 bits, one per Group Address, in 8 192 octets. Group
 * Address `ga` lives in octet `ga / 8`, bit `ga % 8` counted from the least
 * significant bit. A set bit means the Filter Table evaluation for that
 * address is positive (the message may pass), a cleared bit that it is
 * negative. Bit 0 (GA 0000h, the broadcast address) is "don't care" and is
 * never evaluated.
 *
 * The same layout is used by Realisation Type 2 (mask 0912h, §5.2.1, "the
 * Filter Table supports filtering of full 16 bit Group Addressing"); the older
 * 3.5 KB table of masks 0910h/0911h covered only 0000h…6FFFh. A full-range
 * table is needed because PID_*_LCGRPCONFIG GROUP_ROUT7000 (§4.5.6) routes
 * addresses ≥ 7000h "depending on the Routing Table" too.
 *
 * Every range ETS can download fits: a range operation is a loop over bits, so
 * there is no entry limit, and a later Set always overrides an earlier Clear
 * of the same address (and vice versa).
 *
 * 8 KB is a coupler's cost, so only a coupler instantiates one: it is a
 * member of @ref TwoPortCoupler, which is itself only created for a coupler
 * stack port. End devices have no Filter Table.
 */

#pragma once

#include "knx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace network {

class FilterTable {
public:
    /// 65 536 bits, one per 16-bit Group Address (§5.3.1.1.1).
    static constexpr size_t kOctets = 8192u;

    /// A new table is cleared: "The default Load State shall be Loaded and the
    /// Routing Table cleared" (03/05/01 §4.5.3).
    FilterTable() = default;

    /// Filter Table evaluation for @p address (§5.3.1.1.2): true = positive.
    /// GA 0000h is never evaluated and always reads negative.
    bool passes(GroupAddress address) const noexcept
    {
        if (address.raw == 0u) {
            return false;
        }
        return (_bits[address.raw >> 3] & bitMask(address.raw)) != 0u;
    }

    /// Set (pass) or clear (block) one address. GA 0000h is ignored.
    void set(GroupAddress address, bool pass) noexcept
    {
        if (address.raw == 0u) {
            return;
        }
        if (pass) {
            _bits[address.raw >> 3] = static_cast<uint8_t>(_bits[address.raw >> 3] | bitMask(address.raw));
        } else {
            _bits[address.raw >> 3] = static_cast<uint8_t>(_bits[address.raw >> 3] & ~bitMask(address.raw));
        }
    }

    /// Set or clear every address from @p first to @p last inclusive.
    /// Does nothing when @p first > @p last.
    void setRange(uint16_t first, uint16_t last, bool pass) noexcept;

    /// SRVID_SET_FILTERTABLE / SRVID_CLEAR_FILTERTABLE: every address.
    void setAll(bool pass) noexcept;

    /// True when every address in [first, last] has the state @p pass
    /// (the State_Read checks of §4.5.7.4/§4.5.7.5). GA 0000h is skipped.
    /// An empty range (first > last) is vacuously true.
    bool rangeIs(uint16_t first, uint16_t last, bool pass) const noexcept;

    /// Number of addresses that pass (GA 0000h not counted).
    size_t passCount() const noexcept;

    /// The raw table in the §5.3.1.1.1 layout, for a memory-mapped view
    /// (Realisation Type 3 is loaded by the MaC with A_Memory_Write) and for
    /// persistence.
    std::span<uint8_t, kOctets> octets() noexcept { return _bits; }
    std::span<const uint8_t, kOctets> octets() const noexcept { return _bits; }

private:
    static constexpr uint8_t bitMask(uint16_t address) noexcept
    {
        return static_cast<uint8_t>(1u << (address & 0x07u));
    }

    std::array<uint8_t, kOctets> _bits{};
};

} // namespace network
} // namespace knx
