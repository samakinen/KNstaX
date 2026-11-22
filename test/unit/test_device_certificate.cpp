// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_device_certificate.cpp
 * @brief The device certificate ETS reads a Factory Tool Key from
 *
 * The expected strings come from a separate implementation of the published
 * layout (a table-driven CRC-4 and a byte-buffered Base32 encoder), so they
 * check the bit order and the CRC of security/device_certificate.hpp, not only
 * its consistency with itself.
 */

#include "unity.h"
#include "knx/security/device_certificate.hpp"

#include <array>
#include <cstdint>
#include <cstring>

using knx::security::formatDeviceCertificate;

namespace {

void expectCertificate(const std::array<uint8_t, 6>& serial, const std::array<uint8_t, 16>& fdsk,
                       const char* expected)
{
    const auto text = formatDeviceCertificate(serial, fdsk);
    TEST_ASSERT_EQUAL_STRING(expected, text.data());
}

constexpr std::array<uint8_t, 6> kZeroSerial{};
constexpr std::array<uint8_t, 16> kZeroKey{};
static_assert(formatDeviceCertificate(kZeroSerial, kZeroKey)[0] == 'A', "usable in constant expressions");

}  // namespace

void setUp(void) {}
void tearDown(void) {}

void test_certificate_of_a_counting_key(void) {
    expectCertificate({0x00, 0xFA, 0x12, 0x34, 0x56, 0x78},
                      {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                       0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F},
                      "AD5BEN-CWPAAA-CAQDAQ-CQMBYI-BEFAWD-ANBYH6");
}

void test_certificate_of_zeros_and_of_ones(void) {
    expectCertificate(kZeroSerial, kZeroKey, "AAAAAA-AAAAAA-AAAAAA-AAAAAA-AAAAAA-AAAAAA");
    std::array<uint8_t, 6> serial{};
    serial.fill(0xFF);
    std::array<uint8_t, 16> key{};
    key.fill(0xFF);
    expectCertificate(serial, key, "777777-777777-777777-777777-777777-777777");
}

void test_certificate_of_a_mixed_key(void) {
    expectCertificate({0x00, 0xFA, 0x01, 0x02, 0x03, 0x04},
                      {0xA0, 0x1B, 0xC2, 0x3D, 0xE4, 0x5F, 0x60, 0x71,
                       0x82, 0x93, 0xA4, 0xB5, 0xC6, 0xD7, 0xE8, 0xF9},
                      "AD5ACA-QDASQB-XQR54R-PWA4MC-SOSLLR-WX5D4V");
}

void test_crc4_of_the_published_examples(void) {
    const std::array<uint8_t, 22> counting{0x00, 0xFA, 0x12, 0x34, 0x56, 0x78, 0x00, 0x01, 0x02, 0x03, 0x04,
                                           0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    TEST_ASSERT_EQUAL_UINT8(14, knx::security::deviceCertificateCrc4(counting));
    std::array<uint8_t, 22> ones{};
    ones.fill(0xFF);
    TEST_ASSERT_EQUAL_UINT8(15, knx::security::deviceCertificateCrc4(ones));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_certificate_of_a_counting_key);
    RUN_TEST(test_certificate_of_zeros_and_of_ones);
    RUN_TEST(test_certificate_of_a_mixed_key);
    RUN_TEST(test_crc4_of_the_published_examples);
    return UNITY_END();
}
