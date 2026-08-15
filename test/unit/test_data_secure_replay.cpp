// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#if KNX_SECURE_ENABLED

#include "knx/security/data_secure.hpp"
#include "unity.h"

#include <array>
#include <vector>

using namespace knx::security;
using knx::GroupAddress;
using knx::IndividualAddress;

void setUp(void) {}
void tearDown(void) {}

void test_data_secure_replay_detection(void) {
    DataSecureSession::Key key{};
    key.fill(0x2A);

    DataSecureSession protector(key);
    DataSecureSession verifier(key);

    DataSecureContext ctx{};
    ctx.srcIa = IndividualAddress(0x0101);
    ctx.dstAddr = GroupAddress(0x0203);
    ctx.cemiCtrl2 = 0x80;
    ctx.seq48 = 0x00000000000005ULL; // explicit non-zero sequence

    std::vector<uint8_t> plain = {0x80, 0x11, 0x22};
    std::array<uint8_t, DataSecureSession::kMaxSecureTpduSize> secure{};

    // Protector creates a secure TPDU
    auto secureLen = protector.protect(ctx, plain, secure);
    TEST_ASSERT_TRUE(secureLen.isOk());
    const auto secureView = std::span<const uint8_t>(secure.data(), secureLen.value());

    // First unprotect by verifier should succeed
    std::array<uint8_t, DataSecureSession::kMaxPlainApduSize> recovered{};
    auto recoveredLen = verifier.unprotect(ctx, secureView, recovered);
    TEST_ASSERT_TRUE(recoveredLen.isOk());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)plain.size(), (uint32_t)recoveredLen.value());

    // Replay: same secure TPDU again should be rejected
    std::array<uint8_t, DataSecureSession::kMaxPlainApduSize> recovered2{};
    TEST_ASSERT_TRUE(verifier.unprotect(ctx, secureView, recovered2).isError());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_data_secure_replay_detection);
    return UNITY_END();
}

#else

int main() { return 0; }

#endif
