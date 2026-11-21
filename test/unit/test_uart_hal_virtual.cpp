// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

// The UART HAL contract (knx/physical/uart_hal.h), its host implementation
// UartHalVirtual, and a board implementation written in C.

#include "unity.h"

#include "knx/physical/uart_hal.h"
#include "knx/physical/uart_hal_virtual.hpp"
#include "uart_hal_c_board.h"

#include <array>
#include <vector>

using knx::physical::UartHalVirtual;

namespace {

struct RxOctet {
    uint8_t octet;
    uint8_t flags;
    uint32_t timestampUs;
};

struct RxLog {
    std::vector<RxOctet> octets;
    const knx_uart_hal_t* hal{nullptr}; ///< When set, the callback answers each octet
    uint8_t answer{0};
};

void recordRx(void* context, uint8_t octet, uint8_t flags, uint32_t timestampUs)
{
    auto* log = static_cast<RxLog*>(context);
    log->octets.push_back({octet, flags, timestampUs});
    if (log->hal != nullptr) {
        // As the TP-UART backend's acknowledge decision does, from the ISR.
        (void)knx_uart_hal_write(log->hal, &log->answer, 1);
    }
}

constexpr knx_uart_hal_config_t CONFIG_8E1{19200u, KNX_UART_HAL_PARITY_EVEN};

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_helpers_reject_null_and_incomplete_hal(void)
{
    TEST_ASSERT_FALSE(knx_uart_hal_is_valid(nullptr));

    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal);
    TEST_ASSERT_TRUE(knx_uart_hal_is_valid(&hal));

    knx_uart_hal_t incomplete = hal;
    incomplete.ops.tx_free = nullptr;
    RxLog log;
    TEST_ASSERT_FALSE(knx_uart_hal_is_valid(&incomplete));
    TEST_ASSERT_FALSE(knx_uart_hal_open(&incomplete, &CONFIG_8E1, &recordRx, &log));
    TEST_ASSERT_EQUAL_UINT32(0, knx_uart_hal_tx_free(&incomplete));
    TEST_ASSERT_FALSE(uart.isOpen());

    TEST_ASSERT_FALSE(knx_uart_hal_open(&hal, nullptr, &recordRx, &log));
    TEST_ASSERT_FALSE(knx_uart_hal_open(&hal, &CONFIG_8E1, nullptr, &log));
    TEST_ASSERT_FALSE(uart.isOpen());
}

void test_open_records_config_and_refuses_second_open(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal);
    RxLog log;

    TEST_ASSERT_TRUE(knx_uart_hal_open(&hal, &CONFIG_8E1, &recordRx, &log));
    TEST_ASSERT_TRUE(uart.isOpen());
    TEST_ASSERT_TRUE(uart.config().has_value());
    TEST_ASSERT_EQUAL_UINT32(19200u, uart.config()->baud);
    TEST_ASSERT_EQUAL_INT(KNX_UART_HAL_PARITY_EVEN, uart.config()->parity);

    TEST_ASSERT_FALSE(knx_uart_hal_open(&hal, &CONFIG_8E1, &recordRx, &log));
    TEST_ASSERT_EQUAL_UINT32(1, uart.openCount());

    knx_uart_hal_close(&hal);
    TEST_ASSERT_FALSE(uart.isOpen());
    TEST_ASSERT_TRUE(knx_uart_hal_open(&hal, &CONFIG_8E1, &recordRx, &log));
    TEST_ASSERT_EQUAL_UINT32(2, uart.openCount());
}

void test_rx_reaches_callback_in_order_with_flags_and_timestamps(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal);
    RxLog log;

    TEST_ASSERT_FALSE(uart.deliverRx(0x03)); // closed: nothing delivered
    TEST_ASSERT_TRUE(knx_uart_hal_open(&hal, &CONFIG_8E1, &recordRx, &log));

    uart.setNowUs(1000);
    TEST_ASSERT_TRUE(uart.deliverRx(0xBC));
    uart.advanceUs(573); // one 8E1 octet at 19200 baud
    TEST_ASSERT_TRUE(uart.deliverRx(0x11, KNX_UART_HAL_RX_PARITY_ERROR | KNX_UART_HAL_RX_OVERRUN));

    TEST_ASSERT_EQUAL_UINT32(2, log.octets.size());
    TEST_ASSERT_EQUAL_HEX8(0xBC, log.octets[0].octet);
    TEST_ASSERT_EQUAL_HEX8(0, log.octets[0].flags);
    TEST_ASSERT_EQUAL_UINT32(1000, log.octets[0].timestampUs);
    TEST_ASSERT_EQUAL_HEX8(0x11, log.octets[1].octet);
    TEST_ASSERT_EQUAL_HEX8(KNX_UART_HAL_RX_PARITY_ERROR | KNX_UART_HAL_RX_OVERRUN, log.octets[1].flags);
    TEST_ASSERT_EQUAL_UINT32(1573, log.octets[1].timestampUs);

    knx_uart_hal_close(&hal);
    TEST_ASSERT_FALSE(uart.deliverRx(0x03));
    TEST_ASSERT_EQUAL_UINT32(2, log.octets.size());
}

void test_timestamps_wrap_at_32_bits(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal);
    RxLog log;
    TEST_ASSERT_TRUE(knx_uart_hal_open(&hal, &CONFIG_8E1, &recordRx, &log));

    uart.setNowUs(0xFFFFFF00u);
    TEST_ASSERT_TRUE(uart.deliverRx(0x01));
    uart.advanceUs(0x200);
    TEST_ASSERT_TRUE(uart.deliverRx(0x02));

    // The consumer's unsigned difference is right across the wrap.
    TEST_ASSERT_EQUAL_UINT32(0x200u, log.octets[1].timestampUs - log.octets[0].timestampUs);
}

void test_write_is_all_or_nothing(void)
{
    UartHalVirtual uart(4);
    knx_uart_hal_t hal{};
    uart.bind(hal);
    RxLog log;

    const std::array<uint8_t, 3> pair{0x80, 0xBC, 0x81};
    TEST_ASSERT_FALSE(knx_uart_hal_write(&hal, pair.data(), pair.size())); // closed
    TEST_ASSERT_EQUAL_UINT32(0, knx_uart_hal_tx_free(&hal));

    TEST_ASSERT_TRUE(knx_uart_hal_open(&hal, &CONFIG_8E1, &recordRx, &log));
    TEST_ASSERT_EQUAL_UINT32(4, knx_uart_hal_tx_free(&hal));
    TEST_ASSERT_TRUE(knx_uart_hal_write(&hal, pair.data(), pair.size()));
    TEST_ASSERT_EQUAL_UINT32(1, knx_uart_hal_tx_free(&hal));

    // Two octets do not fit into one free slot: nothing is queued.
    TEST_ASSERT_FALSE(knx_uart_hal_write(&hal, pair.data(), 2));
    TEST_ASSERT_EQUAL_UINT32(3, uart.txQueued());
    TEST_ASSERT_EQUAL_UINT32(2, uart.rejectedWrites()); // this one and the write while closed

    TEST_ASSERT_FALSE(knx_uart_hal_write(&hal, pair.data(), 0));
    TEST_ASSERT_FALSE(knx_uart_hal_write(&hal, nullptr, 1));

    const auto sent = uart.transmit(2);
    TEST_ASSERT_EQUAL_UINT32(2, sent.size());
    TEST_ASSERT_EQUAL_HEX8(0x80, sent[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBC, sent[1]);
    TEST_ASSERT_EQUAL_UINT32(3, knx_uart_hal_tx_free(&hal));
    TEST_ASSERT_TRUE(knx_uart_hal_write(&hal, pair.data(), 2));

    uart.transmitAll();
    const std::vector<uint8_t> expected{0x80, 0xBC, 0x81, 0x80, 0xBC};
    TEST_ASSERT_EQUAL_UINT32(expected.size(), uart.transmitted().size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), uart.transmitted().data(), expected.size());
}

void test_close_drops_queued_tx(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal);
    RxLog log;
    TEST_ASSERT_TRUE(knx_uart_hal_open(&hal, &CONFIG_8E1, &recordRx, &log));

    const uint8_t resetRequest = 0x01;
    TEST_ASSERT_TRUE(knx_uart_hal_write(&hal, &resetRequest, 1));
    knx_uart_hal_close(&hal);
    TEST_ASSERT_EQUAL_UINT32(0, uart.txQueued());
    TEST_ASSERT_TRUE(uart.transmitAll().empty());
}

void test_write_from_rx_callback_is_recorded_as_interrupt_context(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal);
    RxLog log;
    log.hal = &hal;
    log.answer = 0x11; // U_AckInformation, addressed
    TEST_ASSERT_TRUE(knx_uart_hal_open(&hal, &CONFIG_8E1, &recordRx, &log));

    const std::array<uint8_t, 2> mainWrite{0x80, 0xBC};
    TEST_ASSERT_TRUE(knx_uart_hal_write(&hal, mainWrite.data(), mainWrite.size()));
    TEST_ASSERT_TRUE(uart.deliverRx(0xBC));

    TEST_ASSERT_EQUAL_UINT32(2, uart.acceptedWrites());
    TEST_ASSERT_EQUAL_UINT32(1, uart.writesFromRxCallback());
    // The main context's write stays contiguous; the ISR's octet follows it.
    const std::vector<uint8_t> expected{0x80, 0xBC, 0x11};
    const auto sent = uart.transmitAll();
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), sent.data(), expected.size());
}

void test_optional_pins_absent_unless_bound(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal);

    TEST_ASSERT_FALSE(knx_uart_hal_has_reset(&hal));
    TEST_ASSERT_FALSE(knx_uart_hal_has_save(&hal));
    TEST_ASSERT_FALSE(knx_uart_hal_set_reset(&hal, true));
    uart.setSave(true);
    TEST_ASSERT_FALSE(knx_uart_hal_read_save(&hal)); // no signal: no power-fail claim
    TEST_ASSERT_FALSE(uart.resetAsserted());
}

void test_optional_pins_when_bound(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal, UartHalVirtual::Pins{true, true});

    TEST_ASSERT_TRUE(knx_uart_hal_has_reset(&hal));
    TEST_ASSERT_TRUE(knx_uart_hal_set_reset(&hal, true));
    TEST_ASSERT_TRUE(uart.resetAsserted());
    TEST_ASSERT_TRUE(knx_uart_hal_set_reset(&hal, false));
    TEST_ASSERT_FALSE(uart.resetAsserted());

    TEST_ASSERT_TRUE(knx_uart_hal_has_save(&hal));
    TEST_ASSERT_FALSE(knx_uart_hal_read_save(&hal));
    uart.setSave(true);
    TEST_ASSERT_TRUE(knx_uart_hal_read_save(&hal));
    TEST_ASSERT_FALSE(knx_uart_hal_has_save_edge_isr(&hal));  // not asked for
}

struct SaveEdgeLog {
    int calls{0};
    uint32_t timestampUs{0};
};

void recordSaveEdge(void* context, uint32_t timestampUs)
{
    auto* log = static_cast<SaveEdgeLog*>(context);
    ++log->calls;
    log->timestampUs = timestampUs;
}

void test_save_edge_interrupt_when_bound(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal, UartHalVirtual::Pins{false, true, true});
    TEST_ASSERT_TRUE(knx_uart_hal_has_save_edge_isr(&hal));
    TEST_ASSERT_FALSE(knx_uart_hal_install_save_edge_isr(&hal, nullptr, nullptr));

    SaveEdgeLog log;
    TEST_ASSERT_TRUE(knx_uart_hal_install_save_edge_isr(&hal, &recordSaveEdge, &log));
    uart.setNowUs(1234u);
    uart.setSave(true);
    uart.setSave(true);  // no change, no edge
    uart.setSave(false);
    TEST_ASSERT_EQUAL_INT(2, log.calls);
    TEST_ASSERT_EQUAL_UINT32(1234u, log.timestampUs);

    knx_uart_hal_remove_save_edge_isr(&hal);
    uart.setSave(true);
    TEST_ASSERT_EQUAL_INT(2, log.calls);
}

void test_save_edge_interrupt_needs_the_save_pin(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal, UartHalVirtual::Pins{false, false, true});
    TEST_ASSERT_FALSE(knx_uart_hal_has_save_edge_isr(&hal));
    SaveEdgeLog log;
    TEST_ASSERT_FALSE(knx_uart_hal_install_save_edge_isr(&hal, &recordSaveEdge, &log));
}

void test_virtual_refuses_invalid_config(void)
{
    UartHalVirtual uart;
    knx_uart_hal_t hal{};
    uart.bind(hal);
    RxLog log;

    const knx_uart_hal_config_t zeroBaud{0u, KNX_UART_HAL_PARITY_EVEN};
    TEST_ASSERT_FALSE(knx_uart_hal_open(&hal, &zeroBaud, &recordRx, &log));
    // 3: within the enum's range (its enumerators are 0..2), so the value is
    // defined in C++, but not a parity. 7 was outside it: UBSan refuses the load.
    const knx_uart_hal_config_t badParity{19200u, static_cast<knx_uart_hal_parity_t>(3)};
    TEST_ASSERT_FALSE(knx_uart_hal_open(&hal, &badParity, &recordRx, &log));
    TEST_ASSERT_FALSE(uart.isOpen());
}

void test_board_hal_written_in_c(void)
{
    c_board_uart_t board;
    knx_uart_hal_t hal{};
    c_board_uart_init(&board, &hal);
    RxLog log;

    TEST_ASSERT_TRUE(knx_uart_hal_is_valid(&hal));
    TEST_ASSERT_FALSE(knx_uart_hal_has_reset(&hal));
    TEST_ASSERT_TRUE(c_board_uart_open_8e1(&hal, &recordRx, &log));
    TEST_ASSERT_EQUAL_UINT32(19200u, board.baud);

    c_board_uart_receive(&board, 0x03, 42u);
    TEST_ASSERT_EQUAL_UINT32(1, log.octets.size());
    TEST_ASSERT_EQUAL_HEX8(0x03, log.octets[0].octet);
    TEST_ASSERT_EQUAL_UINT32(42u, log.octets[0].timestampUs);

    const std::array<uint8_t, 8> full{};
    TEST_ASSERT_TRUE(c_board_uart_write_via_helpers(&hal, full.data(), 6));
    TEST_ASSERT_EQUAL_UINT32(2, knx_uart_hal_tx_free(&hal));
    TEST_ASSERT_FALSE(c_board_uart_write_via_helpers(&hal, full.data(), 3));
    TEST_ASSERT_EQUAL_UINT32(6, board.tx_len);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_helpers_reject_null_and_incomplete_hal);
    RUN_TEST(test_open_records_config_and_refuses_second_open);
    RUN_TEST(test_rx_reaches_callback_in_order_with_flags_and_timestamps);
    RUN_TEST(test_timestamps_wrap_at_32_bits);
    RUN_TEST(test_write_is_all_or_nothing);
    RUN_TEST(test_close_drops_queued_tx);
    RUN_TEST(test_write_from_rx_callback_is_recorded_as_interrupt_context);
    RUN_TEST(test_optional_pins_absent_unless_bound);
    RUN_TEST(test_optional_pins_when_bound);
    RUN_TEST(test_save_edge_interrupt_when_bound);
    RUN_TEST(test_save_edge_interrupt_needs_the_save_pin);
    RUN_TEST(test_virtual_refuses_invalid_config);
    RUN_TEST(test_board_hal_written_in_c);
    return UNITY_END();
}
