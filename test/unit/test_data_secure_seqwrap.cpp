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

void test_data_secure_sequence_wrap_blocked(void) {
    DataSecureSession::Key key{};
    key.fill(0x2A);

    DataSecureSession sess(key);

    // Set send sequence to max 48-bit value so next increment would wrap to zero
    const uint64_t max48 = 0x0000FFFFFFFFFFFFULL;
    sess.setSendSeq(max48);

    DataSecureContext ctx{};
    ctx.srcIa = IndividualAddress(0x0101);
    ctx.dstAddr = GroupAddress(0x0203);
    ctx.cemiCtrl2 = 0x80;

    std::vector<uint8_t> plain = {0x80, 0x11, 0x22};
    std::array<uint8_t, DataSecureSession::kMaxSecureTpduSize> secure{};

    // Protect should fail because next sequence would wrap to zero
    TEST_ASSERT_TRUE(sess.protect(ctx, plain, secure).isError());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_data_secure_sequence_wrap_blocked);
    return UNITY_END();
}

#else

int main() { return 0; }

#endif
