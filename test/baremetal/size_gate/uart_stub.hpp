// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file uart_stub.hpp
 * @brief A board UART for the measured image: enough to link the TP-UART backend as a product would
 *
 * Only the measured image (not the emulator variant) includes this. The
 * receive interrupt (Uart_IRQHandler, in the vector table) reads a stand-in
 * data register and calls the backend's RX callback, so the whole receive
 * path and its acknowledge decision stay in the image after LTO, as they do
 * behind a real USART interrupt. The image is linked and measured, not run.
 */

#pragma once

#include "knx/physical/uart_hal.h"

#include <cstddef>
#include <cstdint>

namespace size_gate::uart {

inline volatile uint32_t g_dataRegister = 0;  // stands in for USART_DATA and its status flags
inline volatile uint8_t g_txSink = 0;
inline volatile uint32_t g_timestampUs = 0;
inline knx_uart_hal_rx_cb_t g_rxCallback = nullptr;
inline void* g_rxContext = nullptr;
inline uint8_t g_context = 0;

inline bool open(void*, const knx_uart_hal_config_t*, knx_uart_hal_rx_cb_t rxCallback, void* rxContext) {
    g_rxContext = rxContext;
    g_rxCallback = rxCallback;
    return true;
}

inline void close(void*) { g_rxCallback = nullptr; }

inline bool write(void*, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        g_txSink = data[i];
    }
    return true;
}

inline size_t txFree(void*) { return 32u; }

inline knx_uart_hal_t makeHal() {
    knx_uart_hal_t hal{};
    hal.context = &g_context;
    hal.ops.open = &open;
    hal.ops.close = &close;
    hal.ops.write = &write;
    hal.ops.tx_free = &txFree;
    return hal;
}

} // namespace size_gate::uart

extern "C" void Uart_IRQHandler() {
    const uint32_t value = size_gate::uart::g_dataRegister;
    if (size_gate::uart::g_rxCallback != nullptr) {
        size_gate::uart::g_rxCallback(size_gate::uart::g_rxContext, static_cast<uint8_t>(value),
                                      static_cast<uint8_t>(value >> 8), size_gate::uart::g_timestampUs);
    }
}
