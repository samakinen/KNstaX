// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file seeded_random.hpp
 * @brief A random generator for a part without a hardware one
 *
 * Data Secure needs unpredictable octets in two places: the Random of an
 * S-A_Sync_Response, which must never repeat for a key, and, on a device that
 * makes its own Factory Tool Key, the key itself. A bare-metal part such as the
 * GD32F103 has no RNG, so the board collects what it has: conversion noise of
 * an ADC, its unique ID, a counter it keeps across resets.
 *
 * SeededRandom turns that material into a key and runs AES-128 in counter mode
 * with it (as NIST SP 800-90A's CTR_DRBG does, without its reseeding):
 *
 *  - absorb() runs a CBC-MAC with a fixed key over the material: every octet
 *    of it reaches the key, and the key is uniform if the material holds
 *    enough entropy, however it is spread;
 *  - finish() makes the CBC-MAC the key, and fill() then returns
 *    AES_key(counter), counter + 1, ...
 *
 * The output never repeats within one seeding (a counter mode). Across
 * resets, the counter the board keeps in flash makes each seeding different
 * even if the noise were poor, and the noise makes the output unpredictable.
 * Nothing here allocates; the state is about 0.2 KiB with the software AES.
 */

#include "knx/security/aes_block.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace security {

class SeededRandom {
public:
    SeededRandom() { (void)_cipher.setKey(kAbsorbKey); }

    SeededRandom(const SeededRandom&) = delete;
    SeededRandom& operator=(const SeededRandom&) = delete;

    /// Add seed material. Only before finish().
    void absorb(std::span<const uint8_t> material)
    {
        if (_ready) {
            return;
        }
        for (const uint8_t octet : material) {
            _chunk[_chunkLength++] = octet;
            if (_chunkLength == _chunk.size()) {
                absorbChunk();
            }
        }
        _absorbed += material.size();
    }

    /// End the seeding: the material so far becomes the key.
    void finish()
    {
        if (_ready) {
            return;
        }
        // Padding as ISO/IEC 9797-1 method 2, and the total length, so two
        // seeds that differ only in trailing zeros give different keys.
        _chunk[_chunkLength++] = 0x80u;
        if (_chunkLength > _chunk.size() - 8u) {
            while (_chunkLength < _chunk.size()) _chunk[_chunkLength++] = 0u;
            absorbChunk();
        }
        while (_chunkLength < _chunk.size() - 8u) _chunk[_chunkLength++] = 0u;
        for (int shift = 56; shift >= 0; shift -= 8) {
            _chunk[_chunkLength++] = static_cast<uint8_t>(static_cast<uint64_t>(_absorbed) >> shift);
        }
        absorbChunk();

        (void)_cipher.setKey(_state);
        _state.fill(0);
        _counter.fill(0);
        _ready = true;
    }

    /// True once finish() has run.
    bool ready() const { return _ready; }

    /// Fill @p out. Before finish() it finishes with what was absorbed.
    void fill(std::span<uint8_t> out)
    {
        if (!_ready) {
            finish();
        }
        std::array<uint8_t, 16> block{};
        size_t used = block.size();
        for (uint8_t& octet : out) {
            if (used == block.size()) {
                (void)_cipher.encryptBlock(_counter.data(), block.data());
                increment();
                used = 0;
            }
            octet = block[used++];
        }
        block.fill(0);
    }

private:
    // A fixed, arbitrary key for the CBC-MAC (the first 16 octets of the
    // binary expansion of pi). Its secrecy does not matter.
    static constexpr std::array<uint8_t, 16> kAbsorbKey{0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
                                                        0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44};

    void absorbChunk()
    {
        for (size_t i = 0; i < _state.size(); ++i) {
            _state[i] = static_cast<uint8_t>(_state[i] ^ _chunk[i]);
        }
        (void)_cipher.encryptBlock(_state.data(), _state.data());
        _chunkLength = 0;
    }

    void increment()
    {
        for (size_t i = _counter.size(); i-- > 0;) {
            if (++_counter[i] != 0u) {
                break;
            }
        }
    }

    Aes128BlockCipher _cipher;
    std::array<uint8_t, 16> _state{};
    std::array<uint8_t, 16> _chunk{};
    std::array<uint8_t, 16> _counter{};
    size_t _chunkLength{0};
    size_t _absorbed{0};
    bool _ready{false};
};

}  // namespace security
}  // namespace knx
