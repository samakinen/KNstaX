// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"
#include "knx/security/data_secure.hpp"

#include <array>
#include <vector>

using namespace knx::security;
using knx::GroupAddress;
using knx::IndividualAddress;

// ============================================================================
// KNX Data Secure Known-Answer Tests (MAC-TR)
//
// Ground truth: KNXUltimate implementation + KNX specs as captured in
// external/KNXUltimate/documents/SECURETUNNELINGWORKFLOW.md
//
// These vectors are generated from KNXUltimate primitives (CBC-MAC + CTR) and
// validate byte-for-byte compatibility.
// ============================================================================

namespace {

struct DataSecureVector {
    const char* name;
    DataSecureSession::Key key;
    DataSecureContext ctx;
    std::vector<uint8_t> plainApdu;
    std::vector<uint8_t> expectedSecureTpdu;
};

const DataSecureVector v1{
    "KNXUltimate Vector 1: GroupValueWrite 1-bit",
    {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
     0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF},
    DataSecureContext{IndividualAddress(0x1101), GroupAddress(0x0F81), 0xE0, 1},
    {0x00, 0x81},
    {0x03, 0xF1, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xF8, 0x40, 0x07, 0xC4, 0xC4, 0xE7},
};

const DataSecureVector v2{
    "KNXUltimate Vector 2: GroupValueWrite payload",
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
     0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F},
    DataSecureContext{IndividualAddress(0x1234), GroupAddress(0x2345), 0xA9, 0x000102030405ULL},
    {0x00, 0x80, 0x12, 0x34, 0x56},
    {0x03, 0xF1, 0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x61, 0x03, 0x44, 0x24, 0xB2, 0x9C, 0xF0, 0x65, 0x35},
};

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_data_secure_known_answer_vector_1(void) {
    DataSecureSession s(v1.key);
    std::array<uint8_t, DataSecureSession::kMaxSecureTpduSize> secure{};
    auto secureLen = s.protect(v1.ctx, v1.plainApdu, secure);
    TEST_ASSERT_TRUE(secureLen.isOk());
    const auto secureView = std::span<const uint8_t>(secure.data(), secureLen.value());
    TEST_ASSERT_EQUAL_UINT32(v1.expectedSecureTpdu.size(), secureView.size());
    for (size_t i = 0; i < secureView.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(v1.expectedSecureTpdu[i], secureView[i]);
    }

    std::array<uint8_t, DataSecureSession::kMaxPlainApduSize> plain{};
    auto plainLen = s.unprotect(v1.ctx, secureView, plain);
    TEST_ASSERT_TRUE(plainLen.isOk());
    TEST_ASSERT_EQUAL_UINT32(v1.plainApdu.size(), plainLen.value());
    for (size_t i = 0; i < plainLen.value(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(v1.plainApdu[i], plain[i]);
    }
}

void test_data_secure_known_answer_vector_2(void) {
    DataSecureSession s(v2.key);
    std::array<uint8_t, DataSecureSession::kMaxSecureTpduSize> secure{};
    auto secureLen = s.protect(v2.ctx, v2.plainApdu, secure);
    TEST_ASSERT_TRUE(secureLen.isOk());
    const auto secureView = std::span<const uint8_t>(secure.data(), secureLen.value());
    TEST_ASSERT_EQUAL_UINT32(v2.expectedSecureTpdu.size(), secureView.size());
    for (size_t i = 0; i < secureView.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(v2.expectedSecureTpdu[i], secureView[i]);
    }

    std::array<uint8_t, DataSecureSession::kMaxPlainApduSize> plain{};
    auto plainLen = s.unprotect(v2.ctx, secureView, plain);
    TEST_ASSERT_TRUE(plainLen.isOk());
    TEST_ASSERT_EQUAL_UINT32(v2.plainApdu.size(), plainLen.value());
    for (size_t i = 0; i < plainLen.value(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(v2.plainApdu[i], plain[i]);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_data_secure_known_answer_vector_1);
    RUN_TEST(test_data_secure_known_answer_vector_2);
    return UNITY_END();
}
