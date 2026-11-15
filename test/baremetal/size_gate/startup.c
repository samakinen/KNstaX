/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2025-2026 Sami Mäkinen */

/* Minimal Cortex-M3 startup for the size gate: vector table, .data/.bss
 * initialisation, C++ constructors, main. Not a BSP; a product writes its own. */

#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
extern int main(void);

void Reset_Handler(void);
void SysTick_Handler(void);
static void Default_Handler(void) { for (;;) {} }

__attribute__((section(".isr_vector"), used))
void (*const g_vectors[16])(void) = {
    (void (*)(void))(&_estack), Reset_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    0, 0, 0, 0,
    Default_Handler, Default_Handler, 0, Default_Handler,
    SysTick_Handler,
};

void Reset_Handler(void) {
    for (uint32_t *s = &_sidata, *d = &_sdata; d < &_edata;) { *d++ = *s++; }
    for (uint32_t *d = &_sbss; d < &_ebss;) { *d++ = 0; }
    for (void (**f)(void) = __init_array_start; f < __init_array_end; ++f) { (*f)(); }
    main();
    for (;;) {}
}
