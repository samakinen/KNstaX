// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#define private public
#include "knx/physical/bitbang_driver_timer_isr.hpp"
#undef private
#include "knx/datalink/tp1_dl_common.hpp"
#include "knx/physical/timer_gpio_hal_virtual.hpp"

#include <vector>

using namespace knx::physical;
namespace datalink = knx::datalink;

namespace {

BitBangDriverTimerIsr driver;
TimerGpioHalVirtualBus virtualBus;
knx_timer_gpio_hal_t hal{};

void scheduleByteWaveform(TimerGpioHalVirtualBus& bus, uint8_t value)
{
    uint64_t t = bus.nowUs();

    auto emitBit = [&](uint8_t bit) {
        if (bit == 0) {
            (void)bus.scheduleRxLevelAtUs(t, 1);
            (void)bus.scheduleRxLevelAtUs(t + 35, 0);
        }
        t += 104;
    };

    emitBit(0);
    for (int bit = 0; bit < 8; ++bit) {
        emitBit(static_cast<uint8_t>((value >> bit) & 0x1));
    }

    const uint8_t parity = static_cast<uint8_t>(__builtin_popcount(value) & 0x1);
    emitBit(parity);
    emitBit(1);
}

void scheduleByteWaveform(TimerGpioHalVirtualBus& bus,
                          uint8_t value,
                          uint8_t dominantLevel,
                          uint32_t bitTimeUs,
                          uint32_t zeroActiveTimeUs)
{
    uint64_t t = bus.nowUs();

    auto emitBit = [&](uint8_t bit) {
        if (bit == 0) {
            (void)bus.scheduleRxLevelAtUs(t, dominantLevel);
            (void)bus.scheduleRxLevelAtUs(t + zeroActiveTimeUs, static_cast<uint8_t>(dominantLevel == 0 ? 1 : 0));
        }
        t += bitTimeUs;
    };

    emitBit(0);
    for (int bit = 0; bit < 8; ++bit) {
        emitBit(static_cast<uint8_t>((value >> bit) & 0x1));
    }

    const uint8_t parity = static_cast<uint8_t>(__builtin_popcount(value) & 0x1);
    emitBit(parity);
    emitBit(1);
}

void scheduleDataWaveform(TimerGpioHalVirtualBus& bus,
                          uint8_t value,
                          uint64_t startTimeUs,
                          uint8_t dominantLevel,
                          uint32_t bitTimeUs,
                          uint32_t zeroActiveTimeUs)
{
    uint64_t t = startTimeUs;

    auto emitBit = [&](uint8_t bit) {
        if (bit == 0) {
            (void)bus.scheduleRxLevelAtUs(t, dominantLevel);
            (void)bus.scheduleRxLevelAtUs(t + zeroActiveTimeUs, static_cast<uint8_t>(dominantLevel == 0 ? 1 : 0));
        }
        t += bitTimeUs;
    };

    for (int bit = 0; bit < 8; ++bit) {
        emitBit(static_cast<uint8_t>((value >> bit) & 0x1));
    }

    const uint8_t parity = static_cast<uint8_t>(__builtin_popcount(value) & 0x1);
    emitBit(parity);
    emitBit(1);
}

// ── Whole-telegram helpers for the acknowledge tests ────────────────────────

constexpr uint16_t kOwnAddressRaw = 0x110A;    // 1.1.10
constexpr uint16_t kRemoteAddressRaw = 0x1114; // 1.1.20
constexpr uint64_t kBitUs = 104;
constexpr uint64_t kCharSpacingUs = 13 * kBitUs; // 11-bit character + 2-bit gap

// A standard L_Data frame carrying a one-octet A_GroupValue_Write, with a
// correct check octet (inverted XOR of everything before it, 03_02_02 §2.2.1).
std::vector<uint8_t> makeStandardFrame(uint16_t source, uint16_t destination, bool group)
{
    std::vector<uint8_t> frame = {
        0xBC,
        static_cast<uint8_t>(source >> 8), static_cast<uint8_t>(source),
        static_cast<uint8_t>(destination >> 8), static_cast<uint8_t>(destination),
        static_cast<uint8_t>(group ? 0xE1 : 0x61),
        0x00, 0x81,
    };
    uint8_t check = 0xFF;
    for (uint8_t octet : frame) {
        check ^= octet;
    }
    frame.push_back(check);
    return frame;
}

void scheduleCharacterAt(TimerGpioHalVirtualBus& bus, uint8_t value, uint64_t startUs)
{
    uint64_t t = startUs;
    auto emitBit = [&](uint8_t bit) {
        if (bit == 0) {
            (void)bus.scheduleRxLevelAtUs(t, 1);
            (void)bus.scheduleRxLevelAtUs(t + 35, 0);
        }
        t += kBitUs;
    };
    emitBit(0);
    for (int bit = 0; bit < 8; ++bit) {
        emitBit(static_cast<uint8_t>((value >> bit) & 0x1));
    }
    emitBit(static_cast<uint8_t>(__builtin_popcount(value) & 0x1));
    emitBit(1);
}

// Put a telegram on the bus and run time until any acknowledge is over.
void receiveTelegram(const std::vector<uint8_t>& frame)
{
    const uint64_t startUs = virtualBus.nowUs() + 20;
    // Start from a recessive line so the first start bit is a real edge.
    (void)virtualBus.scheduleRxLevelAtUs(startUs - 1, 0);
    for (size_t i = 0; i < frame.size(); ++i) {
        scheduleCharacterAt(virtualBus, frame[i], startUs + i * kCharSpacingUs);
    }
    virtualBus.advanceTimeUs(20 + frame.size() * kCharSpacingUs + 40 * kBitUs);
}

std::vector<BitBangDriverTimerIsr::Message> drainMessages()
{
    std::vector<BitBangDriverTimerIsr::Message> messages;
    BitBangDriverTimerIsr::Message message{};
    while (driver.popMessage(message)) {
        messages.push_back(message);
    }
    return messages;
}

// The DL-ACK byte the ISR put on the bus, or -1 when it sent none.
int sentAckByte(const std::vector<BitBangDriverTimerIsr::Message>& messages)
{
    for (const auto& message : messages) {
        if (message.type == BitBangDriverTimerIsr::MessageType::DlAckTxStart) {
            return message.data;
        }
    }
    return -1;
}

bool sawMessage(const std::vector<BitBangDriverTimerIsr::Message>& messages,
                BitBangDriverTimerIsr::MessageType type)
{
    for (const auto& message : messages) {
        if (message.type == type) {
            return true;
        }
    }
    return false;
}

void advanceUntilTxState(uint8_t state, uint64_t limitUs = 40000)
{
    for (uint64_t t = 0; t < limitUs && driver._txState != state; t += 2) {
        virtualBus.advanceTimeUs(2);
    }
    TEST_ASSERT_EQUAL_UINT8(state, driver._txState);
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

void test_tx_path_emits_gpio_transitions()
{
    const uint8_t frame[1] = {0x00};
    TEST_ASSERT_TRUE(driver.send(frame));

    // KNX t_idle: a System-priority frame may start only after 50 bit-times of
    // bus idle (61 bits from the sync point) ≈ 6.4 ms — advance past it plus
    // the frame itself.
    virtualBus.advanceTimeUs(10000);

    const auto& transitions = virtualBus.capturedTxTransitions();
    TEST_ASSERT_GREATER_THAN(0, static_cast<int>(transitions.size()));
    TEST_ASSERT_EQUAL_UINT8(1, transitions.front().level);
}

void test_rx_path_replays_waveform_into_data_and_end_messages()
{
    scheduleByteWaveform(virtualBus, 0xA5);

    virtualBus.advanceTimeUs(2200);

    BitBangDriverTimerIsr::Message message{};
    bool sawData = false;
    bool sawEnd = false;

    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::Data) {
            sawData = true;
        }
        if (message.type == BitBangDriverTimerIsr::MessageType::End) {
            sawEnd = true;
        }
    }

    TEST_ASSERT_TRUE(sawData);
    TEST_ASSERT_TRUE(sawEnd);
}

void test_collision_when_bus_goes_dominant_during_recessive_one_bit()
{
    // Collision is only meaningful while we expect the bus recessive (sending
    // a logical '1'): a dominant edge there is another transmitter winning
    // bitwise arbitration. During dominant-driven phases the edge is our own.
    driver._txState = BitBangDriverTimerIsr::TX_SENDING_ONE;
    (void)virtualBus.scheduleRxLevelAtUs(virtualBus.nowUs(), 1);
    virtualBus.advanceTimeUs(1);
    driver.onRxEdge();

    BitBangDriverTimerIsr::Message message{};
    bool sawCollision = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::Collision) {
            sawCollision = true;
        }
    }

    TEST_ASSERT_TRUE(sawCollision);
}

void test_rx_edge_during_active_transmit_does_not_start_receive_framing()
{
    driver._txState = BitBangDriverTimerIsr::TX_SENDING_ZERO_ACTIVE;
    driver._rxState = BitBangDriverTimerIsr::RX_IDLE;
    driver._rxAlarmUs = 0;

    driver.onRxEdge();

    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::RX_IDLE, driver._rxState);
    TEST_ASSERT_EQUAL_UINT64(0u, driver._rxAlarmUs);
    TEST_ASSERT_EQUAL_UINT8(1u, driver._rxZeroDetected);
}

void test_message_queue_drop_counter_stays_zero_under_moderate_burst()
{
    for (size_t i = 0; i < 32; ++i) {
        scheduleByteWaveform(virtualBus, static_cast<uint8_t>(i));
    }

    virtualBus.advanceTimeUs(32 * 2200);

    TEST_ASSERT_EQUAL_UINT32(0, driver.droppedMessageCount());
}

void test_inverted_transceiver_traits_flip_tx_and_rx_polarity()
{
    driver.shutdown();
    virtualBus.reset();
    (void)virtualBus.bind(hal);

    BitBangConfig config;
    config.txPin = 4;
    config.rxPin = 5;
    config.enablePullup = false;
    config.txDominantHigh = false;
    config.rxDominantHigh = false;
    TEST_ASSERT_TRUE(driver.init(hal, config));
    virtualBus.clearCapturedTxTransitions();

    const uint8_t frame[1] = {0x00};
    TEST_ASSERT_TRUE(driver.send(frame));

    // Cover the KNX t_idle bus-access wait (~6.4 ms) plus the frame.
    virtualBus.advanceTimeUs(10000);
    const auto& transitions = virtualBus.capturedTxTransitions();
    TEST_ASSERT_FALSE(transitions.empty());
    bool sawDominantLow = false;
    for (const auto& transition : transitions) {
        if (transition.level == 0) {
            sawDominantLow = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(sawDominantLow);

    driver.shutdown();
    virtualBus.reset();
    (void)virtualBus.bind(hal);
    TEST_ASSERT_TRUE(driver.init(hal, config));

    scheduleByteWaveform(virtualBus, 0x5A, 0, config.serialBitTimeUs, config.zeroActiveTimeUs);
    virtualBus.advanceTimeUs(2200);

    BitBangDriverTimerIsr::Message message{};
    bool sawExpectedData = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::Data && message.data == 0x5A) {
            sawExpectedData = true;
        }
    }

    TEST_ASSERT_TRUE(sawExpectedData);
}

void test_unaddressed_telegram_arms_no_ack_slot()
{
    // The ISR decides the DL-ACK autonomously from the streamed header; a
    // telegram not addressed to this device must not arm the ACK slot (and
    // therefore produce no ACK-related activity at all).
    scheduleByteWaveform(virtualBus, 0xA5);

    virtualBus.advanceTimeUs(2200);

    BitBangDriverTimerIsr::Message message{};
    bool sawAckActivity = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::EarlyAckArmed
                || message.type == BitBangDriverTimerIsr::MessageType::DlAckTxStart) {
            sawAckActivity = true;
        }
    }

    TEST_ASSERT_FALSE(sawAckActivity);
}

void test_start_bit_glitch_shorter_than_validation_time_is_rejected()
{
    // Force bus recessive first so a RISING edge can be simulated.
    (void)virtualBus.scheduleRxLevelAtUs(0, 0);
    // Dominant edge at t=10 fires GPIO ISR → RX_START_BIT_PENDING, validation alarm at t=25.
    (void)virtualBus.scheduleRxLevelAtUs(10, 1);
    // Glitch ends at t=12, before the validation timer at t=25.
    (void)virtualBus.scheduleRxLevelAtUs(12, 0);

    virtualBus.advanceTimeUs(100);

    // processStartBit reads bus level (recessive) at t=25 → rejected → back to IDLE.
    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::RX_IDLE, driver._rxState);
    BitBangDriverTimerIsr::Message message{};
    TEST_ASSERT_FALSE(driver.popMessage(message));
}

void test_first_data_zero_edge_after_start_sample_keeps_byte_sync_point()
{
    driver.shutdown();
    virtualBus.reset();
    (void)virtualBus.bind(hal);

    BitBangConfig config;
    config.txPin = 4;
    config.rxPin = 5;
    config.enablePullup = false;
    config.bitSamplingOffsetUs = 52;
    TEST_ASSERT_TRUE(driver.init(hal, config));

    driver.onRxEdge();
    TEST_ASSERT_EQUAL_UINT64(0u, driver._lastCharStartUs);
    TEST_ASSERT_EQUAL_INT(-1, driver._rxBitPosition);
    TEST_ASSERT_EQUAL_UINT64(15u, driver._rxAlarmUs);
    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::RX_START_BIT_PENDING, driver._rxState);

    virtualBus.advanceTimeUs(52);
    TEST_ASSERT_EQUAL_INT(0, driver._rxBitPosition);
    TEST_ASSERT_EQUAL_UINT64(156u, driver._rxAlarmUs);
    TEST_ASSERT_EQUAL_UINT8(0u, driver._rxZeroDetected);

    virtualBus.advanceTimeUs(52);
    driver.onRxEdge();

    TEST_ASSERT_EQUAL_UINT64(0u, driver._lastCharStartUs);
    TEST_ASSERT_EQUAL_INT(0, driver._rxBitPosition);
    TEST_ASSERT_EQUAL_UINT64(156u, driver._rxAlarmUs);
    TEST_ASSERT_EQUAL_UINT8(1u, driver._rxZeroDetected);
}

void test_late_start_sample_followed_by_first_data_edge_reanchors_when_bit_position_is_zero()
{
    driver.shutdown();
    virtualBus.reset();
    (void)virtualBus.bind(hal);

    BitBangConfig config;
    config.txPin = 4;
    config.rxPin = 5;
    config.enablePullup = false;
    config.bitSamplingOffsetUs = 52;
    TEST_ASSERT_TRUE(driver.init(hal, config));

    driver._rxState = BitBangDriverTimerIsr::RX_RECEIVE;
    driver._lastCharStartUs = 0;
    driver._rxBitPosition = -1;
    driver._rxZeroDetected = 1;
    driver._rxAlarmUs = 52;
    // Mark the deadline as the one programmed into hardware. onTimerAlarm()
    // services only the alarm that was actually armed, so a test that
    // fabricates a deadline has to say so — but it must not go through
    // rearmTimer(), which would also program the virtual timer and let the
    // scheduler fire this same alarm a second time.
    driver._armedAlarmOwner = BitBangDriverTimerIsr::AlarmOwner::Rx;
    driver._armedAlarmUs = driver._rxAlarmUs;

    virtualBus.advanceTimeUs(104);
    driver.onTimerAlarm();

    TEST_ASSERT_EQUAL_INT(0, driver._rxBitPosition);
    TEST_ASSERT_EQUAL_UINT64(156u, driver._rxAlarmUs);
    // position -1 no longer clears _rxZeroDetected; processStartBit() did that.
    // The flag stays at whatever value it held going in (1, set by manual setup).
    TEST_ASSERT_EQUAL_UINT8(1u, driver._rxZeroDetected);

    driver.onRxEdge();

    TEST_ASSERT_EQUAL_INT(0, driver._rxBitPosition);
    TEST_ASSERT_EQUAL_UINT64(156u, driver._rxAlarmUs);
    TEST_ASSERT_EQUAL_UINT64(0u, driver._lastCharStartUs);
}

void test_late_mid_byte_timer_callback_keeps_rx_sample_grid()
{
    driver.shutdown();
    virtualBus.reset();
    (void)virtualBus.bind(hal);

    BitBangConfig config;
    config.txPin = 4;
    config.rxPin = 5;
    config.enablePullup = false;
    config.bitSamplingOffsetUs = 52;
    TEST_ASSERT_TRUE(driver.init(hal, config));

    driver._rxState = BitBangDriverTimerIsr::RX_RECEIVE;
    driver._lastCharStartUs = 0;
    driver._rxBitPosition = 2;
    driver._rxByte = 0;
    driver._rxZeroDetected = 0;
    driver._rxAlarmUs = 364;
    // Mark the deadline as the one programmed into hardware. onTimerAlarm()
    // services only the alarm that was actually armed, so a test that
    // fabricates a deadline has to say so — but it must not go through
    // rearmTimer(), which would also program the virtual timer and let the
    // scheduler fire this same alarm a second time.
    driver._armedAlarmOwner = BitBangDriverTimerIsr::AlarmOwner::Rx;
    driver._armedAlarmUs = driver._rxAlarmUs;

    virtualBus.advanceTimeUs(430);
    driver.onTimerAlarm();

    TEST_ASSERT_EQUAL_INT(3, driver._rxBitPosition);
    TEST_ASSERT_EQUAL_UINT8(0x04u, driver._rxByte);
    TEST_ASSERT_EQUAL_UINT64(468u, driver._rxAlarmUs);
}

void test_back_to_back_unaddressed_telegrams_carry_no_ack_state_forward()
{
    // No task-side ACK API exists anymore; the ISR alone decides. Verify no
    // ACK state leaks from one unaddressed telegram to the next.
    scheduleByteWaveform(virtualBus, 0x33);
    virtualBus.advanceTimeUs(2200);

    BitBangDriverTimerIsr::Message message{};
    while (driver.popMessage(message)) {
        TEST_ASSERT_NOT_EQUAL(
            static_cast<int>(BitBangDriverTimerIsr::MessageType::EarlyAckArmed),
            static_cast<int>(message.type));
    }

    scheduleByteWaveform(virtualBus, 0x55);
    virtualBus.advanceTimeUs(2200);

    bool sawUnexpectedAck = false;
    while (driver.popMessage(message)) {
        if (message.type == BitBangDriverTimerIsr::MessageType::DlAckTxStart
                || message.type == BitBangDriverTimerIsr::MessageType::EarlyAckArmed) {
            sawUnexpectedAck = true;
        }
    }

    TEST_ASSERT_FALSE(sawUnexpectedAck);
}

void test_start_bit_late_validation_fire_accepts_real_start_bit()
{
    // Simulate: start bit dominant at t=0 (level→1), equalization starts at t=35
    // (level→0), but the timer ISR fires late at t=40 — past zeroActiveTimeUs.
    // The bus is recessive when processStartBit runs.
    // Expected: treated as a glitch and discarded — _rxState → IDLE.
    // (If the start bit was genuine, the higher layer will request retransmission.)

    (void)virtualBus.scheduleRxLevelAtUs(0, 1);  // start bit dominant edge
    (void)virtualBus.scheduleRxLevelAtUs(35, 0); // equalization (recessive)

    // Manually set state as if onRxEdge() ran at t=0, without arming the timer
    // (so advanceTimeUs won't fire processStartBit at t=15).
    driver._rxState = BitBangDriverTimerIsr::RX_START_BIT_PENDING;
    driver._lastCharStartUs = 0;
    driver._rxZeroDetected = 1;
    driver._rxAlarmUs = 15; // scheduled alarm time used by onTimerAlarm's firedAlarmUs
    // Mark the deadline as the one programmed into hardware. onTimerAlarm()
    // services only the alarm that was actually armed, so a test that
    // fabricates a deadline has to say so — but it must not go through
    // rearmTimer(), which would also program the virtual timer and let the
    // scheduler fire this same alarm a second time.
    driver._armedAlarmOwner = BitBangDriverTimerIsr::AlarmOwner::Rx;
    driver._armedAlarmUs = driver._rxAlarmUs;

    virtualBus.advanceTimeUs(40); // time now = 40; GPIO edge at t=0 re-sets _rxZeroDetected=1

    // Fire the timer manually, simulating the ISR arriving late at t=40.
    driver.onTimerAlarm();

    // processStartBit sees a recessive bus and discards the event — no late-fire
    // acceptance. Higher layer handles retransmission if needed.
    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::RX_IDLE, driver._rxState);
    TEST_ASSERT_EQUAL_UINT64(0u, driver._rxAlarmUs);
    TEST_ASSERT_EQUAL_UINT8(0u, driver._rxZeroDetected);
    BitBangDriverTimerIsr::Message message{};
    TEST_ASSERT_FALSE(driver.popMessage(message));
}

void test_rx_edge_in_receive_state_does_not_reschedule_timer()
{
    // When in RECEIVE state, a GPIO edge must only set _rxZeroDetected.
    // It must not modify _rxState, _rxBitPosition, _rxAlarmUs, or _lastCharStartUs.
    driver._rxState = BitBangDriverTimerIsr::RX_RECEIVE;
    driver._rxBitPosition = 3;
    driver._rxByte = 0x07;
    driver._rxZeroDetected = 0;
    driver._rxAlarmUs = 500;
    driver._lastCharStartUs = 100;

    driver.onRxEdge();

    TEST_ASSERT_EQUAL_UINT8(1u, driver._rxZeroDetected);
    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::RX_RECEIVE, driver._rxState);
    TEST_ASSERT_EQUAL_INT(3, driver._rxBitPosition);
    TEST_ASSERT_EQUAL_UINT64(500u, driver._rxAlarmUs);
    TEST_ASSERT_EQUAL_UINT64(100u, driver._lastCharStartUs);
}

void test_tx_ack_window_clears_stale_rx_loopback_state()
{
    driver._txTelegramLength = 1u;
    driver._txBytePosition = 0u;
    driver._txBitPosition = 9;
    driver._txBuffer[0] = datalink::CTRL_FRAME_TYPE | datalink::CTRL_ACK_REQ;

    driver._rxState = BitBangDriverTimerIsr::RX_RECEIVE;
    driver._rxByte = 0x6Du;
    driver._rxBitPosition = 9;
    driver._rxZeroDetected = 1u;

    const uint64_t nextDelayUs = driver.processEndOfTxBit();

    // TX_WAITING_ACK is itself the open-ACK-window state; there is no separate
    // flag to check any more.
    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::TX_WAITING_ACK, driver._txState);
    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::RX_IDLE, driver._rxState);
    TEST_ASSERT_EQUAL_UINT8(0u, driver._rxByte);
    TEST_ASSERT_EQUAL_INT(-1, driver._rxBitPosition);
    TEST_ASSERT_EQUAL_UINT8(0u, driver._rxZeroDetected);
    TEST_ASSERT_TRUE(nextDelayUs > 0u);
}

// ── B4: check octet and length (03_02_02 §2.4.2) ───────────────────────────

void test_addressed_correct_frame_is_acknowledged()
{
    driver.setOwnAddress(kOwnAddressRaw);
    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, kOwnAddressRaw, false));
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::ACK_BYTE_ACK, sentAckByte(drainMessages()));
}

// A frame whose check octet is wrong has no character error, so only the
// parity/framing test was applied and it was acknowledged: the sender never
// repeated a frame this device then discarded.
void test_addressed_frame_with_bad_check_octet_is_nakked()
{
    driver.setOwnAddress(kOwnAddressRaw);
    auto frame = makeStandardFrame(kRemoteAddressRaw, kOwnAddressRaw, false);
    frame.back() ^= 0x01;
    receiveTelegram(frame);
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::ACK_BYTE_NACK, sentAckByte(drainMessages()));
}

// Seven octets is shorter than any standard L_Data frame (8–23).
void test_addressed_frame_shorter_than_eight_octets_is_nakked()
{
    driver.setOwnAddress(kOwnAddressRaw);
    std::vector<uint8_t> frame = {0xBC, 0x11, 0x14, 0x11, 0x0A, 0x60};
    uint8_t check = 0xFF;
    for (uint8_t octet : frame) {
        check ^= octet;
    }
    frame.push_back(check);
    receiveTelegram(frame);
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::ACK_BYTE_NACK, sentAckByte(drainMessages()));
}

// ── B5: full group table, BUSY instead of a dropped frame ──────────────────

// The ISR table held 64 group addresses while the data link allows 256; the
// 65th and later were never acknowledged. The table is passed unsorted.
void test_group_address_beyond_the_64th_is_acknowledged()
{
    std::vector<uint16_t> table;
    for (uint16_t i = 0; i < 200; ++i) {
        table.push_back(static_cast<uint16_t>(0x0A00u + 199u - i));
    }
    driver.setAckGroupAddresses(table.data(), table.size());

    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, table[150], true));
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::ACK_BYTE_ACK, sentAckByte(drainMessages()));

    // A group address outside the table is not ours: no acknowledge at all.
    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, 0x0900u, true));
    TEST_ASSERT_EQUAL_INT(-1, sentAckByte(drainMessages()));
}

#if KNX_FEATURE_COUPLER
// A coupler acknowledges the group frames it routes (03/05/01 §4.5.5
// GROUP_IACK_ROUT) on top of its own group addresses; the conditions are
// OR-ed (03/03/03 §2.4.2.4.4). The Filter Table bit is read in the ISR.
void test_coupler_group_ack_filter_acknowledges_routed_group_frames()
{
    std::vector<uint8_t> filterTable(8192u, 0u);
    filterTable[0x0801u >> 3] = static_cast<uint8_t>(1u << (0x0801u & 7u));
    GroupAckFilter filter{};
    filter.filterTable = filterTable.data();
    filter.low = GroupAckFilter::Mode::FilterTable;
    filter.high = GroupAckFilter::Mode::Never;
    driver.setGroupAckFilter(&filter);
    const uint16_t own = 0x0A01u;
    driver.setAckGroupAddresses(&own, 1u);

    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, 0x0801u, true));
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::ACK_BYTE_ACK, sentAckByte(drainMessages()));
    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, 0x0802u, true));
    TEST_ASSERT_EQUAL_INT(-1, sentAckByte(drainMessages()));
    // Its own group address is still acknowledged, filter or not.
    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, own, true));
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::ACK_BYTE_ACK, sentAckByte(drainMessages()));
    // GROUP_7000 = LOCK: nothing above 6FFFh.
    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, 0x7801u, true));
    TEST_ASSERT_EQUAL_INT(-1, sentAckByte(drainMessages()));
    // GROUP_IACK_ROUT = 0 is Always on both ranges.
    filter.low = GroupAckFilter::Mode::Always;
    filter.high = GroupAckFilter::Mode::Always;
    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, 0x7801u, true));
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::ACK_BYTE_ACK, sentAckByte(drainMessages()));
    // An individual frame for someone else is outside this rule.
    driver.setOwnAddress(kOwnAddressRaw);
    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, 0x1234u, false));
    TEST_ASSERT_EQUAL_INT(-1, sentAckByte(drainMessages()));
    driver.setGroupAckFilter(nullptr);
    driver.setAckGroupAddresses(nullptr, 0u);
}
#endif

// While the receive queue is full the device answers BUSY, and End tells the
// consumer not to deliver this copy: the sender repeats it.
void test_busy_device_answers_busy_and_marks_the_frame_refused()
{
    driver.setOwnAddress(kOwnAddressRaw);
    driver.setLocalBusy(true);
    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, kOwnAddressRaw, false));

    const auto messages = drainMessages();
    TEST_ASSERT_EQUAL_INT(BitBangDriverTimerIsr::ACK_BYTE_BUSY, sentAckByte(messages));
    bool endMarkedRefused = false;
    for (const auto& message : messages) {
        if (message.type == BitBangDriverTimerIsr::MessageType::End) {
            endMarkedRefused = (message.data & BitBangDriverTimerIsr::END_FLAG_ANSWERED_BUSY) != 0u;
        }
    }
    TEST_ASSERT_TRUE(endMarkedRefused);
}

// ── B6: bus monitor never acknowledges ──────────────────────────────────────

void test_bus_monitor_mode_sends_no_acknowledge()
{
    driver.setOwnAddress(kOwnAddressRaw);
    driver.setBusMonitorMode(true);
    receiveTelegram(makeStandardFrame(kRemoteAddressRaw, kOwnAddressRaw, false));

    const auto messages = drainMessages();
    TEST_ASSERT_EQUAL_INT(-1, sentAckByte(messages));
    TEST_ASSERT_FALSE(sawMessage(messages, BitBangDriverTimerIsr::MessageType::EarlyAckArmed));
    TEST_ASSERT_TRUE(sawMessage(messages, BitBangDriverTimerIsr::MessageType::End));
}

// ── B1: own group frames wait for the acknowledge ──────────────────────────

void test_own_group_frame_without_a_bit_opens_the_ack_window()
{
    const auto frame = makeStandardFrame(kOwnAddressRaw, 0x0A01u, true);
    TEST_ASSERT_TRUE(driver.send(frame));
    virtualBus.advanceTimeUs(30000);

    const auto messages = drainMessages();
    TEST_ASSERT_FALSE(sawMessage(messages, BitBangDriverTimerIsr::MessageType::TxComplete));
    bool sawAckTimeout = false;
    for (const auto& message : messages) {
        if (message.type == BitBangDriverTimerIsr::MessageType::TxAckResponse
                && message.data == BitBangDriverTimerIsr::ACK_BYTE_NONE) {
            sawAckTimeout = true;
        }
    }
    TEST_ASSERT_TRUE(sawAckTimeout);
}

void test_own_frame_to_individual_broadcast_needs_no_acknowledge()
{
    const auto frame = makeStandardFrame(kOwnAddressRaw, 0xFFFFu, false);
    TEST_ASSERT_TRUE(driver.send(frame));
    virtualBus.advanceTimeUs(30000);

    const auto messages = drainMessages();
    TEST_ASSERT_TRUE(sawMessage(messages, BitBangDriverTimerIsr::MessageType::TxComplete));
    TEST_ASSERT_FALSE(sawMessage(messages, BitBangDriverTimerIsr::MessageType::TxAckResponse));
}

// ── B2: arbitration loss is retried, not reported as BUSY ───────────────────

void loseArbitrationNow()
{
    // Wait until a recessive '1' bit of the second octet is being sent, then
    // let another sender pull the bus dominant.
    for (int i = 0; i < 20000; ++i) {
        if (driver._txState == BitBangDriverTimerIsr::TX_SENDING_ONE && driver._txBytePosition >= 1) {
            break;
        }
        virtualBus.advanceTimeUs(1);
    }
    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::TX_SENDING_ONE, driver._txState);
    virtualBus.advanceTimeUs(10);
    const uint64_t now = virtualBus.nowUs();
    (void)virtualBus.scheduleRxLevelAtUs(now, 0);
    (void)virtualBus.scheduleRxLevelAtUs(now + 1, 1);
    (void)virtualBus.scheduleRxLevelAtUs(now + 36, 0);
    virtualBus.advanceTimeUs(2);
}

void test_lost_arbitration_restarts_the_frame_without_busy()
{
    const auto frame = makeStandardFrame(kOwnAddressRaw, 0x0A01u, true);
    TEST_ASSERT_TRUE(driver.send(frame));
    loseArbitrationNow();

    // Back to waiting for the bus with the whole frame still queued.
    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::TX_WAITING_SLOT, driver._txState);
    TEST_ASSERT_EQUAL_UINT16(frame.size(), driver._txTelegramLength);

    virtualBus.advanceTimeUs(40000);
    const auto messages = drainMessages();
    TEST_ASSERT_TRUE(sawMessage(messages, BitBangDriverTimerIsr::MessageType::Collision));
    TEST_ASSERT_FALSE(sawMessage(messages, BitBangDriverTimerIsr::MessageType::TxArbitrationLost));
    int outcomes = 0;
    for (const auto& message : messages) {
        if (message.type == BitBangDriverTimerIsr::MessageType::TxAckResponse) {
            ++outcomes;
            // The only outcome is the restarted frame's acknowledge window
            // (nobody answers here) — never a BUSY for the lost attempt.
            TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::ACK_BYTE_NONE, message.data);
        }
    }
    TEST_ASSERT_EQUAL_INT(1, outcomes);
}

void test_frame_is_abandoned_after_ten_arbitration_losses()
{
    const auto frame = makeStandardFrame(kOwnAddressRaw, 0x0A01u, true);
    TEST_ASSERT_TRUE(driver.send(frame));
    driver._txArbitrationLosses = BitBangDriverTimerIsr::MAX_TX_ARBITRATION_ATTEMPTS - 1u;
    loseArbitrationNow();

    TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::TX_IDLE, driver._txState);
    bool sawGiveUp = false;
    for (const auto& message : drainMessages()) {
        if (message.type == BitBangDriverTimerIsr::MessageType::TxArbitrationLost) {
            sawGiveUp = true;
            TEST_ASSERT_EQUAL_UINT8(BitBangDriverTimerIsr::MAX_TX_ARBITRATION_ATTEMPTS, message.data);
        }
    }
    TEST_ASSERT_TRUE(sawGiveUp);
}

// ── B3: 150 bit times after BUSY (03_02_02 §2.4.2, A.8) ────────────────────

void test_repetition_after_busy_waits_150_bit_times()
{
    const auto frame = makeStandardFrame(kOwnAddressRaw, 0x0A01u, true);
    TEST_ASSERT_TRUE(driver.send(frame));
    advanceUntilTxState(BitBangDriverTimerIsr::TX_WAITING_ACK);

    const uint64_t busyStartUs = virtualBus.nowUs() + 10 * kBitUs;
    (void)virtualBus.scheduleRxLevelAtUs(busyStartUs - 1, 0);
    scheduleCharacterAt(virtualBus, BitBangDriverTimerIsr::ACK_BYTE_BUSY, busyStartUs);
    virtualBus.advanceTimeUs(25 * kBitUs);
    bool sawBusy = false;
    for (const auto& message : drainMessages()) {
        if (message.type == BitBangDriverTimerIsr::MessageType::TxAckResponse) {
            sawBusy = message.data == BitBangDriverTimerIsr::ACK_BYTE_BUSY;
        }
    }
    TEST_ASSERT_TRUE(sawBusy);

    // The data link repeats at once; the medium must hold the frame back.
    virtualBus.clearCapturedTxTransitions();
    TEST_ASSERT_TRUE(driver.send(frame));
    virtualBus.advanceTimeUs(40000);

    const auto& transitions = virtualBus.capturedTxTransitions();
    uint64_t firstDominantUs = 0;
    for (const auto& transition : transitions) {
        if (transition.level == 1) {
            firstDominantUs = transition.timestampUs;
            break;
        }
    }
    TEST_ASSERT_TRUE(firstDominantUs != 0);
    TEST_ASSERT_TRUE(firstDominantUs >= busyStartUs + (11 + 150) * kBitUs);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_tx_path_emits_gpio_transitions);
    RUN_TEST(test_rx_path_replays_waveform_into_data_and_end_messages);
    RUN_TEST(test_collision_when_bus_goes_dominant_during_recessive_one_bit);
    RUN_TEST(test_rx_edge_during_active_transmit_does_not_start_receive_framing);
    RUN_TEST(test_message_queue_drop_counter_stays_zero_under_moderate_burst);
    RUN_TEST(test_inverted_transceiver_traits_flip_tx_and_rx_polarity);
    RUN_TEST(test_unaddressed_telegram_arms_no_ack_slot);
    RUN_TEST(test_start_bit_glitch_shorter_than_validation_time_is_rejected);
    RUN_TEST(test_start_bit_late_validation_fire_accepts_real_start_bit);
    RUN_TEST(test_first_data_zero_edge_after_start_sample_keeps_byte_sync_point);
    RUN_TEST(test_late_start_sample_followed_by_first_data_edge_reanchors_when_bit_position_is_zero);
    RUN_TEST(test_rx_edge_in_receive_state_does_not_reschedule_timer);
    RUN_TEST(test_tx_ack_window_clears_stale_rx_loopback_state);
    RUN_TEST(test_late_mid_byte_timer_callback_keeps_rx_sample_grid);
    RUN_TEST(test_back_to_back_unaddressed_telegrams_carry_no_ack_state_forward);
    RUN_TEST(test_addressed_correct_frame_is_acknowledged);
    RUN_TEST(test_addressed_frame_with_bad_check_octet_is_nakked);
    RUN_TEST(test_addressed_frame_shorter_than_eight_octets_is_nakked);
    RUN_TEST(test_group_address_beyond_the_64th_is_acknowledged);
#if KNX_FEATURE_COUPLER
    RUN_TEST(test_coupler_group_ack_filter_acknowledges_routed_group_frames);
#endif
    RUN_TEST(test_busy_device_answers_busy_and_marks_the_frame_refused);
    RUN_TEST(test_bus_monitor_mode_sends_no_acknowledge);
    RUN_TEST(test_own_group_frame_without_a_bit_opens_the_ack_window);
    RUN_TEST(test_own_frame_to_individual_broadcast_needs_no_acknowledge);
    RUN_TEST(test_lost_arbitration_restarts_the_frame_without_busy);
    RUN_TEST(test_frame_is_abandoned_after_ten_arbitration_losses);
    RUN_TEST(test_repetition_after_busy_waits_150_bit_times);
    return UNITY_END();
}
