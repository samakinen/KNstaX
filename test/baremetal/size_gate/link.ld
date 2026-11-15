/* Memory map for the size gate. The regions are deliberately larger than the
 * H8i8o's GD32F103CB (128 KiB flash, 20 KiB RAM): an over-budget image must
 * still link, so that scripts/ci/knstax_size_budget.py can report by how much
 * it is over instead of the linker refusing with an opaque overflow. The
 * budgets themselves live in size_budget.json. */
MEMORY
{
    FLASH (rx)  : ORIGIN = DEFINED(__flash_origin) ? __flash_origin : 0x08000000, LENGTH = 512K
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}

ENTRY(Reset_Handler)
_Min_Stack_Size = 2560;   /* main + interrupt stack, per the H8i8o RAM budget */

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

    /* Exception unwinding tables are unused with -fno-exceptions; discard
     * them rather than let a stray one count against the flash budget. */
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }

    _sidata = LOADADDR(.data);
    .data : {
        . = ALIGN(4); _sdata = .;
        *(.data .data.*)
        . = ALIGN(4); _edata = .;
    } > RAM AT > FLASH

    .bss (NOLOAD) : {
        . = ALIGN(4); _sbss = .;
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4); _ebss = .;
    } > RAM

    /* The heap grows up from here to the stack reserve; _sbrk enforces it. */
    .heap (NOLOAD) : {
        . = ALIGN(8); _heap_start = .;
    } > RAM

    _heap_limit = ORIGIN(RAM) + LENGTH(RAM) - _Min_Stack_Size;
    _estack = ORIGIN(RAM) + LENGTH(RAM);

    ASSERT(_heap_start <= _heap_limit, "static RAM leaves no room for the heap and stack")
}
