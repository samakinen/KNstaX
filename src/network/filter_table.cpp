// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file filter_table.cpp
 * @brief Filter Table Realisation Type 3 bit field (03/05/01 §5.3.1).
 */

#include "knx/network/filter_table.hpp"

#include <algorithm>
#include <bit>

namespace knx {
namespace network {

namespace {

/// Mask of the bits at positions [from, to] inclusive within one octet.
constexpr uint8_t octetMask(unsigned from, unsigned to)
{
    return static_cast<uint8_t>((0xFFu << from) & (0xFFu >> (7u - to)));
}

}  // namespace

void FilterTable::setRange(uint16_t first, uint16_t last, bool pass) noexcept
{
    if (first > last) {
        return;
    }

    const size_t firstOctet = first >> 3;
    const size_t lastOctet = last >> 3;
    const auto apply = [this, pass](size_t octet, uint8_t mask) {
        _bits[octet] = pass ? static_cast<uint8_t>(_bits[octet] | mask)
                            : static_cast<uint8_t>(_bits[octet] & ~mask);
    };

    if (firstOctet == lastOctet) {
        apply(firstOctet, octetMask(first & 7u, last & 7u));
    } else {
        apply(firstOctet, octetMask(first & 7u, 7u));
        std::fill(_bits.begin() + static_cast<std::ptrdiff_t>(firstOctet + 1),
                  _bits.begin() + static_cast<std::ptrdiff_t>(lastOctet),
                  pass ? uint8_t{0xFF} : uint8_t{0x00});
        apply(lastOctet, octetMask(0u, last & 7u));
    }

    // GA 0000h is "don't care" (§5.3.1.1.2): keep its bit cleared so the raw
    // table and passCount() never report it.
    _bits[0] = static_cast<uint8_t>(_bits[0] & 0xFEu);
}

void FilterTable::setAll(bool pass) noexcept
{
    _bits.fill(pass ? 0xFFu : 0x00u);
    _bits[0] = static_cast<uint8_t>(_bits[0] & 0xFEu);
}

bool FilterTable::rangeIs(uint16_t first, uint16_t last, bool pass) const noexcept
{
    if (first > last) {
        return true;
    }
    if (first == 0u) {
        if (last == 0u) {
            return true;
        }
        first = 1u;
    }

    const size_t firstOctet = first >> 3;
    const size_t lastOctet = last >> 3;
    const auto matches = [this, pass](size_t octet, uint8_t mask) {
        const uint8_t bits = static_cast<uint8_t>(_bits[octet] & mask);
        return pass ? bits == mask : bits == 0u;
    };

    if (firstOctet == lastOctet) {
        return matches(firstOctet, octetMask(first & 7u, last & 7u));
    }
    if (!matches(firstOctet, octetMask(first & 7u, 7u)) || !matches(lastOctet, octetMask(0u, last & 7u))) {
        return false;
    }
    const uint8_t expected = pass ? 0xFFu : 0x00u;
    return std::all_of(_bits.begin() + static_cast<std::ptrdiff_t>(firstOctet + 1),
                       _bits.begin() + static_cast<std::ptrdiff_t>(lastOctet),
                       [expected](uint8_t octet) { return octet == expected; });
}

size_t FilterTable::passCount() const noexcept
{
    size_t count = 0;
    for (uint8_t octet : _bits) {
        count += static_cast<size_t>(std::popcount(octet));
    }
    // Bit 0 is kept clear by every mutator, but octets() is writable.
    return count - ((_bits[0] & 0x01u) != 0u ? 1u : 0u);
}

} // namespace network
} // namespace knx
