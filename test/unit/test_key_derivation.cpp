// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"
#include "knx/security/key_derivation.hpp"

#include <vector>
#include <ranges>
#include <algorithm>
#include <array>
#include <cstring>
#include <span>

using namespace knx::security;

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

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_pbkdf2_derivation);
    RUN_TEST(test_pbkdf2_different_salts);
    RUN_TEST(test_pbkdf2_different_passwords);
    RUN_TEST(test_pbkdf2_iterations);
    return UNITY_END();
}
