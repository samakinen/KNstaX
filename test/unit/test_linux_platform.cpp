// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_linux_platform.cpp
 * @brief LinuxPlatform: file-backed EEPROM, timing, tasks and the
 *        synchronisation primitives, plus UartConfig validation
 *
 * Nothing here needs hardware. The UART itself is not exercised: LinuxUart
 * opens whichever serial device the host happens to have, so its behaviour is
 * not something a host test can pin down.
 */

#include "unity.h"

#include "knx/platform/linux_platform.hpp"
#include "knx/platform/memory_interface.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/uart_interface.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>

using namespace knx::platform;

namespace {

// LinuxMemory persists here; nothing else in the test suite uses it.
constexpr const char* kEepromFile = "/tmp/knx_eeprom.bin";

} // namespace

void setUp(void)
{
    std::remove(kEepromFile);
}

void tearDown(void)
{
    std::remove(kEepromFile);
}

// ============================================================================
// System information
// ============================================================================

void test_serial_and_mac_are_set(void)
{
    LinuxPlatform platform;

    TEST_ASSERT_NOT_EQUAL(0u, platform.uniqueSerialNumber());

    std::array<uint8_t, 6> mac{};
    platform.macAddress(mac);
    bool allZero = true;
    for (uint8_t b : mac) {
        allZero = allZero && (b == 0u);
    }
    TEST_ASSERT_FALSE(allZero);
}

void test_random_bytes_fill_the_buffer(void)
{
    LinuxPlatform platform;

    std::array<uint8_t, 32> a{};
    std::array<uint8_t, 32> b{};
    platform.randomBytes(a);
    platform.randomBytes(b);
    TEST_ASSERT_FALSE(a == b);
}

// ============================================================================
// File-backed EEPROM
// ============================================================================

void test_memory_write_then_read(void)
{
    LinuxPlatform platform;
    MemoryInterface& memory = platform.memory();
    TEST_ASSERT_TRUE(memory.init().isOk());

    const uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), memory.write(100, std::span<const uint8_t>(data)));

    uint8_t readBack[sizeof(data)] = {};
    TEST_ASSERT_EQUAL_UINT32(sizeof(data), memory.read(100, std::span<uint8_t>(readBack)));
    TEST_ASSERT_EQUAL_MEMORY(data, readBack, sizeof(data));
}

void test_memory_fill_and_erase(void)
{
    LinuxPlatform platform;
    MemoryInterface& memory = platform.memory();
    TEST_ASSERT_TRUE(memory.init().isOk());

    TEST_ASSERT_EQUAL_UINT32(4u, memory.write(50, 0xFF, 4));
    uint8_t readBack[4] = {};
    memory.read(50, std::span<uint8_t>(readBack));
    const uint8_t ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_MEMORY(ones, readBack, sizeof(ones));

    TEST_ASSERT_TRUE(memory.erase(50, 4).isOk());
    memory.read(50, std::span<uint8_t>(readBack));
    const uint8_t zeros[4] = {};
    TEST_ASSERT_EQUAL_MEMORY(zeros, readBack, sizeof(zeros));
}

void test_memory_clamps_at_the_end(void)
{
    LinuxPlatform platform;
    MemoryInterface& memory = platform.memory();
    TEST_ASSERT_TRUE(memory.init().isOk());

    const auto size = static_cast<uint32_t>(memory.size());
    uint8_t buf[10] = {};

    // A transfer that runs past the end is cut short; one that starts past
    // it does nothing.
    TEST_ASSERT_EQUAL_UINT32(5u, memory.read(size - 5u, std::span<uint8_t>(buf)));
    TEST_ASSERT_EQUAL_UINT32(5u, memory.write(size - 5u, std::span<const uint8_t>(buf)));
    TEST_ASSERT_EQUAL_UINT32(0u, memory.read(size, std::span<uint8_t>(buf)));
    TEST_ASSERT_EQUAL_UINT32(0u, memory.write(size, std::span<const uint8_t>(buf, 1)));
    TEST_ASSERT_FALSE(memory.erase(size, 1).isOk());
    TEST_ASSERT_FALSE(memory.erase(0, 0).isOk());
}

void test_memory_commit_persists_across_instances(void)
{
    const uint8_t pattern[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    {
        LinuxPlatform platform;
        MemoryInterface& memory = platform.memory();
        TEST_ASSERT_TRUE(memory.init().isOk());
        TEST_ASSERT_EQUAL_UINT32(sizeof(pattern), memory.write(10, std::span<const uint8_t>(pattern)));
        memory.commit();
    }
    {
        LinuxPlatform platform;
        MemoryInterface& memory = platform.memory();
        TEST_ASSERT_TRUE(memory.init().isOk());
        uint8_t readBack[sizeof(pattern)] = {};
        TEST_ASSERT_EQUAL_UINT32(sizeof(pattern), memory.read(10, std::span<uint8_t>(readBack)));
        TEST_ASSERT_EQUAL_MEMORY(pattern, readBack, sizeof(pattern));
    }
}

// ============================================================================
// Timing
// ============================================================================

void test_delay_advances_millis_and_micros(void)
{
    LinuxPlatform platform;

    const uint32_t startMs = platform.millis();
    const uint64_t startUs = platform.micros();
    platform.delay(10);

    // Only lower bounds: a loaded host may sleep longer, never shorter.
    TEST_ASSERT_GREATER_OR_EQUAL(10u, platform.millis() - startMs);
    TEST_ASSERT_GREATER_OR_EQUAL(10000u, platform.micros() - startUs);
}

// ============================================================================
// Tasks
// ============================================================================

void test_task_runs_and_is_joined(void)
{
    LinuxPlatform platform;
    const SemaphoreHandle done = platform.createBinarySemaphore();
    std::atomic<bool> ran{false};
    std::atomic<TaskHandle> seenHandle{nullptr};

    TaskConfig config{};
    config.name = "test";
    config.function = [&]() {
        seenHandle = platform.currentTask();
        ran = true;
        (void)platform.semaphoreGive(done);
    };

    const TaskHandle task = platform.createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_TRUE(platform.semaphoreTake(done, UINT32_MAX).isOk());
    platform.deleteTask(task);

    TEST_ASSERT_TRUE(ran.load());
    TEST_ASSERT_TRUE(seenHandle.load() == task);
    TEST_ASSERT_NULL(platform.currentTask());
    platform.deleteSemaphore(done);
}

void test_task_notification_wakes_the_task(void)
{
    LinuxPlatform platform;
    const SemaphoreHandle done = platform.createBinarySemaphore();
    std::atomic<uint32_t> taken{0};

    TaskConfig config{};
    config.name = "notify";
    config.function = [&]() {
        taken = platform.taskNotifyTake(TaskNotifyClearMode::Clear, UINT32_MAX);
        (void)platform.semaphoreGive(done);
    };

    const TaskHandle task = platform.createTask(config);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_TRUE(platform.taskNotifyGive(task).isOk());
    TEST_ASSERT_TRUE(platform.semaphoreTake(done, UINT32_MAX).isOk());
    platform.deleteTask(task);

    TEST_ASSERT_GREATER_OR_EQUAL(1u, taken.load());
    platform.deleteSemaphore(done);
}

// ============================================================================
// Synchronisation primitives
// ============================================================================

void test_mutex_lock_unlock(void)
{
    LinuxPlatform platform;
    const MutexHandle mutex = platform.createMutex();
    TEST_ASSERT_NOT_NULL(mutex);

    TEST_ASSERT_TRUE(platform.lockMutex(mutex, 0).isOk());
    platform.unlockMutex(mutex);
    TEST_ASSERT_TRUE(platform.lockMutex(mutex, 100).isOk());
    platform.unlockMutex(mutex);

    platform.deleteMutex(mutex);
}

void test_binary_semaphore(void)
{
    LinuxPlatform platform;
    const SemaphoreHandle sem = platform.createBinarySemaphore();
    TEST_ASSERT_NOT_NULL(sem);

    // Created empty.
    TEST_ASSERT_FALSE(platform.semaphoreTake(sem, 0).isOk());
    TEST_ASSERT_TRUE(platform.semaphoreGive(sem).isOk());
    // Binary: a second give does not count up.
    TEST_ASSERT_TRUE(platform.semaphoreGive(sem).isOk());
    TEST_ASSERT_TRUE(platform.semaphoreTake(sem, 0).isOk());
    TEST_ASSERT_FALSE(platform.semaphoreTake(sem, 10).isOk());

    platform.deleteSemaphore(sem);
}

void test_queue_is_fifo_and_bounded(void)
{
    LinuxPlatform platform;
    const QueueHandle queue = platform.createQueue(sizeof(uint32_t), 2);
    TEST_ASSERT_NOT_NULL(queue);

    const uint32_t first = 0x12345678u;
    const uint32_t second = 0x9ABCDEF0u;
    TEST_ASSERT_TRUE(platform.queueSend(queue, &first, 0).isOk());
    TEST_ASSERT_TRUE(platform.queueSend(queue, &second, 0).isOk());
    TEST_ASSERT_FALSE(platform.queueSend(queue, &first, 0).isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(platform.queueCount(queue)));

    uint32_t received = 0;
    TEST_ASSERT_TRUE(platform.queueReceive(queue, &received, 0).isOk());
    TEST_ASSERT_EQUAL_HEX32(first, received);
    TEST_ASSERT_TRUE(platform.queueReceive(queue, &received, 0).isOk());
    TEST_ASSERT_EQUAL_HEX32(second, received);
    TEST_ASSERT_FALSE(platform.queueReceive(queue, &received, 0).isOk());

    platform.deleteQueue(queue);
}

void test_event_group_bits(void)
{
    LinuxPlatform platform;
    const EventGroupHandle group = platform.createEventGroup();
    TEST_ASSERT_NOT_NULL(group);

    platform.eventGroupSetBits(group, 0x01);
    uint32_t bits = platform.eventGroupWaitBits(group, 0x03, EventGroupClearMode::Keep,
                                                EventGroupWaitMode::Any, 0);
    TEST_ASSERT_EQUAL_HEX32(0x01u, bits & 0x03u);

    // Waiting for all of 0x03 with only 0x01 set times out.
    bits = platform.eventGroupWaitBits(group, 0x03, EventGroupClearMode::Keep,
                                       EventGroupWaitMode::All, 0);
    TEST_ASSERT_EQUAL_HEX32(0u, bits & 0x02u);

    platform.eventGroupSetBits(group, 0x02);
    bits = platform.eventGroupWaitBits(group, 0x03, EventGroupClearMode::Clear,
                                       EventGroupWaitMode::All, 0);
    TEST_ASSERT_EQUAL_HEX32(0x03u, bits & 0x03u);

    // Cleared on exit.
    bits = platform.eventGroupWaitBits(group, 0x03, EventGroupClearMode::Keep,
                                       EventGroupWaitMode::Any, 0);
    TEST_ASSERT_EQUAL_HEX32(0u, bits & 0x03u);

    platform.deleteEventGroup(group);
}

// ============================================================================
// Interfaces
// ============================================================================

void test_network_and_uart_are_provided(void)
{
    LinuxPlatform platform;
    TEST_ASSERT_NOT_NULL(platform.network());
    TEST_ASSERT_NOT_NULL(platform.uart());
    TEST_ASSERT_FALSE(platform.uart()->isOpen());
}

void test_uart_config_validation(void)
{
    UartConfig config;
    TEST_ASSERT_TRUE(config.validate().isOk());  // 19200 8E1, the TPUART default

    config.baudRate = 100;
    TEST_ASSERT_FALSE(config.validate().isOk());
    config.baudRate = 10000000;
    TEST_ASSERT_FALSE(config.validate().isOk());
    config.baudRate = 9600;
    TEST_ASSERT_TRUE(config.validate().isOk());
    config.baudRate = 115200;
    TEST_ASSERT_TRUE(config.validate().isOk());

    config.dataBits = 4;
    TEST_ASSERT_FALSE(config.validate().isOk());
    config.dataBits = 9;
    TEST_ASSERT_FALSE(config.validate().isOk());
    config.dataBits = 7;
    TEST_ASSERT_TRUE(config.validate().isOk());
    config.dataBits = 8;

    config.stopBits = 0;
    TEST_ASSERT_FALSE(config.validate().isOk());
    config.stopBits = 3;
    TEST_ASSERT_FALSE(config.validate().isOk());
    config.stopBits = 2;
    TEST_ASSERT_TRUE(config.validate().isOk());
    config.stopBits = 1;

    config.rxBufferSize = 0;
    TEST_ASSERT_FALSE(config.validate().isOk());
    config.rxBufferSize = 256;
    config.txBufferSize = 65537;
    TEST_ASSERT_FALSE(config.validate().isOk());
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_serial_and_mac_are_set);
    RUN_TEST(test_random_bytes_fill_the_buffer);

    RUN_TEST(test_memory_write_then_read);
    RUN_TEST(test_memory_fill_and_erase);
    RUN_TEST(test_memory_clamps_at_the_end);
    RUN_TEST(test_memory_commit_persists_across_instances);

    RUN_TEST(test_delay_advances_millis_and_micros);

    RUN_TEST(test_task_runs_and_is_joined);
    RUN_TEST(test_task_notification_wakes_the_task);

    RUN_TEST(test_mutex_lock_unlock);
    RUN_TEST(test_binary_semaphore);
    RUN_TEST(test_queue_is_fifo_and_bounded);
    RUN_TEST(test_event_group_bits);

    RUN_TEST(test_network_and_uart_are_provided);
    RUN_TEST(test_uart_config_validation);

    return UNITY_END();
}
