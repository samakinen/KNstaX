// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * KNXnet/IP Routing egress pacing and ROUTING_BUSY back-off.
 *
 * The rules being checked are 03/02/06 §2.1 (at most 50 datagrams per second,
 * never two inside 5 ms) and 03/08/05 §4.3 (stop on ROUTING_BUSY for tw, and
 * when notifications overlap take the longer wait). All of it is millisecond
 * arithmetic against a supplied clock, so the tests run at simulated time and
 * a one-second budget costs no wall time to exhaust.
 */

#include "unity.h"

#include "knx/netip/routing_flow_control.hpp"

using knx::netip::RoutingBusyInfo;
using knx::netip::RoutingFlowControl;

void setUp(void) {}
void tearDown(void) {}

namespace {

RoutingFlowControl::Config gapOnly()
{
    // Isolates the spacing rule from the rate rule, so a failure names one.
    return RoutingFlowControl::Config{
        .maxDatagramsPerSecond = 0,
        .minSendGapMs = 5,
        .randomBackoffSlotMs = 0,
    };
}

RoutingFlowControl::Config rateOnly()
{
    return RoutingFlowControl::Config{
        .maxDatagramsPerSecond = 50,
        .minSendGapMs = 0,
        .randomBackoffSlotMs = 0,
    };
}

RoutingFlowControl::Config deterministic()
{
    return RoutingFlowControl::Config{
        .maxDatagramsPerSecond = 50,
        .minSendGapMs = 5,
        .randomBackoffSlotMs = 0,
    };
}

} // namespace

void test_first_datagram_is_always_allowed(void)
{
    RoutingFlowControl fc(deterministic());
    TEST_ASSERT_TRUE(fc.canSend(0));
    TEST_ASSERT_EQUAL_UINT32(0, fc.millisecondsUntilSendable(0));
}

void test_minimum_gap_between_datagrams_is_enforced(void)
{
    RoutingFlowControl fc(gapOnly());

    fc.noteSent(1000);
    TEST_ASSERT_FALSE(fc.canSend(1000));
    TEST_ASSERT_FALSE(fc.canSend(1004));
    TEST_ASSERT_EQUAL_UINT32(1, fc.millisecondsUntilSendable(1004));
    // "SHALL always pause ... for at least 5 ms" — the boundary is allowed.
    TEST_ASSERT_TRUE(fc.canSend(1005));
}

void test_fifty_datagrams_per_second_is_a_ceiling(void)
{
    RoutingFlowControl fc(rateOnly());

    for (int i = 0; i < 50; ++i) {
        TEST_ASSERT_TRUE(fc.canSend(static_cast<uint32_t>(i)));
        fc.noteSent(static_cast<uint32_t>(i));
    }

    // The 51st inside the same second must wait for the window to roll.
    TEST_ASSERT_FALSE(fc.canSend(60));
    TEST_ASSERT_EQUAL_UINT32(940, fc.millisecondsUntilSendable(60));
    TEST_ASSERT_TRUE(fc.canSend(1000));
}

void test_budget_refills_after_the_window(void)
{
    RoutingFlowControl fc(rateOnly());

    for (int i = 0; i < 50; ++i) fc.noteSent(0);
    TEST_ASSERT_FALSE(fc.canSend(999));

    fc.noteSent(1000);
    // The window restarted with this send, so 49 remain rather than none.
    for (int i = 0; i < 49; ++i) {
        TEST_ASSERT_TRUE(fc.canSend(1000));
        fc.noteSent(1000);
    }
    TEST_ASSERT_FALSE(fc.canSend(1000));
}

void test_routing_busy_stops_sending_for_the_wait_time(void)
{
    RoutingFlowControl fc(deterministic());

    const RoutingBusyInfo busy{.deviceState = 0, .waitTimeMs = 100, .controlField = 0x0000};
    fc.noteBusyReceived(busy, 1000);

    TEST_ASSERT_TRUE(fc.isBusyBackoffActive(1000));
    TEST_ASSERT_FALSE(fc.canSend(1099));
    TEST_ASSERT_EQUAL_UINT32(1, fc.millisecondsUntilSendable(1099));
    TEST_ASSERT_TRUE(fc.canSend(1100));
}

void test_overlapping_busy_takes_the_longer_wait(void)
{
    RoutingFlowControl fc(deterministic());

    fc.noteBusyReceived(RoutingBusyInfo{.deviceState = 0, .waitTimeMs = 100, .controlField = 0}, 1000);
    // A shorter wait arriving mid-back-off must not shorten it: §4.3 resolves
    // the overlap by the higher value, so a chatty device cannot cut in.
    fc.noteBusyReceived(RoutingBusyInfo{.deviceState = 0, .waitTimeMs = 20, .controlField = 0}, 1050);
    TEST_ASSERT_FALSE(fc.canSend(1099));
    TEST_ASSERT_TRUE(fc.canSend(1100));

    // A longer one does extend it.
    fc.noteBusyReceived(RoutingBusyInfo{.deviceState = 0, .waitTimeMs = 100, .controlField = 0}, 1150);
    TEST_ASSERT_FALSE(fc.canSend(1249));
    TEST_ASSERT_TRUE(fc.canSend(1250));
}

void test_busy_with_unknown_control_field_still_stops_sending(void)
{
    RoutingFlowControl fc(deterministic());

    // "any device that does not interpret this routing busy control field SHALL
    // stop sending for the time tw" — a non-zero field is not an exemption.
    fc.noteBusyReceived(RoutingBusyInfo{.deviceState = 0, .waitTimeMs = 50, .controlField = 0x1234}, 500);
    TEST_ASSERT_FALSE(fc.canSend(549));
    TEST_ASSERT_TRUE(fc.canSend(550));
}

void test_busy_counter_counts_independent_notifications(void)
{
    RoutingFlowControl fc(deterministic());
    const RoutingBusyInfo busy{.deviceState = 0, .waitTimeMs = 20, .controlField = 0};

    fc.noteBusyReceived(busy, 1000);
    TEST_ASSERT_EQUAL_UINT32(1, fc.busyCounter(1000));

    // Inside the 10 ms holdoff these are several devices reacting to the same
    // congestion, not several rounds of it, so N does not climb.
    fc.noteBusyReceived(busy, 1005);
    TEST_ASSERT_EQUAL_UINT32(1, fc.busyCounter(1005));

    fc.noteBusyReceived(busy, 1015);
    TEST_ASSERT_EQUAL_UINT32(2, fc.busyCounter(1015));
}

void test_busy_counter_decays_after_slow_duration(void)
{
    RoutingFlowControl fc(deterministic());
    const RoutingBusyInfo busy{.deviceState = 0, .waitTimeMs = 20, .controlField = 0};

    fc.noteBusyReceived(busy, 0);
    fc.noteBusyReceived(busy, 20);
    TEST_ASSERT_EQUAL_UINT32(2, fc.busyCounter(20));

    // tslowduration = N * 100 ms from the last notification; nothing decays
    // before it elapses.
    TEST_ASSERT_EQUAL_UINT32(2, fc.busyCounter(219));
    // Then one step of N every tbd = 5 ms.
    TEST_ASSERT_EQUAL_UINT32(1, fc.busyCounter(225));
    TEST_ASSERT_EQUAL_UINT32(0, fc.busyCounter(230));
    TEST_ASSERT_EQUAL_UINT32(0, fc.busyCounter(10000));
}

void test_random_backoff_extends_the_wait_within_bounds(void)
{
    RoutingFlowControl::Config config{
        .maxDatagramsPerSecond = 50,
        .minSendGapMs = 5,
        .randomBackoffSlotMs = 50,
    };

    // trandom = random * N * 50 ms, and with N = 1 a full draw adds 50 ms.
    RoutingFlowControl high(config);
    high.noteBusyReceived(RoutingBusyInfo{.deviceState = 0, .waitTimeMs = 100, .controlField = 0}, 0, 1.0f);
    TEST_ASSERT_FALSE(high.canSend(149));
    TEST_ASSERT_TRUE(high.canSend(150));

    // A zero draw is the plain tw, which is the floor the spec mandates.
    RoutingFlowControl low(config);
    low.noteBusyReceived(RoutingBusyInfo{.deviceState = 0, .waitTimeMs = 100, .controlField = 0}, 0, 0.0f);
    TEST_ASSERT_TRUE(low.canSend(100));
}

void test_deadlines_survive_the_clock_wrapping(void)
{
    RoutingFlowControl fc(gapOnly());

    // A 32-bit millisecond clock wraps every 49.7 days. Comparing raw values
    // rather than differences turns "5 ms from now" into "49 days from now"
    // exactly once per wrap, and the device stops transmitting until reboot.
    const uint32_t justBeforeWrap = 0xFFFFFFFEu;
    fc.noteSent(justBeforeWrap);
    TEST_ASSERT_FALSE(fc.canSend(justBeforeWrap + 1));  // wraps to 0xFFFFFFFF
    TEST_ASSERT_FALSE(fc.canSend(2));                   // wrapped past zero
    TEST_ASSERT_TRUE(fc.canSend(3));                    // 5 ms after the send
}

void test_reset_clears_pacing_and_backoff(void)
{
    RoutingFlowControl fc(deterministic());
    fc.noteSent(100);
    fc.noteBusyReceived(RoutingBusyInfo{.deviceState = 0, .waitTimeMs = 100, .controlField = 0}, 100);
    TEST_ASSERT_FALSE(fc.canSend(120));

    fc.reset();
    TEST_ASSERT_TRUE(fc.canSend(120));
    TEST_ASSERT_EQUAL_UINT32(0, fc.busyCounter(120));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_first_datagram_is_always_allowed);
    RUN_TEST(test_minimum_gap_between_datagrams_is_enforced);
    RUN_TEST(test_fifty_datagrams_per_second_is_a_ceiling);
    RUN_TEST(test_budget_refills_after_the_window);
    RUN_TEST(test_routing_busy_stops_sending_for_the_wait_time);
    RUN_TEST(test_overlapping_busy_takes_the_longer_wait);
    RUN_TEST(test_busy_with_unknown_control_field_still_stops_sending);
    RUN_TEST(test_busy_counter_counts_independent_notifications);
    RUN_TEST(test_busy_counter_decays_after_slow_duration);
    RUN_TEST(test_random_backoff_extends_the_wait_within_bounds);
    RUN_TEST(test_deadlines_survive_the_clock_wrapping);
    RUN_TEST(test_reset_clears_pacing_and_backoff);
    return UNITY_END();
}
