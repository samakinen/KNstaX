// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/physical/uart_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool open;
    uint32_t baud;
    knx_uart_hal_rx_cb_t rx_cb;
    void* rx_context;
    uint8_t tx[8];
    size_t tx_len;
} c_board_uart_t;

void c_board_uart_init(c_board_uart_t* uart, knx_uart_hal_t* out_hal);
void c_board_uart_receive(c_board_uart_t* uart, uint8_t octet, uint32_t timestamp_us);
bool c_board_uart_write_via_helpers(const knx_uart_hal_t* hal, const uint8_t* data, size_t len);
bool c_board_uart_open_8e1(const knx_uart_hal_t* hal, knx_uart_hal_rx_cb_t rx_cb, void* rx_context);

#ifdef __cplusplus
}
#endif
