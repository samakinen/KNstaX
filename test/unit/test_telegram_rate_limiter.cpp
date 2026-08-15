// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_telegram_rate_limiter.cpp
 * @brief Unit tests for the reusable outbound telegram rate limiter.
 */

#include "unity.h"
#include "knx/application/telegram_rate_limiter.hpp"

using knx::application::TelegramRateLimiter;
using knx::application::TelegramRateLimitConfig;

void setUp() {}
void tearDown() {}

// Default (unconfigured) limiter never blocks.
void test_unconfigured_allows_everything() {
    TelegramRateLimiter limiter;
    TEST_ASSERT_TRUE(limiter.unlimited());
    for (uint32_t t = 0; t < 1000; t += 1) {
        TEST_ASSERT_TRUE(limiter.tryConsume(t));
    }
}

// minGapMs enforces spacing between consecutive sends.
void test_min_gap_spacing() {
    TelegramRateLimiter limiter(TelegramRateLimitConfig{.maxTelegrams = 0, .perWindowMs = 1000, .minGapMs = 100});

    TEST_ASSERT_TRUE(limiter.tryConsume(0));      // first send always ok
    TEST_ASSERT_FALSE(limiter.tryConsume(50));    // 50 ms < 100 ms gap
    TEST_ASSERT_FALSE(limiter.tryConsume(99));    // still too soon
    TEST_ASSERT_TRUE(limiter.tryConsume(100));    // exactly the gap → ok
    TEST_ASSERT_FALSE(limiter.tryConsume(150));   // too soon after the 100 ms send
    TEST_ASSERT_TRUE(limiter.tryConsume(200));
}

// Token bucket caps the count within a window and refills after it.
void test_token_bucket_window() {
    TelegramRateLimiter limiter(TelegramRateLimitConfig{.maxTelegrams = 3, .perWindowMs = 1000, .minGapMs = 0});

    TEST_ASSERT_TRUE(limiter.tryConsume(0));
    TEST_ASSERT_TRUE(limiter.tryConsume(10));
    TEST_ASSERT_TRUE(limiter.tryConsume(20));     // 3 tokens spent
    TEST_ASSERT_FALSE(limiter.tryConsume(30));    // bucket empty
    TEST_ASSERT_FALSE(limiter.tryConsume(999));   // still in the window
    TEST_ASSERT_TRUE(limiter.tryConsume(1000));   // window elapsed → refilled
    TEST_ASSERT_TRUE(limiter.tryConsume(1001));
}

// A blocked tryConsume must not consume a token (state is unchanged on refusal).
void test_blocked_consume_is_side_effect_free() {
    TelegramRateLimiter limiter(TelegramRateLimitConfig{.maxTelegrams = 1, .perWindowMs = 1000, .minGapMs = 0});
    TEST_ASSERT_TRUE(limiter.tryConsume(0));
    // Many refusals in the same window...
    for (uint32_t t = 1; t < 1000; ++t) {
        TEST_ASSERT_FALSE(limiter.tryConsume(t));
    }
    // ...then the very next window still grants exactly one.
    TEST_ASSERT_TRUE(limiter.tryConsume(1000));
    TEST_ASSERT_FALSE(limiter.tryConsume(1500));
}

// allowed() is a non-consuming peek consistent with tryConsume().
void test_allowed_is_non_consuming() {
    TelegramRateLimiter limiter(TelegramRateLimitConfig{.maxTelegrams = 1, .perWindowMs = 1000, .minGapMs = 0});
    TEST_ASSERT_TRUE(limiter.allowed(0));
    TEST_ASSERT_TRUE(limiter.allowed(0));   // peeking twice does not consume
    TEST_ASSERT_TRUE(limiter.tryConsume(0));
    TEST_ASSERT_FALSE(limiter.allowed(10));
}

// Combined min-gap AND token bucket: both constraints apply.
void test_combined_constraints() {
    TelegramRateLimiter limiter(TelegramRateLimitConfig{.maxTelegrams = 5, .perWindowMs = 1000, .minGapMs = 100});
    TEST_ASSERT_TRUE(limiter.tryConsume(0));
    TEST_ASSERT_FALSE(limiter.tryConsume(50));   // token available but gap not met
    TEST_ASSERT_TRUE(limiter.tryConsume(100));
    TEST_ASSERT_TRUE(limiter.tryConsume(250));
}

// reset() clears history.
void test_reset_clears_history() {
    TelegramRateLimiter limiter(TelegramRateLimitConfig{.maxTelegrams = 1, .perWindowMs = 1000, .minGapMs = 0});
    TEST_ASSERT_TRUE(limiter.tryConsume(0));
    TEST_ASSERT_FALSE(limiter.tryConsume(10));
    limiter.reset();
    TEST_ASSERT_TRUE(limiter.tryConsume(10));    // fresh bucket after reset
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unconfigured_allows_everything);
    RUN_TEST(test_min_gap_spacing);
    RUN_TEST(test_token_bucket_window);
    RUN_TEST(test_blocked_consume_is_side_effect_free);
    RUN_TEST(test_allowed_is_non_consuming);
    RUN_TEST(test_combined_constraints);
    RUN_TEST(test_reset_clears_history);
    return UNITY_END();
}
