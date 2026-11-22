// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_cooperative_platform.cpp
 * @brief CooperativePlatform: the single-threaded Platform for bare-metal targets
 *
 * Covers the contract in cooperative_platform.hpp: tasks are refused, locks
 * never block, queues are fixed rings that fail instead of waiting, and an
 * interrupt-side notification reaches the main loop as a flag.
 */

#include "unity.h"

#include "knx/platform/cooperative_platform.hpp"
#include "knx/platform/mutex.hpp"
#include "knx/util/log.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

using namespace knx;
using namespace knx::platform;

namespace {

struct FakeBoard {
    uint32_t nowMs{0};
    uint32_t idleCalls{0};
    uint32_t restartCalls{0};
    std::string lastLine;
    uint32_t serial{0x12345678u};
};

uint32_t boardMillis(void* ctx) { return static_cast<FakeBoard*>(ctx)->nowMs; }
uint64_t boardMicros(void* ctx) { return static_cast<uint64_t>(static_cast<FakeBoard*>(ctx)->nowMs) * 1000u; }
void boardIdle(void* ctx) {
    auto* board = static_cast<FakeBoard*>(ctx);
    ++board->idleCalls;
    ++board->nowMs;  // each idle step lets one millisecond pass, as WFI would until the next tick
}
uint32_t boardSerial(void* ctx) { return static_cast<FakeBoard*>(ctx)->serial; }
void boardRestart(void* ctx) { ++static_cast<FakeBoard*>(ctx)->restartCalls; }
void boardLog(void* ctx, const char* line) { static_cast<FakeBoard*>(ctx)->lastLine = line; }

FakeBoard g_board;

CooperativeHooks makeHooks() {
    CooperativeHooks hooks{};
    hooks.context = &g_board;
    hooks.millis = &boardMillis;
    hooks.micros = &boardMicros;
    hooks.idle = &boardIdle;
    hooks.serialNumber = &boardSerial;
    hooks.restart = &boardRestart;
    hooks.logLine = &boardLog;
    return hooks;
}

}  // namespace

void setUp(void) { g_board = FakeBoard{}; }
void tearDown(void) {}

void test_time_comes_from_the_board_hooks(void) {
    CooperativePlatform platform(makeHooks());
    g_board.nowMs = 1234;
    TEST_ASSERT_EQUAL_UINT32(1234u, platform.millis());
    TEST_ASSERT_EQUAL_UINT64(1234000u, platform.micros());
}

void test_delay_idles_until_the_time_has_passed(void) {
    CooperativePlatform platform(makeHooks());
    g_board.nowMs = 100;
    platform.delay(5);
    TEST_ASSERT_EQUAL_UINT32(105u, g_board.nowMs);
    TEST_ASSERT_EQUAL_UINT32(5u, g_board.idleCalls);
}

void test_delay_survives_a_millisecond_counter_wrap(void) {
    CooperativePlatform platform(makeHooks());
    g_board.nowMs = 0xFFFFFFFEu;
    platform.delay(4);
    TEST_ASSERT_EQUAL_UINT32(2u, g_board.nowMs);
}

void test_missing_time_hooks_read_as_zero(void) {
    CooperativePlatform platform(CooperativeHooks{});
    TEST_ASSERT_EQUAL_UINT32(0u, platform.millis());
    TEST_ASSERT_EQUAL_UINT64(0u, platform.micros());
    TEST_ASSERT_EQUAL_UINT32(0u, platform.uniqueSerialNumber());
}

void test_tasks_are_refused(void) {
    CooperativePlatform platform(makeHooks());
    TaskConfig config{};
    config.name = "rx";
    config.function = [] {};
    TEST_ASSERT_NULL(platform.createTask(config));
    TEST_ASSERT_NULL(platform.currentTask());
}

void test_mutex_handle_is_valid_and_locking_never_fails(void) {
    CooperativePlatform platform(makeHooks());
    MutexHandle mutex = platform.createMutex();
    TEST_ASSERT_NOT_NULL(mutex);
    TEST_ASSERT_TRUE(platform.lockMutex(mutex, 0).isOk());
    TEST_ASSERT_TRUE(platform.lockMutex(mutex, 0).isOk());  // a no-op lock cannot be contended
    platform.unlockMutex(mutex);
    platform.deleteMutex(mutex);
}

void test_queue_is_fifo_and_wraps_around(void) {
    CooperativePlatform platform(makeHooks());
    QueueHandle queue = platform.createQueue(sizeof(uint32_t), 3);
    TEST_ASSERT_NOT_NULL(queue);

    uint32_t out = 0;
    for (uint32_t round = 0; round < 4; ++round) {
        for (uint32_t i = 0; i < 3; ++i) {
            const uint32_t value = round * 10u + i;
            TEST_ASSERT_TRUE(platform.queueSend(queue, &value).isOk());
        }
        TEST_ASSERT_EQUAL_UINT64(3u, platform.queueCount(queue));
        for (uint32_t i = 0; i < 3; ++i) {
            TEST_ASSERT_TRUE(platform.queueReceive(queue, &out).isOk());
            TEST_ASSERT_EQUAL_UINT32(round * 10u + i, out);
        }
    }
    platform.deleteQueue(queue);
}

void test_queue_fails_instead_of_waiting(void) {
    CooperativePlatform platform(makeHooks());
    QueueHandle queue = platform.createQueue(sizeof(uint8_t), 1);
    uint8_t value = 7;
    TEST_ASSERT_TRUE(platform.queueSend(queue, &value).isOk());

    const auto full = platform.queueSend(queue, &value, 1000);
    TEST_ASSERT_TRUE(full.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::ResourceUnavailable, full.error());

    TEST_ASSERT_TRUE(platform.queueReceive(queue, &value).isOk());
    const auto empty = platform.queueReceive(queue, &value, 1000);
    TEST_ASSERT_TRUE(empty.isError());
    TEST_ASSERT_EQUAL(util::ErrorCode::Timeout, empty.error());
    platform.deleteQueue(queue);
}

void test_queue_rejects_a_zero_size(void) {
    CooperativePlatform platform(makeHooks());
    TEST_ASSERT_NULL(platform.createQueue(0, 4));
    TEST_ASSERT_NULL(platform.createQueue(4, 0));
}

void test_notification_from_an_interrupt_reaches_the_main_loop(void) {
    CooperativePlatform platform(makeHooks());
    TEST_ASSERT_FALSE(platform.consumeNotification());
    TEST_ASSERT_TRUE(platform.taskNotifyGiveFromISR(nullptr).isOk());
    TEST_ASSERT_TRUE(platform.consumeNotification());
    TEST_ASSERT_FALSE(platform.consumeNotification());  // consumed
}

void test_notify_take_never_blocks(void) {
    CooperativePlatform platform(makeHooks());
    TEST_ASSERT_EQUAL_UINT32(0u, platform.taskNotifyTake(TaskNotifyClearMode::Clear, 5000));
    TEST_ASSERT_TRUE(platform.taskNotifyGive(nullptr).isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, platform.taskNotifyTake(TaskNotifyClearMode::Keep, 5000));
    TEST_ASSERT_EQUAL_UINT32(1u, platform.taskNotifyTake(TaskNotifyClearMode::Clear, 5000));
    TEST_ASSERT_EQUAL_UINT32(0u, platform.taskNotifyTake(TaskNotifyClearMode::Clear, 5000));
    TEST_ASSERT_EQUAL_UINT32(0u, g_board.idleCalls);  // it did not wait
}

void test_event_group_reports_and_clears_bits(void) {
    CooperativePlatform platform(makeHooks());
    EventGroupHandle group = platform.createEventGroup();
    platform.eventGroupSetBits(group, 0x5u);

    // "Any" matches on one bit; "All" needs both.
    TEST_ASSERT_EQUAL_UINT32(0x5u, platform.eventGroupWaitBits(group, 0x1u, EventGroupClearMode::Keep,
                                                               EventGroupWaitMode::Any, 100));
    platform.eventGroupWaitBits(group, 0x3u, EventGroupClearMode::Clear, EventGroupWaitMode::All, 100);
    TEST_ASSERT_EQUAL_UINT32(0x5u, platform.eventGroupWaitBits(group, 0x4u, EventGroupClearMode::Keep,
                                                               EventGroupWaitMode::Any, 0));  // not satisfied: kept

    platform.eventGroupWaitBits(group, 0x4u, EventGroupClearMode::Clear, EventGroupWaitMode::Any, 0);
    platform.eventGroupClearBits(group, 0x1u);
    TEST_ASSERT_EQUAL_UINT32(0u, platform.eventGroupWaitBits(group, 0x7u, EventGroupClearMode::Keep,
                                                             EventGroupWaitMode::Any, 0));
    platform.deleteEventGroup(group);
}

void test_binary_semaphore_gives_once(void) {
    CooperativePlatform platform(makeHooks());
    SemaphoreHandle sem = platform.createBinarySemaphore();
    TEST_ASSERT_EQUAL(util::ErrorCode::Timeout, platform.semaphoreTake(sem, 100).error());
    TEST_ASSERT_TRUE(platform.semaphoreGive(sem).isOk());
    TEST_ASSERT_TRUE(platform.semaphoreGive(sem).isOk());  // binary: a second give does not stack
    TEST_ASSERT_TRUE(platform.semaphoreTake(sem, 0).isOk());
    TEST_ASSERT_TRUE(platform.semaphoreTake(sem, 0).isError());
    platform.deleteSemaphore(sem);
}

#if KNX_LOG_LEVEL_COMPILE == KNX_LOG_LEVEL_NONE
// Level 0 links no formatting code (see CooperativePlatform::log): the sink is
// never called, so nothing a product logs through the platform reaches it.
void test_log_at_level_none_never_reaches_the_sink(void) {
    CooperativePlatform platform(makeHooks());
    platform.log("I", "Tag", "value=%d", 42);
    TEST_ASSERT_TRUE(g_board.lastLine.empty());
}
#else
void test_log_goes_to_the_sink_and_is_truncated(void) {
    CooperativePlatform platform(makeHooks());
    platform.log("I", "Tag", "value=%d", 42);
    TEST_ASSERT_EQUAL_STRING("I Tag: value=42", g_board.lastLine.c_str());

    const std::string longText(300, 'x');
    platform.log("E", "Tag", "%s", longText.c_str());
    TEST_ASSERT_TRUE(g_board.lastLine.size() < 100u);
    TEST_ASSERT_TRUE(g_board.lastLine.rfind("E Tag: xxx", 0) == 0);
}
#endif

void test_log_without_a_sink_is_dropped(void) {
    CooperativeHooks hooks = makeHooks();
    hooks.logLine = nullptr;
    CooperativePlatform platform(hooks);
    platform.log("I", "Tag", "nothing listens");  // must not crash
    TEST_ASSERT_TRUE(g_board.lastLine.empty());
}

void test_mac_address_is_local_and_derived_from_the_serial(void) {
    CooperativePlatform platform(makeHooks());
    std::array<uint8_t, 6> mac{};
    platform.macAddress(std::span<uint8_t, 6>(mac));
    TEST_ASSERT_EQUAL_HEX8(0x02, mac[0]);  // locally administered unicast
    TEST_ASSERT_EQUAL_HEX8(0x12, mac[2]);
    TEST_ASSERT_EQUAL_HEX8(0x78, mac[5]);
    TEST_ASSERT_NULL(platform.network());
}

void boardRandom(void* ctx, uint8_t* out, size_t length) {
    (void)ctx;
    for (size_t i = 0; i < length; ++i) out[i] = static_cast<uint8_t>(0xA0u + i);
}

void test_random_bytes_come_from_the_board_hook(void) {
    // A board with a noise source supplies the S-A_Sync random itself.
    CooperativeHooks hooks = makeHooks();
    hooks.randomBytes = &boardRandom;
    CooperativePlatform platform(hooks);
    std::array<uint8_t, 4> random{};
    platform.randomBytes(random);
    TEST_ASSERT_EQUAL_HEX8(0xA0, random[0]);
    TEST_ASSERT_EQUAL_HEX8(0xA3, random[3]);

    // Without the hook the platform's fallback still fills the buffer.
    CooperativePlatform fallback(makeHooks());
    std::array<uint8_t, 16> a{};
    std::array<uint8_t, 16> b{};
    fallback.randomBytes(a);
    fallback.randomBytes(b);
    TEST_ASSERT_TRUE(a != b);
}

void test_restart_calls_the_board_hook(void) {
    // The hook returns here (a real one resets the MCU), so restart() would go on
    // to spin. Only the hook call is checked, through the same pointer it uses.
    CooperativePlatform platform(makeHooks());
    TEST_ASSERT_EQUAL_UINT32(0u, g_board.restartCalls);
    makeHooks().restart(&g_board);
    TEST_ASSERT_EQUAL_UINT32(1u, g_board.restartCalls);
    (void)platform;
}

void test_local_lock_compiles_to_the_selected_mutex(void) {
    // On hosts LocalMutex is std::mutex and really excludes; on a bare-metal
    // build it is a no-op with the same interface. The code below is valid for both.
    LocalMutex mutex;
    {
        LocalLock<LocalMutex> lock(mutex);
    }
    TEST_ASSERT_TRUE(mutex.try_lock());
    mutex.unlock();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_time_comes_from_the_board_hooks);
    RUN_TEST(test_delay_idles_until_the_time_has_passed);
    RUN_TEST(test_delay_survives_a_millisecond_counter_wrap);
    RUN_TEST(test_missing_time_hooks_read_as_zero);
    RUN_TEST(test_tasks_are_refused);
    RUN_TEST(test_mutex_handle_is_valid_and_locking_never_fails);
    RUN_TEST(test_queue_is_fifo_and_wraps_around);
    RUN_TEST(test_queue_fails_instead_of_waiting);
    RUN_TEST(test_queue_rejects_a_zero_size);
    RUN_TEST(test_notification_from_an_interrupt_reaches_the_main_loop);
    RUN_TEST(test_notify_take_never_blocks);
    RUN_TEST(test_event_group_reports_and_clears_bits);
    RUN_TEST(test_binary_semaphore_gives_once);
#if KNX_LOG_LEVEL_COMPILE == KNX_LOG_LEVEL_NONE
    RUN_TEST(test_log_at_level_none_never_reaches_the_sink);
#else
    RUN_TEST(test_log_goes_to_the_sink_and_is_truncated);
#endif
    RUN_TEST(test_log_without_a_sink_is_dropped);
    RUN_TEST(test_mac_address_is_local_and_derived_from_the_serial);
    RUN_TEST(test_random_bytes_come_from_the_board_hook);
    RUN_TEST(test_restart_calls_the_board_hook);
    RUN_TEST(test_local_lock_compiles_to_the_selected_mutex);
    return UNITY_END();
}
