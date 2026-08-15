// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "knx/util/result.hpp"

namespace knx {
namespace security {

// AES-128 CTR stream cipher helper.
//
// Uses `counter0` as the initial 16-byte counter block and produces `out = in XOR keystream`.
class Aes128Ctr {
public:
    using Key = std::array<uint8_t, 16>;
    using Counter = std::array<uint8_t, 16>;

    static util::Result<void> crypt(const Key& key,
                                    const Counter& counter0,
                                    std::span<const uint8_t> in,
                                    std::span<uint8_t> out);
};

} // namespace security
} // namespace knx
