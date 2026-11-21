/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Configuration of the GD32F1 reference port.
 *
 * A product may put a gd32f1_port_config.h on its include path to override
 * any of these defaults (the port includes it first when it exists). The
 * defaults are those of the H8i8o module, the board the port was validated on.
 */
#pragma once

#if defined(__has_include)
#if __has_include("gd32f1_port_config.h")
#include "gd32f1_port_config.h"
#endif
#endif

/* Core clock. Only 48 MHz from the internal oscillator (HSI/2 x 12) is
 * implemented: it needs no crystal, and 48 MHz / 19200 baud divides exactly. */
#ifndef GD32F1_CORE_HZ
#define GD32F1_CORE_HZ 48000000u
#endif

/* TP-UART: USART0 (the STM32's USART1) on PA9 (TX) and PA10 (RX). The
 * interrupt priority is below SysTick (0x00), so time stamps are current. */
#ifndef GD32F1_TPUART_PRIORITY
#define GD32F1_TPUART_PRIORITY 0x40u
#endif
#ifndef GD32F1_TPUART_TX_RING
#define GD32F1_TPUART_TX_RING 32u
#endif
/* Octet trace of the host link for diagnostics; 0 compiles it out. */
#ifndef GD32F1_TPUART_TRACE_ENTRIES
#define GD32F1_TPUART_TRACE_ENTRIES 0u
#endif

/* The transceiver's SAVE output (bus power failing). Leave GD32F1_SAVE_PIN
 * undefined when the board does not wire it; read_save is then NULL.
 * GD32F1_SAVE_PORT: 0 = GPIOA, 1 = GPIOB, 2 = GPIOC. */
/* #define GD32F1_SAVE_PORT 1 */
/* #define GD32F1_SAVE_PIN 15 */
#ifndef GD32F1_SAVE_ACTIVE_LOW
#define GD32F1_SAVE_ACTIVE_LOW 1
#endif

/* Persistence region for FlashLog: the last 8 KiB of a 128 KiB part, eight
 * 1 KiB pages (medium density; high-density parts have 2 KiB pages). The
 * linker script must end the image below it. */
#ifndef GD32F1_PERSIST_BASE
#define GD32F1_PERSIST_BASE 0x0801E000u
#endif
#ifndef GD32F1_PERSIST_PAGE_SIZE
#define GD32F1_PERSIST_PAGE_SIZE 1024u
#endif
#ifndef GD32F1_PERSIST_PAGE_COUNT
#define GD32F1_PERSIST_PAGE_COUNT 8u
#endif

#if GD32F1_CORE_HZ != 48000000u
#error "the GD32F1 port implements only a 48 MHz core clock"
#endif
