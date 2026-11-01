// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file secure_memory.hpp
 * @brief Comparing and erasing secrets without telling the optimiser or a timer.
 *
 * Two small things every KNX Secure path needs and that the obvious library
 * calls get wrong for the purpose:
 *
 *  - `memcmp` and `operator==` return at the first differing octet, so the time
 *    a MAC comparison takes says how many leading octets of a forged MAC were
 *    right. constantTimeEqual() always looks at every octet.
 *  - `memset` on a buffer that is about to go out of scope is a dead store the
 *    compiler may delete. secureZero() writes through a volatile pointer, which
 *    it may not.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace security {

/// Equal length and equal contents, in time that depends only on the length.
[[nodiscard]] inline bool constantTimeEqual(std::span<const uint8_t> a, std::span<const uint8_t> b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

/// Overwrite with zeros in a way the optimiser cannot drop as a dead store.
inline void secureZero(std::span<uint8_t> bytes) noexcept
{
    volatile uint8_t* p = bytes.data();
    for (size_t i = 0; i < bytes.size(); ++i) {
        p[i] = 0;
    }
}

template <size_t N>
inline void secureZero(std::array<uint8_t, N>& bytes) noexcept
{
    secureZero(std::span<uint8_t>(bytes));
}

} // namespace security
} // namespace knx
