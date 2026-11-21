// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/*
 * A minimal board-side knx_uart_hal_t written in C, as a board would write
 * one. It proves uart_hal.h compiles as C and that its inline helpers work
 * from C; test_uart_hal_virtual drives it through uart_hal_c_board.h.
 */

#include "uart_hal_c_board.h"

#include <string.h>

static bool c_board_open(void* context,
                         const knx_uart_hal_config_t* config,
                         knx_uart_hal_rx_cb_t rx_cb,
                         void* rx_context)
{
    c_board_uart_t* uart = (c_board_uart_t*)context;
    if (uart->open || config->parity != KNX_UART_HAL_PARITY_EVEN) {
        return false;
    }
    uart->baud = config->baud;
    uart->rx_cb = rx_cb;
    uart->rx_context = rx_context;
    uart->open = true;
    return true;
}

static void c_board_close(void* context)
{
    c_board_uart_t* uart = (c_board_uart_t*)context;
    uart->open = false;
    uart->rx_cb = NULL;
    uart->tx_len = 0;
}

static bool c_board_write(void* context, const uint8_t* data, size_t len)
{
    c_board_uart_t* uart = (c_board_uart_t*)context;
    if (!uart->open || len > sizeof uart->tx - uart->tx_len) {
        return false;
    }
    memcpy(&uart->tx[uart->tx_len], data, len);
    uart->tx_len += len;
    return true;
}

static size_t c_board_tx_free(void* context)
{
    const c_board_uart_t* uart = (const c_board_uart_t*)context;
    return uart->open ? sizeof uart->tx - uart->tx_len : 0;
}

void c_board_uart_init(c_board_uart_t* uart, knx_uart_hal_t* out_hal)
{
    memset(uart, 0, sizeof *uart);
    out_hal->context = uart;
    out_hal->ops.open = c_board_open;
    out_hal->ops.close = c_board_close;
    out_hal->ops.write = c_board_write;
    out_hal->ops.tx_free = c_board_tx_free;
    out_hal->ops.set_reset = NULL;
    out_hal->ops.read_save = NULL;
    out_hal->ops.install_save_edge_isr = NULL;
    out_hal->ops.remove_save_edge_isr = NULL;
}

void c_board_uart_receive(c_board_uart_t* uart, uint8_t octet, uint32_t timestamp_us)
{
    if (uart->open && uart->rx_cb) {
        uart->rx_cb(uart->rx_context, octet, 0u, timestamp_us);
    }
}

bool c_board_uart_write_via_helpers(const knx_uart_hal_t* hal, const uint8_t* data, size_t len)
{
    return knx_uart_hal_write(hal, data, len);
}

bool c_board_uart_open_8e1(const knx_uart_hal_t* hal, knx_uart_hal_rx_cb_t rx_cb, void* rx_context)
{
    const knx_uart_hal_config_t config = {19200u, KNX_UART_HAL_PARITY_EVEN};
    return knx_uart_hal_open(hal, &config, rx_cb, rx_context);
}
