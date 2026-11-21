/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GD32F1 reference port: clock, time, GPIO, identity, watchdog, reset.
 *
 * Register level, no vendor HAL. GD32F103 and STM32F103 share these
 * peripherals' registers. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gd32f1_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called first in Reset_Handler, before the clock, .data, .bss or
 * constructors: drive outputs to their safe level here. It must not use
 * global variables (they are not initialised yet). Weak; empty by default. */
void gd32f1_early_init(void);

/* Clock to 48 MHz (HSI/2 x 12), GPIOA-C and AFIO clocks on, JTAG released with
 * SWD kept (SWJ_CFG = 010, never 100), SysTick at 1 ms, DWT cycle counter on. */
void gd32f1_system_init(void);

/* Time since boot. Safe from any context. */
uint32_t gd32f1_millis(void);
uint64_t gd32f1_micros(void);

/* GPIO. port: 0 = A, 1 = B, 2 = C. */
enum {
    GD32F1_GPIO_INPUT_FLOATING = 0x04u,
    GD32F1_GPIO_INPUT_PULL_UP = 0x18u,   /* CTL 0x8 with the output latch high */
    GD32F1_GPIO_INPUT_PULL_DOWN = 0x28u, /* CTL 0x8 with the output latch low */
    GD32F1_GPIO_OUTPUT_2MHZ = 0x02u,     /* push-pull */
    GD32F1_GPIO_AF_OUTPUT_50MHZ = 0x0Bu, /* alternate function, push-pull */
};
void gd32f1_gpio_mode(uint32_t port, uint32_t pin, uint32_t mode);
void gd32f1_gpio_write(uint32_t port, uint32_t pin, bool high);
bool gd32f1_gpio_read(uint32_t port, uint32_t pin);

/* 32 bits derived from the 96-bit unique ID (FNV-1a). */
uint32_t gd32f1_serial_number(void);

/* What caused the last reset (RCU_RSTSCK flags, then cleared). */
uint32_t gd32f1_reset_cause(void);
const char* gd32f1_reset_cause_text(uint32_t cause);

/* Free watchdog: about 1 s; feed from the main loop only. Cannot be stopped. */
void gd32f1_watchdog_start(void);
void gd32f1_watchdog_feed(void);

void gd32f1_restart(void) __attribute__((noreturn));

/* Heap high-water mark kept by _sbrk (gd32f1_syscalls.c). */
uint32_t gd32f1_heap_high_water(void);

#ifdef __cplusplus
}
#endif
