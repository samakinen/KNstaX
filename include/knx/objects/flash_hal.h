// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/*
 * Flash HAL for persistence on MCUs without NVS (FlashLog, FlashLogPersistence).
 *
 * A C interface so a board can implement it without C++. The board describes
 * one region of its internal flash, reserved for the stack, and provides
 * erase, program and read for it. Everything above that (the log format,
 * banks, compaction, recovery after a power cut) is the stack's.
 *
 * Contract
 * --------
 *   - Geometry. The region is page_count pages of page_size bytes starting at
 *     base. page_count is even and at least 2: the stack splits the region into
 *     two banks of page_count / 2 pages. write_unit is the smallest amount the
 *     flash programs at once (2 on STM32F1 and GD32F1, 8 on STM32G0 and L4),
 *     a power of two from 1 to 32. Addresses are the board's own; the stack
 *     only adds offsets to base.
 *   - Erased state. An erased byte reads 0xFF, and programming only clears
 *     bits. The stack never programs a write unit twice.
 *   - erase_page(addr): erase the page that starts at addr. Blocking; may take
 *     milliseconds and may stall code running from flash while it lasts.
 *   - program(addr, data, len): addr and len are multiples of write_unit, and
 *     the range lies inside one page. Blocking.
 *   - read(addr, out, len): any address and length inside the region. Reading
 *     a memory-mapped flash is a memcpy.
 *   - Every function returns false on a hardware error. The stack reads back
 *     what it programmed and checks erased pages, so a board need not verify.
 *
 * Concurrency model: the stack calls these from its main context only, one
 * call at a time. Nothing here is used from an interrupt.
 *
 * On flash with ECC (STM32G0, L4), reading a write unit whose programming was
 * cut short can raise an ECC fault. The stack does not handle that; a board on
 * such a part must make the read return false instead of faulting.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t base;       /* address of the first page of the region */
    uint32_t page_size;  /* erase unit in bytes */
    uint32_t page_count; /* pages in the region: even, at least 2 */
    uint32_t write_unit; /* program granularity in bytes: a power of two, 1 to 32 */
} knx_flash_hal_geometry_t;

typedef struct {
    bool (*erase_page)(void* context, uint32_t addr);
    bool (*program)(void* context, uint32_t addr, const uint8_t* data, size_t len);
    bool (*read)(void* context, uint32_t addr, uint8_t* out, size_t len);
} knx_flash_hal_ops_t;

typedef struct {
    void* context;
    knx_flash_hal_geometry_t geometry;
    knx_flash_hal_ops_t ops;
} knx_flash_hal_t;

static inline bool knx_flash_hal_is_valid(const knx_flash_hal_t* hal)
{
    if (!hal || !hal->ops.erase_page || !hal->ops.program || !hal->ops.read) {
        return false;
    }
    const knx_flash_hal_geometry_t* g = &hal->geometry;
    const uint32_t unit = g->write_unit;
    return g->page_size > 0 && g->page_count >= 2 && (g->page_count % 2u) == 0 && unit >= 1 && unit <= 32 &&
           (unit & (unit - 1u)) == 0 && (g->page_size % unit) == 0;
}

#ifdef __cplusplus
}
#endif
