// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_seeded_random.cpp
 * @brief The seeded generator for parts without a hardware RNG
 */

#include "unity.h"
#include "knx/security/seeded_random.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using knx::security::Aes128BlockCipher;
using knx::security::SeededRandom;

namespace {

std::vector<uint8_t> output(std::span<const uint8_t> seed, size_t length) {
    SeededRandom random;
    random.absorb(seed);
    random.finish();
    std::vector<uint8_t> out(length);
    random.fill(out);
    return out;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// The construction, recomputed here from the block cipher: a CBC-MAC with the
// fixed key over the padded material and its length, then AES in counter mode
// from 0 under it.
void test_output_is_aes_ctr_under_the_cbc_mac_of_the_seed(void) {
    const std::array<uint8_t, 5> seed{1, 2, 3, 4, 5};

    std::array<uint8_t, 16> block{1, 2, 3, 4, 5, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::array<uint8_t, 16> lengthBlock{};
    lengthBlock[15] = 5;
    // Material, 0x80 and zeros fill the first block only up to octet 8; the
    // 64-bit length goes in the last eight octets of the same block.
    std::memcpy(block.data() + 8, lengthBlock.data() + 8, 8);
    Aes128BlockCipher mac;
    TEST_ASSERT_TRUE(mac.setKey({0x24, 0x3F, 0x6A, 0x88, 0x85, 0xA3, 0x08, 0xD3,
                                 0x13, 0x19, 0x8A, 0x2E, 0x03, 0x70, 0x73, 0x44}).isOk());
    std::array<uint8_t, 16> key{};
    TEST_ASSERT_TRUE(mac.encryptBlock(block.data(), key.data()).isOk());

    Aes128BlockCipher ctr;
    TEST_ASSERT_TRUE(ctr.setKey(key).isOk());
    std::array<uint8_t, 16> counter{};
    std::array<uint8_t, 32> expected{};
    TEST_ASSERT_TRUE(ctr.encryptBlock(counter.data(), expected.data()).isOk());
    counter[15] = 1;
    TEST_ASSERT_TRUE(ctr.encryptBlock(counter.data(), expected.data() + 16).isOk());

    const auto out = output(seed, 32);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), out.data(), 32);
}

// One octet of difference anywhere in the seed, or only its length, changes the output.
void test_every_octet_of_the_seed_counts(void) {
    std::vector<uint8_t> seed(100, 0x55);
    const auto base = output(seed, 16);
    for (size_t i = 0; i < seed.size(); i += 33) {
        auto changed = seed;
        changed[i] ^= 0x01;
        TEST_ASSERT_TRUE(output(changed, 16) != base);
    }
    std::vector<uint8_t> longer(seed);
    longer.push_back(0);
    TEST_ASSERT_TRUE(output(longer, 16) != base);
}

// Successive fills continue the stream: no block repeats, and a fill split in
// two gives the same octets as one fill.
void test_fills_continue_the_stream(void) {
    const std::array<uint8_t, 3> seed{9, 8, 7};
    const auto whole = output(seed, 64);

    SeededRandom random;
    random.absorb(seed);
    std::vector<uint8_t> first(10);
    std::vector<uint8_t> second(54);
    random.fill(first);   // finishes the seeding itself
    random.fill(second);
    TEST_ASSERT_TRUE(random.ready());
    // A fill starts a new counter block, so the split stream skips the rest of
    // the first block: the second fill starts at block 1.
    TEST_ASSERT_EQUAL_MEMORY(whole.data(), first.data(), 10);
    TEST_ASSERT_EQUAL_MEMORY(whole.data() + 16, second.data(), 48);

    for (size_t a = 0; a < 4; ++a) {
        for (size_t b = a + 1; b < 4; ++b) {
            TEST_ASSERT_TRUE(std::memcmp(whole.data() + a * 16, whole.data() + b * 16, 16) != 0);
        }
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_output_is_aes_ctr_under_the_cbc_mac_of_the_seed);
    RUN_TEST(test_every_octet_of_the_seed_counts);
    RUN_TEST(test_fills_continue_the_stream);
    return UNITY_END();
}
