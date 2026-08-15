// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_link_monitor.cpp
 * @brief Unit tests for the medium-neutral link-health debounce state machine.
 */

#include "unity.h"
#include "knx/physical/link_monitor.hpp"

using knx::physical::LinkEventKind;
using knx::physical::LinkMonitor;
using knx::physical::LinkSignalConfig;
using knx::physical::LinkState;
using knx::physical::LinkStatus;

void setUp() {}
void tearDown() {}

namespace {

LinkSignalConfig activeHighConfig() {
    LinkSignalConfig config{};
    config.pin = 3;
    config.activeHigh = true;
    config.downDebounceUs = 20000;
    config.upDebounceUs = 200000;
    return config;
}

} // namespace

// An unconfigured monitor is inert: no samples are accepted, state stays
// Unknown, so a backend without the signal changes no behaviour above it.
void test_unconfigured_is_inert() {
    LinkMonitor monitor;
    LinkStatus status{};

    TEST_ASSERT_FALSE(monitor.isConfigured());
    TEST_ASSERT_FALSE(monitor.sample(false, 0, status));
    TEST_ASSERT_FALSE(monitor.sample(true, 1000000, status));
    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Unknown), static_cast<int>(monitor.state()));
}

// The first sample is adopted without debouncing: at init there is no prior
// state to debounce against, and the real level is what should be reported.
void test_first_sample_seeds_immediately() {
    LinkMonitor monitor;
    monitor.configure(activeHighConfig());

    LinkStatus status{};
    TEST_ASSERT_TRUE(monitor.sample(true, 1000, status));
    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Up), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL(static_cast<int>(LinkEventKind::StateChanged), static_cast<int>(status.kind));
    TEST_ASSERT_EQUAL_UINT32(0, status.transitionCount);
    TEST_ASSERT_EQUAL_UINT64(1000, status.timestampUs);
}

// Loss is only reported after the down window has elapsed.
void test_down_requires_debounce_window() {
    LinkMonitor monitor;
    monitor.configure(activeHighConfig());

    LinkStatus status{};
    TEST_ASSERT_TRUE(monitor.sample(true, 0, status));

    TEST_ASSERT_FALSE(monitor.sample(false, 1000, status));
    TEST_ASSERT_TRUE(monitor.hasPendingChange());
    TEST_ASSERT_FALSE(monitor.sample(false, 20000, status));   // 19 ms since candidate
    TEST_ASSERT_TRUE(monitor.sample(false, 21000, status));    // 20 ms → commit

    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Down), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL_UINT32(1, status.transitionCount);
    TEST_ASSERT_FALSE(monitor.hasPendingChange());
}

// A dip that recovers inside the window never reaches the layers above — this
// is the case a bus-voltage sag under heavy traffic produces.
void test_transient_dip_is_filtered() {
    LinkMonitor monitor;
    monitor.configure(activeHighConfig());

    LinkStatus status{};
    TEST_ASSERT_TRUE(monitor.sample(true, 0, status));

    TEST_ASSERT_FALSE(monitor.sample(false, 1000, status));
    TEST_ASSERT_FALSE(monitor.sample(false, 10000, status));
    TEST_ASSERT_FALSE(monitor.sample(true, 15000, status));    // recovered in time

    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Up), static_cast<int>(monitor.state()));
    TEST_ASSERT_EQUAL_UINT32(0, monitor.transitionCount());

    // The window restarts from the recovery, so the next dip needs its own
    // full 20 ms rather than inheriting credit from the previous one.
    TEST_ASSERT_FALSE(monitor.sample(false, 20000, status));
    TEST_ASSERT_FALSE(monitor.sample(false, 39000, status));
    TEST_ASSERT_TRUE(monitor.sample(false, 40000, status));
    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Down), static_cast<int>(status.state));
}

// Recovery uses the longer up window: bus power return is bouncy, and coming
// back too early resumes transmission into a supply that is still settling.
void test_up_uses_its_own_longer_window() {
    LinkMonitor monitor;
    monitor.configure(activeHighConfig());

    LinkStatus status{};
    TEST_ASSERT_TRUE(monitor.sample(false, 0, status));
    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Down), static_cast<int>(status.state));

    TEST_ASSERT_FALSE(monitor.sample(true, 10000, status));
    TEST_ASSERT_FALSE(monitor.sample(true, 100000, status));   // past the down window
    TEST_ASSERT_FALSE(monitor.sample(true, 209000, status));   // still short of 200 ms
    TEST_ASSERT_TRUE(monitor.sample(true, 210000, status));

    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Up), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL_UINT32(1, status.transitionCount);
}

// Polarity is board wiring, not stack policy: an active-low signal must
// produce identical states from inverted levels.
void test_active_low_polarity() {
    LinkSignalConfig config = activeHighConfig();
    config.activeHigh = false;

    LinkMonitor monitor;
    monitor.configure(config);

    TEST_ASSERT_TRUE(monitor.levelMeansUp(0));
    TEST_ASSERT_FALSE(monitor.levelMeansUp(1));

    LinkStatus status{};
    TEST_ASSERT_TRUE(monitor.sample(monitor.levelMeansUp(0), 0, status));
    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Up), static_cast<int>(status.state));

    TEST_ASSERT_FALSE(monitor.sample(monitor.levelMeansUp(1), 1000, status));
    TEST_ASSERT_TRUE(monitor.sample(monitor.levelMeansUp(1), 30000, status));
    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Down), static_cast<int>(status.state));
}

// Flapping is counted, so a consumer can tell "always up" from "recovered".
// Each transition takes two samples: one to observe the new level, a later one
// to find it still there once the window has elapsed. A level cannot be shown
// to have persisted from a single reading.
void test_transition_count_tracks_flapping() {
    LinkMonitor monitor;
    monitor.configure(activeHighConfig());

    LinkStatus status{};
    TEST_ASSERT_TRUE(monitor.sample(true, 0, status));

    TEST_ASSERT_FALSE(monitor.sample(false, 100000, status));
    TEST_ASSERT_TRUE(monitor.sample(false, 130000, status));
    TEST_ASSERT_EQUAL_UINT32(1, status.transitionCount);

    TEST_ASSERT_FALSE(monitor.sample(true, 200000, status));
    TEST_ASSERT_TRUE(monitor.sample(true, 500000, status));
    TEST_ASSERT_EQUAL_UINT32(2, status.transitionCount);

    TEST_ASSERT_FALSE(monitor.sample(false, 600000, status));
    TEST_ASSERT_TRUE(monitor.sample(false, 700000, status));
    TEST_ASSERT_EQUAL_UINT32(3, status.transitionCount);
}

// reset() returns the monitor to "never sampled" while keeping the config, so
// the next sample seeds again instead of reporting a spurious transition.
void test_reset_reseeds() {
    LinkMonitor monitor;
    monitor.configure(activeHighConfig());

    LinkStatus status{};
    TEST_ASSERT_TRUE(monitor.sample(true, 0, status));
    TEST_ASSERT_FALSE(monitor.sample(false, 100000, status));
    TEST_ASSERT_TRUE(monitor.sample(false, 130000, status));
    TEST_ASSERT_EQUAL_UINT32(1, monitor.transitionCount());

    monitor.reset();
    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Unknown), static_cast<int>(monitor.state()));
    TEST_ASSERT_EQUAL_UINT32(0, monitor.transitionCount());

    TEST_ASSERT_TRUE(monitor.sample(false, 200000, status));
    TEST_ASSERT_EQUAL(static_cast<int>(LinkState::Down), static_cast<int>(status.state));
    TEST_ASSERT_EQUAL_UINT32(0, status.transitionCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unconfigured_is_inert);
    RUN_TEST(test_first_sample_seeds_immediately);
    RUN_TEST(test_down_requires_debounce_window);
    RUN_TEST(test_transient_dip_is_filtered);
    RUN_TEST(test_up_uses_its_own_longer_window);
    RUN_TEST(test_active_low_polarity);
    RUN_TEST(test_transition_count_tracks_flapping);
    RUN_TEST(test_reset_reseeds);
    return UNITY_END();
}
