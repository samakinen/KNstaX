/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GD32F1 reference port: USART0 (PA9 TX, PA10 RX) to a TP-UART transceiver,
 * as KNstaX's knx_uart_hal_t (include/knx/physical/uart_hal.h).
 *
 * Receive: one interrupt per octet, which calls the stack's callback with the
 * octet, its error flags and a microsecond time stamp. Transmit: a small ring
 * emptied by the TXE interrupt. write() masks interrupts while it copies, so a
 * write from the main context and one from the receive callback never
 * interleave (uart_hal.h, concurrency model).
 *
 * SAVE: when GD32F1_SAVE_PIN is configured (gd32f1_config.h), read_save reads
 * it and an EXTI edge interrupt on that line gives the stack its early
 * power-fail warning. The port then owns that line's EXTI handler. set_reset
 * is NULL: the reference board does not wire the transceiver's reset.
 *
 * The vector names follow the STM32F103 (USART1_IRQHandler for the GD32's
 * USART0).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "gd32f1_config.h"
#include "knx/physical/uart_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

knx_uart_hal_t gd32f1_tpuart_hal(void);

/* True while SAVE signals a bus power failure; false when SAVE is not wired. */
bool gd32f1_tpuart_save_asserted(void);

/* Diagnostics: octets received (and how many had an error flag), octets sent,
 * and writes refused because the ring was full or the port closed. */
typedef struct {
    uint32_t rx_octets;
    uint32_t rx_errors;
    uint32_t tx_octets;
    uint32_t tx_refused;
} gd32f1_tpuart_stats_t;

gd32f1_tpuart_stats_t gd32f1_tpuart_stats(void);

/* Octet trace (GD32F1_TPUART_TRACE_ENTRIES > 0): every octet on the host link,
 * in order. Entries are 16 bits:
 *   0x8000 | dir << 14 | ms & 0x3FFF   start of a burst (dir 1 = to the chip);
 *                                      a burst ends when the direction changes
 *                                      or after 2 ms without an octet
 *   0x00nn                             one octet of the burst
 * Read from the main loop; the interrupt writes. When the ring is full, new
 * entries are dropped and counted. Both return 0 when the trace is compiled out. */
#define GD32F1_TPUART_TRACE_MARK 0x8000u
#define GD32F1_TPUART_TRACE_TX 0x4000u

size_t gd32f1_tpuart_trace_read(uint16_t* out, size_t max);
uint32_t gd32f1_tpuart_trace_dropped(void);

#ifdef __cplusplus
}
#endif
