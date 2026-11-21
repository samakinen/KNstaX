// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/security/aes128_soft.hpp"

namespace knx {
namespace security {

namespace {

constexpr uint8_t rotl8(uint8_t x, unsigned n) {
    return static_cast<uint8_t>((x << n) | (x >> (8u - n)));
}

/// The AES S-box (FIPS-197 §5.1.1) from its definition: the multiplicative
/// inverse in GF(2^8) followed by the affine transformation. p walks the field
/// by multiplying by 3 (a generator) while q walks it by dividing by 3, so q is
/// always the inverse of p.
consteval std::array<uint8_t, 256> makeSbox() {
    std::array<uint8_t, 256> box{};
    uint8_t p = 1;
    uint8_t q = 1;
    do {
        p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80u) != 0 ? 0x1Bu : 0u));
        q = static_cast<uint8_t>(q ^ (q << 1));
        q = static_cast<uint8_t>(q ^ (q << 2));
        q = static_cast<uint8_t>(q ^ (q << 4));
        if ((q & 0x80u) != 0) {
            q = static_cast<uint8_t>(q ^ 0x09u);
        }
        box[p] = static_cast<uint8_t>(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63u);
    } while (p != 1);
    box[0] = 0x63;  // 0 has no inverse; the affine map of 0
    return box;
}

constexpr std::array<uint8_t, 256> kSbox = makeSbox();

static_assert(kSbox[0x00] == 0x63 && kSbox[0x01] == 0x7C && kSbox[0x53] == 0xED && kSbox[0xFF] == 0x16,
              "S-box does not match FIPS-197 Figure 7");

// A column as a 32-bit word, octet 0 (row 0) in the low bits.
inline uint32_t load(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void store(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

inline uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

/// xtime on four octets at once.
inline uint32_t mul2(uint32_t x) {
    return ((x & 0x7F7F7F7Fu) << 1) ^ (((x >> 7) & 0x01010101u) * 0x1Bu);
}

/// S-box of row @p row of column @p x, as the low octet.
inline uint32_t sub(uint32_t x, unsigned row) { return kSbox[(x >> (8u * row)) & 0xFFu]; }

} // namespace

Aes128Soft::~Aes128Soft() {
    volatile uint32_t* key = _key;
    for (size_t i = 0; i < 4; ++i) {
        key[i] = 0;
    }
}

void Aes128Soft::setKey(const Key& key) noexcept {
    for (size_t i = 0; i < 4; ++i) {
        _key[i] = load(key.data() + 4 * i);
    }
}

void Aes128Soft::encrypt(const uint8_t* in, uint8_t* out) const noexcept {
    uint32_t k0 = _key[0];
    uint32_t k1 = _key[1];
    uint32_t k2 = _key[2];
    uint32_t k3 = _key[3];
    uint32_t s0 = load(in) ^ k0;
    uint32_t s1 = load(in + 4) ^ k1;
    uint32_t s2 = load(in + 8) ^ k2;
    uint32_t s3 = load(in + 12) ^ k3;
    uint32_t rcon = 1;
    for (unsigned round = 1;; ++round) {
        // SubBytes and ShiftRows in one pass: row n of column c comes from column c + n.
        const uint32_t t0 = sub(s0, 0) | (sub(s1, 1) << 8) | (sub(s2, 2) << 16) | (sub(s3, 3) << 24);
        const uint32_t t1 = sub(s1, 0) | (sub(s2, 1) << 8) | (sub(s3, 2) << 16) | (sub(s0, 3) << 24);
        const uint32_t t2 = sub(s2, 0) | (sub(s3, 1) << 8) | (sub(s0, 2) << 16) | (sub(s1, 3) << 24);
        const uint32_t t3 = sub(s3, 0) | (sub(s0, 1) << 8) | (sub(s1, 2) << 16) | (sub(s2, 3) << 24);

        // The next round key from this one (FIPS-197 §5.2): SubWord(RotWord(w3)) ^ Rcon.
        k0 ^= (sub(k3, 1) | (sub(k3, 2) << 8) | (sub(k3, 3) << 16) | (sub(k3, 0) << 24)) ^ rcon;
        k1 ^= k0;
        k2 ^= k1;
        k3 ^= k2;
        rcon = mul2(rcon);

        if (round == 10) {  // the last round has no MixColumns
            s0 = t0 ^ k0;
            s1 = t1 ^ k1;
            s2 = t2 ^ k2;
            s3 = t3 ^ k3;
            break;
        }

        // MixColumns: out_i = 2a_i ^ 3a_(i+1) ^ a_(i+2) ^ a_(i+3), all four rows at once.
        uint32_t u = t0 ^ ror(t0, 8);
        s0 = mul2(u) ^ ror(t0, 8) ^ ror(u, 16) ^ k0;
        u = t1 ^ ror(t1, 8);
        s1 = mul2(u) ^ ror(t1, 8) ^ ror(u, 16) ^ k1;
        u = t2 ^ ror(t2, 8);
        s2 = mul2(u) ^ ror(t2, 8) ^ ror(u, 16) ^ k2;
        u = t3 ^ ror(t3, 8);
        s3 = mul2(u) ^ ror(t3, 8) ^ ror(u, 16) ^ k3;
    }
    store(out, s0);
    store(out + 4, s1);
    store(out + 8, s2);
    store(out + 12, s3);
}

} // namespace security
} // namespace knx
