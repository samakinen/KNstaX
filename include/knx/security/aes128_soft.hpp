// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file aes128_soft.hpp
 * @brief Portable AES-128, encryption only.
 *
 * KNX Data Secure (CCM: CBC-MAC and CTR) and the KNXnet/IP Secure framing only
 * ever encrypt a block, so there is no decryption and no inverse S-box. The
 * state is held as four 32-bit columns and the round keys are derived on the
 * fly from the cipher key, so the object is the 16-octet key and there is no
 * expanded key schedule. One 256-octet S-box, built at compile time, is the only
 * table. See docs/design/software-aes.md (variant V3): about 800 octets of flash
 * and 1 500 instructions per block on a Cortex-M3.
 *
 * Loads and stores are by octet: no alignment requirement and no dependence on
 * the target's byte order.
 *
 * Timing: there are no branches on secret data, but the S-box lookups are
 * indexed by it. On parts without a data cache (Cortex-M0+/M3/M4) a table read
 * takes the same time whatever the index. On a CPU with a data cache (Cortex-M7,
 * application processors, hosts) the access pattern can leak through cache
 * timing to code that shares the cache; this implementation does not defend
 * against that. Prefer a hardware or constant-time library backend there.
 */

#include <array>
#include <cstddef>
#include <cstdint>

namespace knx {
namespace security {

class Aes128Soft {
public:
    static constexpr size_t kBlockSize = 16;
    using Key = std::array<uint8_t, 16>;
    using Block = std::array<uint8_t, kBlockSize>;

    /// An all-zero key until setKey().
    Aes128Soft() noexcept = default;
    explicit Aes128Soft(const Key& key) noexcept { setKey(key); }
    /// Wipes the key.
    ~Aes128Soft();

    Aes128Soft(const Aes128Soft&) = delete;
    Aes128Soft& operator=(const Aes128Soft&) = delete;

    void setKey(const Key& key) noexcept;

    /// Encrypts one 16-octet block. @p in and @p out may be the same block.
    void encrypt(const uint8_t* in, uint8_t* out) const noexcept;

private:
    uint32_t _key[4]{};
};

} // namespace security
} // namespace knx
