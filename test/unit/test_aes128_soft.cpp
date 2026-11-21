// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/security/aes128_ctr.hpp"
#include "knx/security/aes128_soft.hpp"
#include "knx/security/aes_block.hpp"

#include <array>
#include <cstdint>
#include <span>

// ============================================================================
// The portable AES-128 (aes128_soft.hpp) on its own, whatever backend the
// build is configured with, and against that backend:
//
//   FIPS-197 Appendix B and C.1     the cipher example and the AES-128 vector
//   SP 800-38A F.5.1                CTR-AES128.Encrypt, the keystream blocks
//                                   (Aes128Soft) and Aes128Ctr (the backend)
//   in place, key change            what CCM and CTR rely on
//   differential                    Aes128Soft against Aes128BlockCipher over
//                                   chained keys and blocks
//
// The KNX CCM vectors (03/03/07 Annex C) are in test_secure_application_layer,
// which runs on the configured backend; the host-small profile configures the
// software one.
// ============================================================================

using knx::security::Aes128BlockCipher;
using knx::security::Aes128Ctr;
using knx::security::Aes128Soft;

namespace {

using Block = std::array<uint8_t, 16>;

const Aes128Soft::Key kKeyC1{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                             0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
const Block kPlainC1{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                     0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
const Block kCipherC1{0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                      0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};

// FIPS-197 Appendix B and SP 800-38A share this key.
const Aes128Soft::Key kKeySp800{0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};

// SP 800-38A F.5.1: initial counter block, plaintext and ciphertext.
const Block kCtrInit{0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                     0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
const std::array<uint8_t, 64> kCtrPlain{
    0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
    0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
    0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
    0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
const std::array<uint8_t, 64> kCtrCipher{
    0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce,
    0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70, 0xfd, 0xff, 0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff,
    0x5a, 0xe4, 0xdf, 0x3e, 0xdb, 0xd5, 0xd3, 0x5e, 0x5b, 0x4f, 0x09, 0x02, 0x0d, 0xb0, 0x3e, 0xab,
    0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03, 0xd1, 0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee};

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_fips197_c1_aes128(void) {
    const Aes128Soft aes(kKeyC1);
    Block out{};
    aes.encrypt(kPlainC1.data(), out.data());
    TEST_ASSERT_EQUAL_MEMORY(kCipherC1.data(), out.data(), out.size());
}

void test_fips197_appendix_b_cipher_example(void) {
    const Block plain{0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d,
                      0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34};
    const Block expected{0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb,
                         0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32};
    const Aes128Soft aes(kKeySp800);
    Block out{};
    aes.encrypt(plain.data(), out.data());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), out.data(), out.size());
}

void test_encrypts_in_place(void) {
    const Aes128Soft aes(kKeyC1);
    Block block = kPlainC1;
    aes.encrypt(block.data(), block.data());
    TEST_ASSERT_EQUAL_MEMORY(kCipherC1.data(), block.data(), block.size());
}

void test_set_key_replaces_the_key(void) {
    Aes128Soft aes;
    aes.setKey(kKeySp800);
    aes.setKey(kKeyC1);
    Block out{};
    aes.encrypt(kPlainC1.data(), out.data());
    TEST_ASSERT_EQUAL_MEMORY(kCipherC1.data(), out.data(), out.size());
}

void test_sp800_38a_ctr_keystream(void) {
    const Aes128Soft aes(kKeySp800);
    Block counter = kCtrInit;
    std::array<uint8_t, 64> out{};
    for (size_t block = 0; block < 4; ++block) {
        Block keyStream{};
        aes.encrypt(counter.data(), keyStream.data());
        for (size_t i = 0; i < 16; ++i) {
            out[16 * block + i] = static_cast<uint8_t>(kCtrPlain[16 * block + i] ^ keyStream[i]);
        }
        for (size_t i = counter.size(); i-- > 0;) {
            if (++counter[i] != 0) {
                break;
            }
        }
    }
    TEST_ASSERT_EQUAL_MEMORY(kCtrCipher.data(), out.data(), out.size());
}

void test_sp800_38a_ctr_on_the_configured_backend(void) {
    std::array<uint8_t, 64> out{};
    TEST_ASSERT_TRUE(Aes128Ctr::crypt(kKeySp800, kCtrInit, kCtrPlain, out).isOk());
    TEST_ASSERT_EQUAL_MEMORY(kCtrCipher.data(), out.data(), out.size());

    // A length that is not a whole number of blocks stops mid-keystream.
    std::array<uint8_t, 21> partial{};
    TEST_ASSERT_TRUE(Aes128Ctr::crypt(kKeySp800, kCtrInit, std::span(kCtrPlain).first<21>(), partial).isOk());
    TEST_ASSERT_EQUAL_MEMORY(kCtrCipher.data(), partial.data(), partial.size());
}

// Each step's ciphertext is the next key and the next block, so a fault
// anywhere in the cipher or the key schedule spreads to every later step.
void test_agrees_with_the_configured_backend(void) {
    Aes128Soft::Key key = kKeyC1;
    Block block = kPlainC1;
    for (int step = 0; step < 500; ++step) {
        const Aes128Soft soft(key);
        Aes128BlockCipher backend;
        TEST_ASSERT_TRUE(backend.setKey(key).isOk());

        Block fromSoft{};
        Block fromBackend{};
        soft.encrypt(block.data(), fromSoft.data());
        TEST_ASSERT_TRUE(backend.encryptBlock(block.data(), fromBackend.data()).isOk());
        TEST_ASSERT_EQUAL_MEMORY(fromBackend.data(), fromSoft.data(), fromSoft.size());

        for (size_t i = 0; i < 16; ++i) {
            key[i] = static_cast<uint8_t>(key[i] ^ fromSoft[i]);
        }
        block = fromSoft;
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fips197_c1_aes128);
    RUN_TEST(test_fips197_appendix_b_cipher_example);
    RUN_TEST(test_encrypts_in_place);
    RUN_TEST(test_set_key_replaces_the_key);
    RUN_TEST(test_sp800_38a_ctr_keystream);
    RUN_TEST(test_sp800_38a_ctr_on_the_configured_backend);
    RUN_TEST(test_agrees_with_the_configured_backend);
    return UNITY_END();
}
