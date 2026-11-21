/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GD32F103xB / STM32F103xB (128 KiB flash, 20 KiB RAM) for the GD32F1 port.
 *
 * Flash: 120 KiB for the image, then 8 KiB for persistence (FlashLog, two
 * banks of four 1 KiB pages at 0x0801E000; GD32F1_PERSIST_* in
 * gd32f1_config.h). The image cannot grow into the persistence region: the
 * FLASH region ends where it starts. Change both together.
 *
 * RAM: .data (with .ramfunc, the flash erase wait), .bss, the heap, and the
 * main stack at the top. The stack reserve is 2.5 KiB; _sbrk refuses to grow
 * the heap into it.
 */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 120K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

ENTRY(Reset_Handler)
_Min_Stack_Size = 2560;

SECTIONS
{
    .isr_vector : { KEEP(*(.isr_vector)) } > FLASH

    .text : {
        *(.text .text.*)
        *(.rodata .rodata.*)
        KEEP(*(.init)) KEEP(*(.fini))
        . = ALIGN(4);
        __preinit_array_start = .; KEEP(*(.preinit_array)) __preinit_array_end = .;
        __init_array_start = .;    KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) __init_array_end = .;
        __fini_array_start = .;    KEEP(*(.fini_array)) KEEP(*(SORT(.fini_array.*))) __fini_array_end = .;
        . = ALIGN(4);
        _etext = .;
    } > FLASH

    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }

    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4); _sdata = .;
        *(.ramfunc .ramfunc.*)
        *(.data .data.*)
        . = ALIGN(4); _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : {
        . = ALIGN(4); _sbss = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4); _ebss = .;
    } > RAM

    .heap (NOLOAD) : {
        . = ALIGN(8); _heap_start = .;
    } > RAM

    _heap_limit = ORIGIN(RAM) + LENGTH(RAM) - _Min_Stack_Size;
    _estack = ORIGIN(RAM) + LENGTH(RAM);

    ASSERT(_heap_start <= _heap_limit, "static RAM leaves no room for the heap and the stack")
}
