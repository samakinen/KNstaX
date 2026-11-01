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

class Aes128BlockCipher;

// AES-128 CBC-MAC as used by KNXnet/IP Secure wrapper primitives.
//
// Computes CBC-MAC over:
//   block0 (16 bytes) || u16be(len(additionalData)) || additionalData || payload
// then zero-pads to a 16-byte boundary and returns the last ciphertext block.
class Aes128CbcMac {
public:
    using Key = std::array<uint8_t, 16>;
    using Block = std::array<uint8_t, 16>;

    static util::Result<void> compute(const Key& key,
                                      const Block& block0,
                                      std::span<const uint8_t> additionalData,
                                      std::span<const uint8_t> payload,
                                      Block& macOut);

    // The same, with a cipher already keyed by the caller. A context that
    // protects every datagram under one key keeps the key schedule rather than
    // rebuilding it two or three times per packet.
    static util::Result<void> compute(Aes128BlockCipher& keyedCipher,
                                      const Block& block0,
                                      std::span<const uint8_t> additionalData,
                                      std::span<const uint8_t> payload,
                                      Block& macOut);
};

} // namespace security
} // namespace knx
