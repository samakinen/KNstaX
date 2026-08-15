// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_group_object_transmit_policy.cpp
 * @brief Unit tests for GroupObject send-on-change / cyclic / min-interval policy.
 */

#include "unity.h"
#include "knx/application/group_object.hpp"

using namespace knx;
using namespace knx::application;

namespace {

GroupObjectConfig makeConfig(DptId dpt, const GroupObjectTransmitPolicy& policy) {
    GroupObjectConfig config{};
    config.address = GroupAddress(1, 2, 3);
    config.dpt = dpt;
    config.flags.read = true;
    config.flags.write = false;
    config.flags.transmit = true;
    config.flags.update = false;
    config.transmitPolicy = policy;
    return config;
}

// Simulate a publish: store the value, decide, and (on Send) mark transmitted.
PublishDecision publish(GroupObject& go, float value, uint32_t nowMs) {
    (void)go.setValue(value);
    const auto decision = go.decidePublish(nowMs);
    if (decision == PublishDecision::Send) {
        go.noteTransmitted(nowMs);
    }
    return decision;
}

PublishDecision publish(GroupObject& go, bool value, uint32_t nowMs) {
    (void)go.setValue(value);
    const auto decision = go.decidePublish(nowMs);
    if (decision == PublishDecision::Send) {
        go.noteTransmitted(nowMs);
    }
    return decision;
}

} // namespace

void setUp() {}
void tearDown() {}

// Default policy is inert: every publish is sent, matching legacy behaviour.
void test_default_policy_always_sends() {
    GroupObject go(makeConfig(dptids::Temperature, {}));
    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, 20.0f, 0));
    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, 20.0f, 10));   // unchanged still sends
    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, 21.0f, 20));
}

// Send-on-change suppresses sub-threshold updates, sends threshold-crossing ones.
void test_on_change_threshold() {
    GroupObjectTransmitPolicy policy{};
    policy.onChangeEnabled = true;
    policy.changeThreshold = 0.5;  // 0.5 °C
    GroupObject go(makeConfig(dptids::Temperature, policy));

    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, 20.0f, 0));       // first always sends
    TEST_ASSERT_EQUAL(PublishDecision::Suppress, publish(go, 20.1f, 10));  // +0.1 < 0.5 → drop
    TEST_ASSERT_EQUAL(PublishDecision::Suppress, publish(go, 20.4f, 20));  // still < 0.5 from 20.0
    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, 22.0f, 30));      // +2.0 → send
    // Baseline is now 22.0; a small drop is suppressed again.
    TEST_ASSERT_EQUAL(PublishDecision::Suppress, publish(go, 21.8f, 40));
}

// Any-change semantics (threshold 0) for a boolean object.
void test_on_change_bool_any_change() {
    GroupObjectTransmitPolicy policy{};
    policy.onChangeEnabled = true;  // threshold 0 → any change
    GroupObject go(makeConfig(dptids::Bool, policy));

    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, false, 0));
    TEST_ASSERT_EQUAL(PublishDecision::Suppress, publish(go, false, 10)); // unchanged
    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, true, 20));      // flipped
    TEST_ASSERT_EQUAL(PublishDecision::Suppress, publish(go, true, 30));
}

// Cyclic heartbeat becomes due once the interval elapses since the last send.
void test_cyclic_heartbeat() {
    GroupObjectTransmitPolicy policy{};
    policy.cyclicIntervalMs = 1000;
    GroupObject go(makeConfig(dptids::Temperature, policy));

    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, 20.0f, 0));
    TEST_ASSERT_FALSE(go.dueForCyclic(999));    // not yet
    TEST_ASSERT_TRUE(go.dueForCyclic(1000));    // interval elapsed
    TEST_ASSERT_TRUE(go.dueForCyclic(5000));

    // A real send resets the cyclic timer.
    go.noteTransmitted(1000);
    TEST_ASSERT_FALSE(go.dueForCyclic(1500));
    TEST_ASSERT_TRUE(go.dueForCyclic(2000));
}

// A publish inside the min-interval floor defers; the pump releases it later.
void test_min_interval_defers() {
    GroupObjectTransmitPolicy policy{};
    policy.minIntervalMs = 500;
    GroupObject go(makeConfig(dptids::Temperature, policy));

    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, 20.0f, 0));

    // 100 ms later — too soon.
    (void)go.setValue(20.5f);
    TEST_ASSERT_EQUAL(PublishDecision::Defer, go.decidePublish(100));
    go.setPendingSend(true);

    TEST_ASSERT_FALSE(go.readyToSendDeferred(400));  // still inside the floor
    TEST_ASSERT_TRUE(go.readyToSendDeferred(500));   // floor elapsed → releasable

    // After the deferred send goes out, the flag clears.
    go.noteTransmitted(500);
    TEST_ASSERT_FALSE(go.pendingSend());
    TEST_ASSERT_FALSE(go.readyToSendDeferred(1000));
}

// Combined send-on-change + cyclic: unchanged values are suppressed on publish
// but the heartbeat still forces a periodic re-send.
void test_change_plus_cyclic() {
    GroupObjectTransmitPolicy policy{};
    policy.onChangeEnabled = true;
    policy.changeThreshold = 0.5;
    policy.cyclicIntervalMs = 1000;
    GroupObject go(makeConfig(dptids::Temperature, policy));

    TEST_ASSERT_EQUAL(PublishDecision::Send, publish(go, 20.0f, 0));
    TEST_ASSERT_EQUAL(PublishDecision::Suppress, publish(go, 20.1f, 100)); // no change → drop
    TEST_ASSERT_TRUE(go.dueForCyclic(1000));                               // but heartbeat is due
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_default_policy_always_sends);
    RUN_TEST(test_on_change_threshold);
    RUN_TEST(test_on_change_bool_any_change);
    RUN_TEST(test_cyclic_heartbeat);
    RUN_TEST(test_min_interval_defers);
    RUN_TEST(test_change_plus_cyclic);
    return UNITY_END();
}
