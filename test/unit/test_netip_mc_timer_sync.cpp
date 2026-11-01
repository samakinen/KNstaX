// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * The Secure Routing timer synchronisation state machine, 03_08_09 §2.2.2.3.2,
 * walked transition by transition at simulated time.
 *
 * Defaults throughout: latencyTolerance 2000 ms, sync fraction 1Ah, so
 * syncLatencyTolerance = round(2000 * 26 / 255) = 204 ms. The random source
 * answers whatever `g_draw` holds, so a delay is the bottom of its window
 * unless a test says otherwise.
 */

#include "unity.h"

#include "knx/netip/ip_secure/mc_timer_sync.hpp"

#include <cstdint>

using knx::netip::ip_secure::McTimerSync;

namespace {

uint32_t g_draw = 0;

const McTimerSync::Serial kOwnSerial{0x00, 0xFA, 0x01, 0x02, 0x03, 0x04};
const McTimerSync::Serial kPeerSerial{0x00, 0xFA, 0x09, 0x09, 0x09, 0x09};
const McTimerSync::Tag kPeerTag{0xBE, 0xEF};

constexpr uint32_t kSync = 204;
constexpr uint32_t kLatency = 2000;

McTimerSync makeSync()
{
    return McTimerSync(kOwnSerial, []() { return g_draw; });
}

/// Started at t = 0 ms with mc_timer = `timer`, notify due at once.
McTimerSync startedAt(uint64_t timer)
{
    auto sync = makeSync();
    sync.start(0, timer, McTimerSync::StartReason::Reconfigured);
    return sync;
}

/// Drive into SCHED_UPDATE with an outdated TIMER_NOTIFY (E04).
void enterUpdate(McTimerSync& sync, uint32_t now)
{
    const uint64_t local = sync.timer(now);
    sync.onTimerNotify(local - kLatency, kPeerSerial, kPeerTag, now);
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedUpdate);
}

} // namespace

void setUp(void) { g_draw = 0; }
void tearDown(void) {}

void test_sync_latency_tolerance_is_the_scaled_fraction(void)
{
    auto sync = makeSync();
    TEST_ASSERT_EQUAL_UINT32(kLatency, sync.latencyToleranceMs());
    TEST_ASSERT_EQUAL_UINT32(kSync, sync.syncLatencyToleranceMs());
    sync.setLatencyTolerance(5000, 0xFF);
    TEST_ASSERT_EQUAL_UINT32(5000, sync.syncLatencyToleranceMs());
}

void test_delay_windows_follow_the_calculated_parameters(void)
{
    auto sync = startedAt(0);
    // Follower: periodic [10000 + 4s, 10000 + 14s], update [100 + 2s, 100 + 12s].
    TEST_ASSERT_FALSE(sync.isTimeKeeper());
    TEST_ASSERT_EQUAL_UINT32(10000 + 4 * kSync, sync.periodicWindow().minMs);
    TEST_ASSERT_EQUAL_UINT32(10000 + 14 * kSync, sync.periodicWindow().maxMs);
    TEST_ASSERT_EQUAL_UINT32(100 + 2 * kSync, sync.updateWindow().minMs);
    TEST_ASSERT_EQUAL_UINT32(100 + 12 * kSync, sync.updateWindow().maxMs);

    // E10 makes it a keeper (A8): periodic [10000, 10000 + 3s], update [100, 100 + s].
    TEST_ASSERT_TRUE(sync.poll(0).has_value());
    TEST_ASSERT_TRUE(sync.isTimeKeeper());
    TEST_ASSERT_EQUAL_UINT32(10000, sync.periodicWindow().minMs);
    TEST_ASSERT_EQUAL_UINT32(10000 + 3 * kSync, sync.periodicWindow().maxMs);
    TEST_ASSERT_EQUAL_UINT32(100, sync.updateWindow().minMs);
    TEST_ASSERT_EQUAL_UINT32(100 + kSync, sync.updateWindow().maxMs);
}

void test_timer_runs_with_the_clock_and_across_its_wrap(void)
{
    auto sync = makeSync();
    sync.start(0xFFFFFF00u, 1000, McTimerSync::StartReason::Reconfigured);
    TEST_ASSERT_EQUAL_UINT64(1000, sync.timer(0xFFFFFF00u));
    // 0x200 ms later, after the 32-bit clock has wrapped.
    TEST_ASSERT_EQUAL_UINT64(1000 + 0x200, sync.timer(0x00000100u));
}

void test_power_up_notify_is_delayed_randomly_up_to_ten_seconds(void)
{
    g_draw = 7000;
    auto sync = makeSync();
    sync.start(0, 0, McTimerSync::StartReason::PowerUp);
    TEST_ASSERT_EQUAL_UINT32(7000, sync.millisecondsUntilNotify(0));
    TEST_ASSERT_FALSE(sync.poll(6999).has_value());
    TEST_ASSERT_TRUE(sync.poll(7000).has_value());

    g_draw = 0xFFFFFFFFu;  // random(0, 10000) never exceeds 10 s
    auto other = makeSync();
    other.start(0, 0, McTimerSync::StartReason::PowerUp);
    TEST_ASSERT_TRUE(other.millisecondsUntilNotify(0) <= McTimerSync::kMaxDelayInitialNotifyMs);
}

void test_reconfigured_start_notifies_at_once(void)
{
    auto sync = startedAt(0);
    TEST_ASSERT_EQUAL_UINT32(0, sync.millisecondsUntilNotify(0));
}

void test_classification_bands(void)
{
    auto sync = startedAt(100000);
    TEST_ASSERT_TRUE(sync.classify(100001, 0) == McTimerSync::Band::Newer);
    TEST_ASSERT_TRUE(sync.classify(100000, 0) == McTimerSync::Band::InSync);
    TEST_ASSERT_TRUE(sync.classify(100000 - kSync + 1, 0) == McTimerSync::Band::InSync);
    TEST_ASSERT_TRUE(sync.classify(100000 - kSync, 0) == McTimerSync::Band::Late);
    TEST_ASSERT_TRUE(sync.classify(100000 - kLatency + 1, 0) == McTimerSync::Band::Late);
    TEST_ASSERT_TRUE(sync.classify(100000 - kLatency, 0) == McTimerSync::Band::Outdated);

    // A young timer must not underflow into "everything is outdated".
    auto young = startedAt(10);
    TEST_ASSERT_TRUE(young.classify(0, 0) == McTimerSync::Band::InSync);
}

// --- TIMER_NOTIFY, E01–E04 ------------------------------------------------

void test_e01_adopts_newer_timer_and_becomes_follower(void)
{
    auto sync = startedAt(1000);
    (void)sync.poll(0);  // keeper
    sync.onTimerNotify(50000, kPeerSerial, kPeerTag, 0);
    TEST_ASSERT_EQUAL_UINT64(50000, sync.timer(0));
    TEST_ASSERT_FALSE(sync.isTimeKeeper());
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedPeriodic);
    TEST_ASSERT_EQUAL_UINT32(sync.periodicWindow().minMs, sync.millisecondsUntilNotify(0));
}

void test_e01_in_sched_update_cancels_the_update(void)
{
    auto sync = startedAt(100000);
    enterUpdate(sync, 0);
    sync.onTimerNotify(200000, kPeerSerial, kPeerTag, 0);
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedPeriodic);
    TEST_ASSERT_EQUAL_UINT64(200000, sync.timer(0));
}

void test_e02_reschedules_periodic_and_cancels_update(void)
{
    auto sync = startedAt(100000);
    (void)sync.poll(0);
    TEST_ASSERT_TRUE(sync.isTimeKeeper());
    sync.onTimerNotify(100000 - 10, kPeerSerial, kPeerTag, 0);
    TEST_ASSERT_FALSE(sync.isTimeKeeper());  // A9
    TEST_ASSERT_EQUAL_UINT64(100000, sync.timer(0));  // no A1
    TEST_ASSERT_EQUAL_UINT32(sync.periodicWindow().minMs, sync.millisecondsUntilNotify(0));

    enterUpdate(sync, 0);
    sync.onTimerNotify(100000 - 10, kPeerSerial, kPeerTag, 0);
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedPeriodic);
}

void test_e03_does_nothing_in_either_state(void)
{
    auto sync = startedAt(100000);
    const uint32_t before = sync.millisecondsUntilNotify(0);
    sync.onTimerNotify(100000 - kSync - 1, kPeerSerial, kPeerTag, 0);
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedPeriodic);
    TEST_ASSERT_EQUAL_UINT32(before, sync.millisecondsUntilNotify(0));

    enterUpdate(sync, 0);
    const uint32_t pending = sync.millisecondsUntilNotify(0);
    sync.onTimerNotify(100000 - kSync - 1, kPeerSerial, kPeerTag, 0);
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedUpdate);
    TEST_ASSERT_EQUAL_UINT32(pending, sync.millisecondsUntilNotify(0));
}

void test_e04_schedules_an_update_answer_once(void)
{
    auto sync = startedAt(100000);
    sync.onTimerNotify(100000 - kLatency, kPeerSerial, kPeerTag, 0);
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedUpdate);
    TEST_ASSERT_EQUAL_UINT32(sync.updateWindow().minMs, sync.millisecondsUntilNotify(0));

    // A second outdated frame in SCHED_UPDATE is A0: the first answer stands.
    const McTimerSync::Serial otherSerial{1, 1, 1, 1, 1, 1};
    sync.onTimerNotify(5, otherSerial, McTimerSync::Tag{1, 1}, 10);
    const auto notify = sync.poll(sync.updateWindow().minMs);
    TEST_ASSERT_TRUE(notify.has_value());
    TEST_ASSERT_TRUE(notify->update);
    TEST_ASSERT_EQUAL_MEMORY(kPeerSerial.data(), notify->serial.data(), 6);
    TEST_ASSERT_EQUAL_MEMORY(kPeerTag.data(), notify->tag.data(), 2);
}

// --- SECURE_WRAPPER, E05–E08 ----------------------------------------------

void test_e05_accepts_and_adopts_but_a_keeper_stays_keeper(void)
{
    auto sync = startedAt(1000);
    (void)sync.poll(0);
    TEST_ASSERT_TRUE(sync.onSecureWrapper(90000, kPeerSerial, kPeerTag, 0));
    TEST_ASSERT_EQUAL_UINT64(90000, sync.timer(0));
    TEST_ASSERT_TRUE(sync.isTimeKeeper());  // NOTE 2
}

void test_e05_e06_do_not_cancel_a_scheduled_update(void)
{
    auto sync = startedAt(100000);
    enterUpdate(sync, 0);
    TEST_ASSERT_TRUE(sync.onSecureWrapper(200000, kPeerSerial, kPeerTag, 0));
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedUpdate);  // NOTE 3
    TEST_ASSERT_TRUE(sync.onSecureWrapper(200000, kPeerSerial, kPeerTag, 0));
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedUpdate);
}

void test_e06_e07_accept_without_moving_the_timer(void)
{
    auto sync = startedAt(100000);
    TEST_ASSERT_TRUE(sync.onSecureWrapper(100000 - 1, kPeerSerial, kPeerTag, 0));
    TEST_ASSERT_TRUE(sync.onSecureWrapper(100000 - kLatency + 1, kPeerSerial, kPeerTag, 0));
    TEST_ASSERT_EQUAL_UINT64(100000, sync.timer(0));
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedPeriodic);
}

void test_e08_discards_and_schedules_an_update(void)
{
    auto sync = startedAt(100000);
    TEST_ASSERT_FALSE(sync.onSecureWrapper(100000 - kLatency, kPeerSerial, kPeerTag, 0));
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedUpdate);
    // Still discarded in SCHED_UPDATE (A0, and no A2).
    TEST_ASSERT_FALSE(sync.onSecureWrapper(1, kPeerSerial, kPeerTag, 0));
}

void test_replayed_frame_is_discarded_once_the_tolerance_has_passed(void)
{
    auto sync = startedAt(100000);
    // Accepted fresh...
    TEST_ASSERT_TRUE(sync.onSecureWrapper(100000, kPeerSerial, kPeerTag, 0));
    // ...and the same frame replayed 2 s later is not.
    TEST_ASSERT_FALSE(sync.onSecureWrapper(100000, kPeerSerial, kPeerTag, kLatency));
}

// --- E09, E10 -------------------------------------------------------------

void test_e09_reschedules_periodic_only(void)
{
    auto sync = startedAt(100000);
    (void)sync.onTransmit(0);
    TEST_ASSERT_EQUAL_UINT32(sync.periodicWindow().minMs, sync.millisecondsUntilNotify(0));

    enterUpdate(sync, 10);
    const uint32_t pending = sync.millisecondsUntilNotify(10);
    TEST_ASSERT_EQUAL_UINT64(sync.timer(10), sync.onTransmit(10));
    TEST_ASSERT_EQUAL_UINT32(pending, sync.millisecondsUntilNotify(10));
}

void test_e10_periodic_sends_own_serial_and_random_tag(void)
{
    g_draw = 0x1234;
    auto sync = startedAt(4242);
    const auto notify = sync.poll(0);
    TEST_ASSERT_TRUE(notify.has_value());
    TEST_ASSERT_FALSE(notify->update);
    TEST_ASSERT_EQUAL_UINT64(4242, notify->timer);
    TEST_ASSERT_EQUAL_MEMORY(kOwnSerial.data(), notify->serial.data(), 6);
    TEST_ASSERT_EQUAL_UINT8(0x12, notify->tag[0]);
    TEST_ASSERT_EQUAL_UINT8(0x34, notify->tag[1]);
    TEST_ASSERT_TRUE(sync.isTimeKeeper());
    TEST_ASSERT_FALSE(sync.poll(0).has_value());
}

void test_e10_update_answers_with_remembered_identity_and_returns_to_periodic(void)
{
    auto sync = startedAt(100000);
    enterUpdate(sync, 0);
    const auto notify = sync.poll(sync.updateWindow().minMs);
    TEST_ASSERT_TRUE(notify.has_value());
    TEST_ASSERT_TRUE(notify->update);
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedPeriodic);
    TEST_ASSERT_TRUE(sync.isTimeKeeper());
}

// --- E11 ------------------------------------------------------------------

void test_e11_restart_resets_timer_to_zero(void)
{
    auto sync = startedAt(0);
    sync.onTimerNotify(123456789, kPeerSerial, kPeerTag, 0);
    (void)sync.poll(0);
    sync.start(500, 0, McTimerSync::StartReason::Reconfigured);
    TEST_ASSERT_EQUAL_UINT64(0, sync.timer(500));
    TEST_ASSERT_FALSE(sync.isTimeKeeper());
    TEST_ASSERT_TRUE(sync.state() == McTimerSync::State::SchedPeriodic);
    TEST_ASSERT_EQUAL_UINT32(0, sync.millisecondsUntilNotify(500));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sync_latency_tolerance_is_the_scaled_fraction);
    RUN_TEST(test_delay_windows_follow_the_calculated_parameters);
    RUN_TEST(test_timer_runs_with_the_clock_and_across_its_wrap);
    RUN_TEST(test_power_up_notify_is_delayed_randomly_up_to_ten_seconds);
    RUN_TEST(test_reconfigured_start_notifies_at_once);
    RUN_TEST(test_classification_bands);
    RUN_TEST(test_e01_adopts_newer_timer_and_becomes_follower);
    RUN_TEST(test_e01_in_sched_update_cancels_the_update);
    RUN_TEST(test_e02_reschedules_periodic_and_cancels_update);
    RUN_TEST(test_e03_does_nothing_in_either_state);
    RUN_TEST(test_e04_schedules_an_update_answer_once);
    RUN_TEST(test_e05_accepts_and_adopts_but_a_keeper_stays_keeper);
    RUN_TEST(test_e05_e06_do_not_cancel_a_scheduled_update);
    RUN_TEST(test_e06_e07_accept_without_moving_the_timer);
    RUN_TEST(test_e08_discards_and_schedules_an_update);
    RUN_TEST(test_replayed_frame_is_discarded_once_the_tolerance_has_passed);
    RUN_TEST(test_e09_reschedules_periodic_only);
    RUN_TEST(test_e10_periodic_sends_own_serial_and_random_tag);
    RUN_TEST(test_e10_update_answers_with_remembered_identity_and_returns_to_periodic);
    RUN_TEST(test_e11_restart_resets_timer_to_zero);
    return UNITY_END();
}
