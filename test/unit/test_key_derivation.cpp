// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"
#include "knx/security/key_derivation.hpp"

#include "knx/security/data_secure.hpp"

#include <vector>
#include <ranges>
#include <algorithm>
#include <array>
#include <cstring>
#include <span>

using namespace knx::security;
using knx::GroupAddress;
using knx::IndividualAddress;

void setUp(void) {}
void tearDown(void) {}

// Test: Basic PBKDF2 derivation
void test_pbkdf2_derivation(void) {
    std::array<uint8_t, 8> password{'p', 'a', 's', 's', 'w', 'o', 'r', 'd'};
    std::array<uint8_t, 4> salt{'s', 'a', 'l', 't'};
    std::array<uint8_t, 32> output{};

    TEST_ASSERT_TRUE(KeyDerivation::pbkdf2(password, salt, 1000, output).isOk());
    TEST_ASSERT_EQUAL(32, output.size());

    // Derived key should be deterministic
    std::array<uint8_t, 32> output2{};
    TEST_ASSERT_TRUE(KeyDerivation::pbkdf2(password, salt, 1000, output2).isOk());
    TEST_ASSERT_TRUE(std::ranges::equal(output, output2));
}

// Test: Different salts produce different keys
void test_pbkdf2_different_salts(void) {
    std::array<uint8_t, 4> password{'p', 'a', 's', 's'};
    std::array<uint8_t, 5> salt1{'s', 'a', 'l', 't', '1'};
    std::array<uint8_t, 5> salt2{'s', 'a', 'l', 't', '2'};

    std::array<uint8_t, 16> output1, output2;
    TEST_ASSERT_TRUE(KeyDerivation::pbkdf2(password, salt1, 100, output1).isOk());
    TEST_ASSERT_TRUE(KeyDerivation::pbkdf2(password, salt2, 100, output2).isOk());

    // Keys should differ
    TEST_ASSERT_FALSE(std::memcmp(output1.data(), output2.data(), 16) == 0);
}

// Test: Different passwords produce different keys
void test_pbkdf2_different_passwords(void) {
    std::array<uint8_t, 4> salt{'s', 'a', 'l', 't'};
    std::array<uint8_t, 4> pass1{'a', 'a', 'a', 'a'};
    std::array<uint8_t, 4> pass2{'b', 'b', 'b', 'b'};

    std::array<uint8_t, 16> output1, output2;
    TEST_ASSERT_TRUE(KeyDerivation::pbkdf2(pass1, salt, 100, output1).isOk());
    TEST_ASSERT_TRUE(KeyDerivation::pbkdf2(pass2, salt, 100, output2).isOk());

    // Keys should differ
    TEST_ASSERT_FALSE(std::memcmp(output1.data(), output2.data(), 16) == 0);
}

// Test: More iterations increase security (but should still be deterministic)
void test_pbkdf2_iterations(void) {
    std::array<uint8_t, 4> password{'p', 'a', 's', 's'};
    std::array<uint8_t, 4> salt{'s', 'a', 'l', 't'};

    std::array<uint8_t, 16> output_100, output_1000;
    TEST_ASSERT_TRUE(KeyDerivation::pbkdf2(password, salt, 100, output_100).isOk());
    TEST_ASSERT_TRUE(KeyDerivation::pbkdf2(password, salt, 1000, output_1000).isOk());

    // More iterations should produce different output
    TEST_ASSERT_FALSE(std::memcmp(output_100.data(), output_1000.data(), 16) == 0);

    // But same iterations should be deterministic
    std::array<uint8_t, 16> output_1000_again;
    TEST_ASSERT_TRUE(KeyDerivation::pbkdf2(password, salt, 1000, output_1000_again).isOk());
    TEST_ASSERT_TRUE(std::ranges::equal(output_1000, output_1000_again));
}

// Test: Derive session key from master key and device ID
void test_derive_session_key(void) {
    KeyDerivation::MasterKey master_key{
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
    };
    KeyDerivation::DeviceId device_id{0x11, 0x01};  // Example KNX device address
    KeyDerivation::Key session_key;

    TEST_ASSERT_TRUE(KeyDerivation::deriveSessionKey(master_key, device_id, session_key).isOk());
    TEST_ASSERT_EQUAL(16, session_key.size());
    TEST_ASSERT_NOT_EQUAL(0, session_key[0]);  // Should not be zero
}

// Test: Different device IDs produce different session keys
void test_derive_session_key_different_devices(void) {
    KeyDerivation::MasterKey master_key{};
    for (int i = 0; i < 32; ++i) master_key[i] = i;

    KeyDerivation::DeviceId device_id1{0x11, 0x01};
    KeyDerivation::DeviceId device_id2{0x11, 0x02};
    KeyDerivation::Key key1, key2;

    TEST_ASSERT_TRUE(KeyDerivation::deriveSessionKey(master_key, device_id1, key1).isOk());
    TEST_ASSERT_TRUE(KeyDerivation::deriveSessionKey(master_key, device_id2, key2).isOk());

    // Keys should differ for different device IDs (at least one byte)
    TEST_ASSERT_FALSE(std::ranges::equal(key1, key2));
}

// Test: Same inputs always produce same output (deterministic)
void test_derive_session_key_deterministic(void) {
    KeyDerivation::MasterKey master_key{};
    for (int i = 0; i < 32; ++i) master_key[i] = 0xAA + i;

    KeyDerivation::DeviceId device_id{0x22, 0x22};
    KeyDerivation::Key key1, key2;

    TEST_ASSERT_TRUE(KeyDerivation::deriveSessionKey(master_key, device_id, key1).isOk());
    TEST_ASSERT_TRUE(KeyDerivation::deriveSessionKey(master_key, device_id, key2).isOk());

    // Should be identical
    TEST_ASSERT_TRUE(std::ranges::equal(key1, key2));
}

// Test: Bilateral key derivation
void test_derive_bilateral_keys(void) {
    KeyDerivation::MasterKey master_key{};
    for (int i = 0; i < 32; ++i) master_key[i] = i * 2;

    KeyDerivation::DeviceId device_id{0x11, 0x22};
    KeyDerivation::DeviceId coupler_id{0x33, 0x44};
    KeyDerivation::Key tx_key, rx_key;

    TEST_ASSERT_TRUE(KeyDerivation::deriveBilateralKeys(master_key, device_id, coupler_id, tx_key, rx_key).isOk());
    TEST_ASSERT_EQUAL(16, tx_key.size());
    TEST_ASSERT_EQUAL(16, rx_key.size());

    // TX and RX keys should be different (at least one byte)
    TEST_ASSERT_FALSE(std::ranges::equal(tx_key, rx_key));
}

// Test: Bilateral keys with swapped device IDs should produce swapped results
void test_derive_bilateral_keys_symmetry(void) {
    KeyDerivation::MasterKey master_key{};
    for (int i = 0; i < 32; ++i) master_key[i] = 0x55 + i;

    KeyDerivation::DeviceId device_id{0xAA, 0xBB};
    KeyDerivation::DeviceId coupler_id{0xCC, 0xDD};

    // Device -> Coupler
    KeyDerivation::Key dev_tx, dev_rx;
    TEST_ASSERT_TRUE(KeyDerivation::deriveBilateralKeys(master_key, device_id, coupler_id, dev_tx, dev_rx).isOk());

    // Coupler -> Device (swapped roles)
    KeyDerivation::Key coup_tx, coup_rx;
    TEST_ASSERT_TRUE(KeyDerivation::deriveBilateralKeys(master_key, coupler_id, device_id, coup_tx, coup_rx).isOk());

    // Device's TX should match Coupler's RX, and vice versa
    TEST_ASSERT_TRUE(std::ranges::equal(dev_tx, coup_rx));
    TEST_ASSERT_TRUE(std::ranges::equal(dev_rx, coup_tx));
}

// Test: Key derivation can seed SecureSession
void test_derive_key_with_secure_session(void) {
    KeyDerivation::MasterKey master_key{};
    for (int i = 0; i < 32; ++i) master_key[i] = i;

    KeyDerivation::DeviceId device_id{0x15, 0x55};
    KeyDerivation::Key session_key;

    TEST_ASSERT_TRUE(KeyDerivation::deriveSessionKey(master_key, device_id, session_key).isOk());

    // Use derived key to create a Data Secure session
    DataSecureSession session(session_key);

    const DataSecureContext ctx{IndividualAddress(0x1101), GroupAddress(0x0F81), 0xE0, 1};
    const std::array<uint8_t, 2> plaintext{0x00, 0x81};
    std::vector<uint8_t> secure(DataSecureSession::protectedTpduSize(plaintext.size()));
    auto secureLen = session.protect(ctx, plaintext, secure);
    TEST_ASSERT_TRUE(secureLen.isOk());
    secure.resize(secureLen.value());
    TEST_ASSERT_TRUE(secure.size() >= (2 + 1 + 6 + 4));

    std::vector<uint8_t> decrypted(DataSecureSession::unprotectedApduSize(secure.size()));
    auto decryptedLen = session.unprotect(ctx, secure, decrypted);
    TEST_ASSERT_TRUE(decryptedLen.isOk());
    decrypted.resize(decryptedLen.value());
    TEST_ASSERT_EQUAL(plaintext.size(), decrypted.size());
    TEST_ASSERT_TRUE(std::ranges::equal(plaintext, decrypted));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_pbkdf2_derivation);
    RUN_TEST(test_pbkdf2_different_salts);
    RUN_TEST(test_pbkdf2_different_passwords);
    RUN_TEST(test_pbkdf2_iterations);
    RUN_TEST(test_derive_session_key);
    RUN_TEST(test_derive_session_key_different_devices);
    RUN_TEST(test_derive_session_key_deterministic);
    RUN_TEST(test_derive_bilateral_keys);
    RUN_TEST(test_derive_bilateral_keys_symmetry);
    RUN_TEST(test_derive_key_with_secure_session);
    return UNITY_END();
}
