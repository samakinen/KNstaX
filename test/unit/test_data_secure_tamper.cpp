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

void test_data_secure_tamper_detection(void) {
    DataSecureSession::Key key{};
    key.fill(0x2A);

    DataSecureSession sess(key);

    DataSecureContext ctx{};
    ctx.srcIa = IndividualAddress(0x0101);
    ctx.dstAddr = GroupAddress(0x0203);
    ctx.cemiCtrl2 = 0x80;
    ctx.seq48 = 0x00000000000007ULL;

    std::vector<uint8_t> plain = {0x80, 0x11, 0x22, 0x33};
    std::array<uint8_t, DataSecureSession::kMaxSecureTpduSize> secure{};
    auto secureLen = sess.protect(ctx, plain, secure);
    TEST_ASSERT_TRUE(secureLen.isOk());
    auto secureView = std::span<uint8_t>(secure.data(), secureLen.value());

    // Tamper with a payload byte (flip one bit)
    if (secureView.size() > 10) secureView[10] ^= 0x01;

    std::array<uint8_t, DataSecureSession::kMaxPlainApduSize> recovered{};
    // Unprotect must fail due to MAC mismatch
    TEST_ASSERT_TRUE(sess.unprotect(ctx, secureView, recovered).isError());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_data_secure_tamper_detection);
    return UNITY_END();
}

#else

int main() { return 0; }

#endif
