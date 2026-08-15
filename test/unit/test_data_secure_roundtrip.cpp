// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#if KNX_SECURE_ENABLED

#include "knx/security/data_secure.hpp"
#include "unity.h"

#include <array>
#include <vector>
#include <span>

using namespace knx::security;
using knx::GroupAddress;
using knx::IndividualAddress;

void setUp(void) {}
void tearDown(void) {}

static uint64_t readU48BE(std::span<const uint8_t> v, size_t off) {
    uint64_t r = 0;
    for (size_t i = 0; i < 6; ++i) {
        r = (r << 8) | v[off + i];
    }
    return r;
}

void test_data_secure_roundtrip(void) {
    DataSecureSession::Key key{};
    key.fill(0x2A);

    DataSecureSession sess(key);

    DataSecureContext ctx{};
    ctx.srcIa = IndividualAddress(0x0101);
    ctx.dstAddr = GroupAddress(0x0203);
    ctx.cemiCtrl2 = 0x80;
    ctx.seq48 = 0x00000102030405ULL; // 48-bit sequence

    std::vector<uint8_t> plain = {0x80, 0x11, 0x22, 0x33};

    std::array<uint8_t, DataSecureSession::kMaxSecureTpduSize> secure{};
    auto secureLen = sess.protect(ctx, plain, secure);
    TEST_ASSERT_TRUE(secureLen.isOk());
    const auto secureView = std::span<const uint8_t>(secure.data(), secureLen.value());

    // Secure TPDU must contain APCI_SEC header and embedded sequence
    TEST_ASSERT_TRUE(secureView.size() >= 8);
    TEST_ASSERT_EQUAL_UINT8(DataSecureSession::APCI_SEC_HIGH, secureView[0]);
    TEST_ASSERT_EQUAL_UINT8(DataSecureSession::APCI_SEC_LOW, secureView[1]);

    // Sequence number is 6 bytes big-endian starting at index 3 (APCI_SEC, APCI_SEC, SCF, seq48[0..5])
    uint64_t seqFromTpdu = readU48BE(secureView, 3);
    TEST_ASSERT_EQUAL_UINT64(ctx.seq48 & 0x0000FFFFFFFFFFFFULL, seqFromTpdu);

    std::array<uint8_t, DataSecureSession::kMaxPlainApduSize> recovered{};
    auto recoveredLen = sess.unprotect(ctx, secureView, recovered);
    TEST_ASSERT_TRUE(recoveredLen.isOk());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)plain.size(), (uint32_t)recoveredLen.value());
    for (size_t i = 0; i < plain.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(plain[i], recovered[i]);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_data_secure_roundtrip);
    return UNITY_END();
}

#else

int main() { return 0; }

#endif
