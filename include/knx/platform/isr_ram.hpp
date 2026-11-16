// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file isr_ram.hpp
 * @brief KNX_ISR_RAM: place interrupt-path code where it runs while flash is busy
 *
 * Code that an interrupt reaches must keep running while the MCU erases or
 * programs its flash (persistence does both). Where flash stalls instruction
 * fetch, that code has to run from RAM:
 *
 *   - ESP-IDF: IRAM_ATTR.
 *   - Bare-metal Cortex-M: section ".ramfunc". The product's linker script
 *     must place `*(.ramfunc .ramfunc.*)` in its initialised data region so
 *     the start-up code copies it to RAM (the size gate's link.ld does).
 *   - Hosts: nothing.
 *
 * A function marked KNX_ISR_RAM must not call out-of-line code that lives in
 * flash, or the placement buys nothing: callees are always_inline, or reached
 * through a pointer the board also placed in RAM (the UART HAL's write()).
 * The board must also keep the vector table and its own handler in RAM.
 *
 * A bare-metal build can opt out (Kconfig KNX_ISR_IN_RAM=n): the code stays
 * in flash, the board masks interrupts during an erase, and the medium makes
 * the bus repeat what it misses (platform::FlashEraseGuard).
 */

#pragma once

#if __has_include("knx/autoconf.h")
#include "knx/autoconf.h"
#endif

#if defined(ESP_PLATFORM)
#include "esp_attr.h"
#define KNX_ISR_RAM IRAM_ATTR
#elif defined(KNX_PLATFORM_BAREMETAL) && defined(__arm__) && defined(CONFIG_KNX_ISR_IN_RAM)
#define KNX_ISR_RAM __attribute__((section(".ramfunc"), noinline))
#else
#define KNX_ISR_RAM
#endif
