// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"
#include <array>

#define private public
#include "knx/physical/bitbang_driver_timer_isr.hpp"
#undef private

#include "knx/physical/timer_gpio_hal_virtual.hpp"
#include "knx/physical/virtual_tp1_bus_peer.hpp"

using namespace knx::physical;

namespace {

BitBangDriverTimerIsr driver;
TimerGpioHalVirtualBus virtualBus;
VirtualTp1BusPeer busPeer(virtualBus);
knx_timer_gpio_hal_t hal{};
BitBangConfig config{};

bool drainForDataByte(uint8_t expected)
{
    BitBangDriverTimerIsr::Message message{};
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::Data && message.data == expected) {
            return true;
        }
    }
    return false;
}

bool drainForMessage(BitBangDriverTimerIsr::MessageType messageType)
{
    BitBangDriverTimerIsr::Message message{};
    while (driver.popMessage(message)) {
        if (message.type == messageType) {
            return true;
        }
    }
    return false;
}

void setupDefaultConfig()
{
    config = BitBangConfig{};
    config.txPin = 4;
    config.rxPin = 5;
    config.enablePullup = false;
}

} // namespace

void setUp()
{
    virtualBus.reset();
    busPeer.clearScript();
    (void)virtualBus.bind(hal);
    setupDefaultConfig();
    TEST_ASSERT_TRUE(driver.init(hal, config));
}

void tearDown()
{
    driver.shutdown();
}

void test_VTP1_002_rx_nominal_frame_decode()
{
    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(0u, 0xA5, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript());

    virtualBus.advanceTimeUs(2500);

    TEST_ASSERT_TRUE(drainForDataByte(0xA5));
    TEST_ASSERT_TRUE(drainForMessage(BitBangDriverTimerIsr::MessageType::End));
}

void test_VTP1_003_no_ack_activity_for_unaddressed_traffic()
{
    // The DL-ACK decision is made autonomously inside the ISR from the
    // streamed frame header (there is no task-side submission API). Traffic
    // that is not addressed to this device must arm no ACK slot at all.
    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(0u, 0x5A, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript());

    virtualBus.advanceTimeUs(4300);

    BitBangDriverTimerIsr::Message message{};
    bool sawEnd = false;
    bool sawAckActivity = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::End) {
            sawEnd = true;
        }
        if (message.type == BitBangDriverTimerIsr::MessageType::EarlyAckArmed
                || message.type == BitBangDriverTimerIsr::MessageType::DlAckTxStart) {
            sawAckActivity = true;
        }
    }

    TEST_ASSERT_TRUE(sawEnd);
    TEST_ASSERT_FALSE(sawAckActivity);
}

void test_VTP1_101_isr_delay_burst()
{
    VirtualTp1BusPeer::FaultProfile faults{};
    faults.fixedDelayUs = 20;

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(0u, 0x3C, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript(faults));

    virtualBus.advanceTimeUs(2600);

    TEST_ASSERT_TRUE(drainForDataByte(0x3C));
}

void test_VTP1_102_timer_jitter_envelope()
{
    VirtualTp1BusPeer::FaultProfile faults{};
    faults.jitterUs = 2;
    faults.seed = 0x1234u;

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(100u, 0x77, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript(faults));
    const auto first = busPeer.lastInjectedEdges();

    driver.shutdown();
    virtualBus.reset();
    busPeer.clearScript();
    (void)virtualBus.bind(hal);
    TEST_ASSERT_TRUE(driver.init(hal, config));

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(100u, 0x77, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript(faults));
    const auto second = busPeer.lastInjectedEdges();

    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(first.size()), static_cast<uint32_t>(second.size()));
    for (size_t i = 0; i < first.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT64(first[i].timestampUs, second[i].timestampUs);
        TEST_ASSERT_EQUAL_UINT8(first[i].level, second[i].level);
    }
}

void test_VTP1_103_drop_rx_edge()
{
    VirtualTp1BusPeer::FaultProfile faults{};
    faults.dropEveryN = 3;

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(0u, 0xA5, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript(faults));

    virtualBus.advanceTimeUs(2600);

    TEST_ASSERT_TRUE(drainForMessage(BitBangDriverTimerIsr::MessageType::FramingError) ||
                     drainForMessage(BitBangDriverTimerIsr::MessageType::ParityError) ||
                     !drainForDataByte(0xA5));
}

void test_VTP1_104_duplicate_rx_edge()
{
    VirtualTp1BusPeer::FaultProfile faults{};
    faults.duplicateEveryN = 2;
    faults.duplicateSpacingUs = 1;

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(0u, 0x12, config, false));
    const uint32_t scriptedCount = static_cast<uint32_t>(busPeer.script().size());
    TEST_ASSERT_TRUE(busPeer.injectScript(faults));

    const uint32_t injectedCount = static_cast<uint32_t>(busPeer.lastInjectedEdges().size());
    TEST_ASSERT_TRUE(injectedCount > scriptedCount);

    virtualBus.advanceTimeUs(2600);
    TEST_ASSERT_EQUAL_UINT32(0u, driver.droppedMessageCount());
}

void test_VTP1_105_collision_overlap()
{
    // Collision = another sender drives the bus dominant while we transmit a
    // recessive '1' bit (bitwise arbitration loss). CTRL 0xFF is Low priority
    // → TX may start only after 53 bit-times of bus idle (64 bits from the
    // sync point ≈ 6656 µs). Start bit ≈ 6656–6760 µs, then eight '1' data
    // bits — inject the dominant pulse inside the first one.
    const uint8_t frame[1] = {0xFF};
    TEST_ASSERT_TRUE(driver.send(frame));

    TEST_ASSERT_TRUE(busPeer.injectCollisionPulseAtUs(6800u, 1u));
    virtualBus.advanceTimeUs(9000);

    TEST_ASSERT_TRUE(drainForMessage(BitBangDriverTimerIsr::MessageType::Collision));
}

void test_VTP1_106_malformed_start_stop()
{
    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(0u, 0x44, config, true));
    TEST_ASSERT_TRUE(busPeer.injectScript());

    virtualBus.advanceTimeUs(2600);

    TEST_ASSERT_TRUE(drainForMessage(BitBangDriverTimerIsr::MessageType::FramingError));
}

void test_VTP1_107_ack_response_bytes_ack_nack_busy()
{
    const uint8_t ackBytes[] = {
        BitBangDriverTimerIsr::ACK_BYTE_ACK,
        BitBangDriverTimerIsr::ACK_BYTE_NACK,
        BitBangDriverTimerIsr::ACK_BYTE_BUSY,
        BitBangDriverTimerIsr::ACK_BYTE_NACK_BUSY,
    };

    for (uint8_t ackByte : ackBytes) {
        driver.shutdown();
        virtualBus.reset();
        busPeer.clearScript();
        (void)virtualBus.bind(hal);
        TEST_ASSERT_TRUE(driver.init(hal, config));

        driver._txState = BitBangDriverTimerIsr::TX_WAITING_ACK;

        TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(0u, ackByte, config, false));
        TEST_ASSERT_TRUE(busPeer.injectScript());
        virtualBus.advanceTimeUs(2500);

        BitBangDriverTimerIsr::Message message{};
        bool sawAckResponse = false;
        while (driver.popMessage(message)) {
            if (message.type == BitBangDriverTimerIsr::MessageType::TxAckResponse) {
                TEST_ASSERT_EQUAL_UINT8(ackByte, message.data);
                sawAckResponse = true;
            }
        }

        TEST_ASSERT_TRUE(sawAckResponse);
        TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::TX_IDLE, driver._txState);
    }
}

void test_VTP1_108_ack_between_other_devices_decodes_as_one_byte_telegram()
{
    // An acknowledgement this device is not awaiting has no ACK window open, so
    // it is not an ACK transaction at all — it decodes down the ordinary receive
    // path as a one-byte telegram. That is what the bus monitor reports as
    // BUS AKRX, and it must NOT be mistaken for a response to a transmission.
    driver._txState = BitBangDriverTimerIsr::TX_IDLE;

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(0u, BitBangDriverTimerIsr::ACK_BYTE_ACK, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript());
    virtualBus.advanceTimeUs(4300);

    BitBangDriverTimerIsr::Message message{};
    bool sawByte = false;
    bool sawEnd = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::Data
                && message.data == BitBangDriverTimerIsr::ACK_BYTE_ACK) {
            sawByte = true;
        }
        if (message.type == BitBangDriverTimerIsr::MessageType::End) {
            sawEnd = true;
        }
        TEST_ASSERT_NOT_EQUAL(BitBangDriverTimerIsr::MessageType::TxAckResponse, message.type);
    }

    TEST_ASSERT_TRUE(sawByte);
    TEST_ASSERT_TRUE(sawEnd);
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::TX_IDLE, driver._txState);
}

void test_VTP1_109_corrupted_ack_reported_as_none_and_not_leaked()
{
    // A malformed stop bit on the acknowledgement character. The byte cannot be
    // trusted as an ACK, and — the part that used to be broken — its error flag
    // must not survive into the next telegram, where it would downgrade that
    // telegram's DL-ACK to NACK and make the peer retransmit a clean frame.
    driver._txState = BitBangDriverTimerIsr::TX_WAITING_ACK;

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(
        0u, BitBangDriverTimerIsr::ACK_BYTE_ACK, config, true));
    TEST_ASSERT_TRUE(busPeer.injectScript());
    virtualBus.advanceTimeUs(2500);

    BitBangDriverTimerIsr::Message message{};
    bool sawAckResponse = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::TxAckResponse) {
            TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::ACK_BYTE_NONE, message.data);
            sawAckResponse = true;
        }
        // The error belongs to the ACK transaction; it must not be published as
        // a receive error against whatever frame the consumer is assembling.
        TEST_ASSERT_NOT_EQUAL(BitBangDriverTimerIsr::MessageType::FramingError, message.type);
        TEST_ASSERT_NOT_EQUAL(BitBangDriverTimerIsr::MessageType::ParityError, message.type);
    }

    TEST_ASSERT_TRUE(sawAckResponse);
    TEST_ASSERT_EQUAL_UINT32(1u, driver.linkCounters().ackCharacterErrors);
    TEST_ASSERT_EQUAL_UINT8(0u, driver._rxErrors);
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::TX_IDLE, driver._txState);

    // The telegram that follows must be reported clean.
    busPeer.clearScript();
    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(3000u, 0xA5, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript());
    virtualBus.advanceTimeUs(3000);

    bool sawEnd = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::End) {
            TEST_ASSERT_EQUAL_UINT8(0u, message.data);
            sawEnd = true;
        }
    }
    TEST_ASSERT_TRUE(sawEnd);
}

void test_VTP1_110_ack_timeout_classified_as_no_edge()
{
    // Individually addressed standard frame → the driver opens an ACK window
    // after the last stop bit. Nothing answers, so the timeout must be
    // attributed to "no edge on the bus" rather than to a rejected or
    // half-decoded character.
    const uint8_t frame[] = {0xB0, 0x11, 0x03, 0x11, 0xFE, 0x60, 0xD2, 0x00};
    TEST_ASSERT_TRUE(driver.send(frame));

    // 61 bit-times of bus idle + 8 characters + the 36 bit-time ACK window.
    virtualBus.advanceTimeUs(25000);

    BitBangDriverTimerIsr::Message message{};
    bool sawTimeout = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::TxAckResponse) {
            TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::ACK_BYTE_NONE, message.data);
            sawTimeout = true;
        }
    }

    TEST_ASSERT_TRUE(sawTimeout);
    TEST_ASSERT_EQUAL_UINT32(1u, driver.linkCounters().ackTimeoutNoEdge);
    TEST_ASSERT_EQUAL_UINT32(0u, driver.linkCounters().ackTimeoutStartRejected);
    TEST_ASSERT_EQUAL_UINT32(0u, driver.linkCounters().ackTimeoutIncomplete);
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::TX_IDLE, driver._txState);
}

void test_VTP1_111_rejected_start_bit_is_counted()
{
    // A dominant pulse far shorter than startBitValidationTimeUs: the bus is
    // already recessive when the validation alarm re-reads it, so the edge is
    // discarded. Counting that is what separates "the peer sent nothing" from
    // "we threw away what the peer sent" in a field trace.
    TEST_ASSERT_EQUAL_UINT32(0u, driver.linkCounters().startValidationRejects);

    TEST_ASSERT_TRUE(busPeer.addEdgeAtUs(100u, 0u));
    TEST_ASSERT_TRUE(busPeer.addEdgeAtUs(101u, 1u));
    TEST_ASSERT_TRUE(busPeer.addEdgeAtUs(104u, 0u));
    TEST_ASSERT_TRUE(busPeer.injectScript());

    virtualBus.advanceTimeUs(1000);

    TEST_ASSERT_EQUAL_UINT32(1u, driver.linkCounters().startValidationRejects);
    TEST_ASSERT_EQUAL_UINT32(0u, driver.linkCounters().ackStartValidationRejects);
    TEST_ASSERT_FALSE(drainForMessage(BitBangDriverTimerIsr::MessageType::Data));
}

void test_VTP1_112_data_bytes_do_not_wake_the_consumer()
{
    // Every notification out of the bit-timing ISR costs a task wakeup, and on
    // ESP-IDF a portYIELD_FROM_ISR, charged to the latency budget of the next
    // bit sample. Per-byte Data messages therefore stay silent; the End that
    // always follows them does the waking.
    static uint32_t notifyCount = 0;
    notifyCount = 0;
    driver.setQueueNotifyCallback([](void*) { ++notifyCount; }, nullptr);

    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(0u, 0xA5, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript());
    virtualBus.advanceTimeUs(2500);

    TEST_ASSERT_TRUE(drainForDataByte(0xA5));
    // Data + End were queued; only End notified.
    TEST_ASSERT_EQUAL_UINT32(1u, notifyCount);

    driver.setQueueNotifyCallback(nullptr, nullptr);
}

void test_VTP1_113_max_size_telegram_length_is_not_truncated()
{
    // MAX_TELEGRAM_SIZE is 263. Held in a uint8_t, a full L_Data_Extended frame
    // wrapped to 7 and the driver keyed a handful of bytes onto the bus.
    std::array<uint8_t, 263> frame{};
    frame.fill(0x55);
    frame[0] = 0xB0;

    TEST_ASSERT_TRUE(driver.send(frame));
    TEST_ASSERT_EQUAL_UINT32(263u, static_cast<uint32_t>(driver._txTelegramLength));
}


void test_VTP1_114_stale_timer_alarm_is_rejected_not_serviced()
{
    // A compare that survived a reprogram must not be serviced under whatever
    // deadline replaced it. Simulate the survivor by firing the alarm with a
    // record that no longer matches the live RX deadline.
    driver._rxState = BitBangDriverTimerIsr::RX_RECEIVE;
    driver._rxBitPosition = 3;
    driver._rxAlarmUs = 5000;
    driver._armedAlarmOwner = BitBangDriverTimerIsr::AlarmOwner::Rx;
    driver._armedAlarmUs = 4000;  // the deadline that was replaced

    driver.onTimerAlarm();

    TEST_ASSERT_EQUAL_UINT32(1u, driver.linkCounters().staleTimerAlarms);
    // The bit position must not have advanced, and the live deadline survives.
    TEST_ASSERT_EQUAL_INT(3, driver._rxBitPosition);
    TEST_ASSERT_EQUAL_UINT64(5000u, driver._rxAlarmUs);

    // An alarm with no record at all is likewise rejected.
    driver._armedAlarmOwner = BitBangDriverTimerIsr::AlarmOwner::None;
    driver._armedAlarmUs = 0;
    driver.onTimerAlarm();
    TEST_ASSERT_EQUAL_UINT32(2u, driver.linkCounters().staleTimerAlarms);
    TEST_ASSERT_EQUAL_INT(3, driver._rxBitPosition);
}

void test_VTP1_115_rearm_records_earlier_deadline_and_prefers_rx_when_equal()
{
    driver._rxAlarmUs = 900;
    driver._txAlarmUs = 1500;
    driver.rearmTimer();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BitBangDriverTimerIsr::AlarmOwner::Rx),
                          static_cast<int>(driver._armedAlarmOwner));
    TEST_ASSERT_EQUAL_UINT64(900u, driver._armedAlarmUs);

    driver._rxAlarmUs = 2000;
    driver.rearmTimer();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BitBangDriverTimerIsr::AlarmOwner::Tx),
                          static_cast<int>(driver._armedAlarmOwner));
    TEST_ASSERT_EQUAL_UINT64(1500u, driver._armedAlarmUs);

    // Equal deadlines: RX wins, because a bit sample cannot be deferred without
    // losing the bit whereas a TX transition only shifts one edge.
    driver._rxAlarmUs = 1500;
    driver.rearmTimer();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BitBangDriverTimerIsr::AlarmOwner::Rx),
                          static_cast<int>(driver._armedAlarmOwner));

    // Nothing scheduled: nothing armed.
    driver._rxAlarmUs = 0;
    driver._txAlarmUs = 0;
    driver.rearmTimer();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BitBangDriverTimerIsr::AlarmOwner::None),
                          static_cast<int>(driver._armedAlarmOwner));
}

void test_VTP1_116_arbitration_loss_cannot_leave_a_stale_ack_intent()
{
    // A DL-ACK intent latched for one telegram must never acknowledge the next.
    // Arbitration loss cancels the armed slot without consuming the intent, so
    // the guard is that a new telegram clears it.
    driver._pendingAckByte = BitBangDriverTimerIsr::ACK_BYTE_ACK;
    driver._txState = BitBangDriverTimerIsr::TX_SENDING_ONE;

    (void)virtualBus.scheduleRxLevelAtUs(virtualBus.nowUs(), 1);
    virtualBus.advanceTimeUs(1);
    driver.onRxEdge();

    TEST_ASSERT_TRUE(drainForMessage(BitBangDriverTimerIsr::MessageType::Collision));
    TEST_ASSERT_EQUAL_UINT8(0u, driver._pendingAckByte);

    // And a telegram addressed to somebody else must still arm nothing.
    driver._txState = BitBangDriverTimerIsr::TX_IDLE;
    driver._pendingAckByte = BitBangDriverTimerIsr::ACK_BYTE_ACK;
    driver._rxState = BitBangDriverTimerIsr::RX_IDLE;
    busPeer.clearScript();
    TEST_ASSERT_TRUE(busPeer.addByteWaveformAtUs(3000u, 0x5A, config, false));
    TEST_ASSERT_TRUE(busPeer.injectScript());
    virtualBus.advanceTimeUs(4300);

    TEST_ASSERT_EQUAL_UINT8(0u, driver._pendingAckByte);
    TEST_ASSERT_FALSE(drainForMessage(BitBangDriverTimerIsr::MessageType::DlAckTxStart));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_VTP1_002_rx_nominal_frame_decode);
    RUN_TEST(test_VTP1_003_no_ack_activity_for_unaddressed_traffic);
    RUN_TEST(test_VTP1_101_isr_delay_burst);
    RUN_TEST(test_VTP1_102_timer_jitter_envelope);
    RUN_TEST(test_VTP1_103_drop_rx_edge);
    RUN_TEST(test_VTP1_104_duplicate_rx_edge);
    RUN_TEST(test_VTP1_105_collision_overlap);
    RUN_TEST(test_VTP1_106_malformed_start_stop);
    RUN_TEST(test_VTP1_107_ack_response_bytes_ack_nack_busy);
    RUN_TEST(test_VTP1_108_ack_between_other_devices_decodes_as_one_byte_telegram);
    RUN_TEST(test_VTP1_109_corrupted_ack_reported_as_none_and_not_leaked);
    RUN_TEST(test_VTP1_110_ack_timeout_classified_as_no_edge);
    RUN_TEST(test_VTP1_111_rejected_start_bit_is_counted);
    RUN_TEST(test_VTP1_112_data_bytes_do_not_wake_the_consumer);
    RUN_TEST(test_VTP1_113_max_size_telegram_length_is_not_truncated);
    RUN_TEST(test_VTP1_114_stale_timer_alarm_is_rejected_not_serviced);
    RUN_TEST(test_VTP1_115_rearm_records_earlier_deadline_and_prefers_rx_when_equal);
    RUN_TEST(test_VTP1_116_arbitration_loss_cannot_leave_a_stale_ack_intent);
    return UNITY_END();
}
