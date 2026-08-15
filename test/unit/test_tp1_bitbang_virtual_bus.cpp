// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#define private public
#include "knx/physical/bitbang_driver_timer_isr.hpp"
#undef private

#include "knx/physical/timer_gpio_hal_virtual.hpp"

#include <vector>

using namespace knx::physical;

namespace {

BitBangDriverTimerIsr driver;
TimerGpioHalVirtualBus virtualBus;
knx_timer_gpio_hal_t hal{};

void assertTransitionTimestampWithin(const TimerGpioHalVirtualBus::TxTransition& transition,
                                     uint64_t expectedUs,
                                     uint64_t toleranceUs)
{
    const uint64_t lower = (expectedUs > toleranceUs) ? (expectedUs - toleranceUs) : 0;
    const uint64_t upper = expectedUs + toleranceUs;
    TEST_ASSERT_TRUE(transition.timestampUs >= lower);
    TEST_ASSERT_TRUE(transition.timestampUs <= upper);
}

} // namespace

void setUp()
{
    virtualBus.reset();
    (void)virtualBus.bind(hal);

    BitBangConfig config;
    config.txPin = 4;
    config.rxPin = 5;
    config.enablePullup = false;
    TEST_ASSERT_TRUE(driver.init(hal, config));
}

void tearDown()
{
    driver.shutdown();
}

void test_VTP1_001_tx_nominal_frame_timing()
{
    const uint8_t frame[1] = {0x00};
    TEST_ASSERT_TRUE(driver.send(frame));

    virtualBus.advanceTimeUs(16000);

    const auto& transitions = virtualBus.capturedTxTransitions();
    TEST_ASSERT_FALSE(transitions.empty());

    // For 0x00 with TP1 framing, start + 8 data + parity are dominant (0), stop is recessive (1).
    // Each dominant cell produces a dominant transition and a recessive transition.
    TEST_ASSERT_EQUAL_UINT32(20u, static_cast<uint32_t>(transitions.size()));

    // Check first two cells timing against nominal scheduler points.
    // TX starts only after the KNX t_idle window: system priority (CTRL 0x00)
    // waits 61 bit times = 6344 us from the (virtual) bus-idle sync point.
    // Tolerance is ±1 us because the simulator uses integer-microsecond scheduling boundaries.
    assertTransitionTimestampWithin(transitions[0], 6344u, 1u);
    TEST_ASSERT_EQUAL_UINT16(4u, transitions[0].pin);
    TEST_ASSERT_EQUAL_UINT8(1u, transitions[0].level);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TimerGpioHalVirtualBus::TxSource::Driver),
                            static_cast<uint8_t>(transitions[0].source));
    assertTransitionTimestampWithin(transitions[1], 6379u, 1u);
    TEST_ASSERT_EQUAL_UINT16(4u, transitions[1].pin);
    TEST_ASSERT_EQUAL_UINT8(0u, transitions[1].level);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TimerGpioHalVirtualBus::TxSource::Driver),
                            static_cast<uint8_t>(transitions[1].source));

    assertTransitionTimestampWithin(transitions[2], 6448u, 1u);
    TEST_ASSERT_EQUAL_UINT8(1u, transitions[2].level);
    assertTransitionTimestampWithin(transitions[3], 6483u, 1u);
    TEST_ASSERT_EQUAL_UINT8(0u, transitions[3].level);

    // Every dominant-to-recessive pair for zero cells should be spaced by zeroActiveTimeUs (35 us).
    for (size_t i = 0; i + 1 < transitions.size(); i += 2) {
        TEST_ASSERT_EQUAL_UINT8(1u, transitions[i].level);
        TEST_ASSERT_EQUAL_UINT8(0u, transitions[i + 1].level);
        const uint64_t spacing = transitions[i + 1].timestampUs - transitions[i].timestampUs;
        TEST_ASSERT_EQUAL_UINT64(35u, spacing);
    }
}

void test_VTP1_005_multiple_frames_sequential_no_state_leakage()
{
    const uint8_t frames[][1] = {{0x11}, {0xA5}, {0x3C}};

    for (const auto& frame : frames) {
        TEST_ASSERT_TRUE(driver.send(frame));
        TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::TX_WAITING_SLOT, driver._txState);
        virtualBus.advanceTimeUs(16000);
        TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::TX_IDLE, driver._txState);
    }

    TEST_ASSERT_EQUAL_UINT32(0u, driver.droppedMessageCount());
    TEST_ASSERT_FALSE(virtualBus.capturedTxTransitions().empty());
}

void test_VTP1_006_nominal_path_no_internal_message_drop()
{
    const uint8_t frame[1] = {0x42};
    TEST_ASSERT_TRUE(driver.send(frame));
    virtualBus.advanceTimeUs(16000);

    BitBangDriverTimerIsr::Message message{};
    bool sawAckOutcome = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::TxAckResponse) {
            sawAckOutcome = true;
        }
    }

    TEST_ASSERT_TRUE(sawAckOutcome);
    TEST_ASSERT_EQUAL_UINT32(0u, driver.droppedMessageCount());
}

void test_VTP1_007_tx_trace_is_deterministic_across_repeated_runs()
{
    const uint8_t frame[1] = {0x00};

    TEST_ASSERT_TRUE(driver.send(frame));
    virtualBus.advanceTimeUs(16000);
    const auto firstRun = virtualBus.capturedTxTransitions();

    driver.shutdown();
    virtualBus.reset();
    (void)virtualBus.bind(hal);

    BitBangConfig config;
    config.txPin = 4;
    config.rxPin = 5;
    config.enablePullup = false;
    TEST_ASSERT_TRUE(driver.init(hal, config));

    TEST_ASSERT_TRUE(driver.send(frame));
    virtualBus.advanceTimeUs(16000);
    const auto secondRun = virtualBus.capturedTxTransitions();

    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(firstRun.size()),
                             static_cast<uint32_t>(secondRun.size()));

    for (size_t i = 0; i < firstRun.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT64(firstRun[i].timestampUs, secondRun[i].timestampUs);
        TEST_ASSERT_EQUAL_UINT16(firstRun[i].pin, secondRun[i].pin);
        TEST_ASSERT_EQUAL_UINT8(firstRun[i].level, secondRun[i].level);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(firstRun[i].source),
                                static_cast<uint8_t>(secondRun[i].source));
    }
}

void test_VTP1_008_multibyte_tx_timing_and_transition_volume()
{
    const uint8_t frame[2] = {0x00, 0x00};
    TEST_ASSERT_TRUE(driver.send(frame));

    virtualBus.advanceTimeUs(16000);

    const auto& transitions = virtualBus.capturedTxTransitions();
    TEST_ASSERT_EQUAL_UINT32(40u, static_cast<uint32_t>(transitions.size()));

    // First byte starts after the 61-bit t_idle window (6344 us); the second
    // character follows 13 bit times (1352 us) later.
    assertTransitionTimestampWithin(transitions[0], 6344u, 1u);
    assertTransitionTimestampWithin(transitions[20], 7696u, 1u);
}

void test_VTP1_009_wait_slot_defers_tx_start()
{
    // send() derives the bus-idle wait from the frame's CTRL priority bits
    // (03_02_02 t_idle): system priority = 61 bit times = 6344 us. Nothing
    // may reach the wire before that window elapses.
    const uint8_t frame[1] = {0x00};
    TEST_ASSERT_TRUE(driver.send(frame));

    virtualBus.advanceTimeUs(6200);
    TEST_ASSERT_TRUE(virtualBus.capturedTxTransitions().empty());

    virtualBus.advanceTimeUs(300);
    const auto& transitions = virtualBus.capturedTxTransitions();
    TEST_ASSERT_FALSE(transitions.empty());
    assertTransitionTimestampWithin(transitions[0], 6344u, 1u);
}

void test_VTP1_004_medium_state_transitions()
{
    const uint8_t frame[1] = {0x5A};
    TEST_ASSERT_TRUE(driver.send(frame));

    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::TX_WAITING_SLOT, driver._txState);

    virtualBus.advanceTimeUs(16000);

    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::TX_IDLE, driver._txState);

    BitBangDriverTimerIsr::Message message{};
    bool sawAckDeadlineMiss = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::TxAckResponse &&
            message.data == BitBangDriverTimerIsr::ACK_BYTE_NONE) {
            sawAckDeadlineMiss = true;
        }
    }

    TEST_ASSERT_TRUE(sawAckDeadlineMiss);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_VTP1_001_tx_nominal_frame_timing);
    RUN_TEST(test_VTP1_004_medium_state_transitions);
    RUN_TEST(test_VTP1_005_multiple_frames_sequential_no_state_leakage);
    RUN_TEST(test_VTP1_006_nominal_path_no_internal_message_drop);
    RUN_TEST(test_VTP1_007_tx_trace_is_deterministic_across_repeated_runs);
    RUN_TEST(test_VTP1_008_multibyte_tx_timing_and_transition_volume);
    RUN_TEST(test_VTP1_009_wait_slot_defers_tx_start);
    return UNITY_END();
}
