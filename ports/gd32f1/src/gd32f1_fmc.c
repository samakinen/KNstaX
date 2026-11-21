/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GD32F1 FMC driver for KNstaX persistence; see gd32f1_fmc.h.
 *
 * Registers from the GD32F10x user manual, FMC chapter (bank 0; the same
 * layout as the STM32F1 FLASH block). */
#include "gd32f1_fmc.h"

#include <stdint.h>
#include <string.h>

#define FMC_REG(offset) (*(volatile uint32_t*)(0x40022000u + (offset)))
#define FMC_KEY FMC_REG(0x04u)
#define FMC_STAT FMC_REG(0x0Cu)
#define FMC_CTL FMC_REG(0x10u)
#define FMC_ADDR FMC_REG(0x14u)

#define STAT_BUSY (1u << 0)
#define STAT_PGERR (1u << 2)
#define STAT_WPERR (1u << 4)
#define STAT_ENDF (1u << 5)
#define STAT_CLEAR (STAT_PGERR | STAT_WPERR | STAT_ENDF)

#define CTL_PG (1u << 0)
#define CTL_PER (1u << 1)
#define CTL_START (1u << 6)
#define CTL_LK (1u << 7)

#define UNLOCK_KEY0 0x45670123u
#define UNLOCK_KEY1 0xCDEF89ABu

#define WRITE_UNIT 2u
#define REGION_END (GD32F1_PERSIST_BASE + GD32F1_PERSIST_PAGE_SIZE * GD32F1_PERSIST_PAGE_COUNT)

/* long_call: RAM is out of BL range from flash. */
#define FMC_RAMFUNC __attribute__((section(".ramfunc"), noinline, long_call))

/* Defence against a stack bug: never touch flash outside the region (the
 * code lives just below it). */
static int in_region(uint32_t addr, size_t len)
{
    return addr >= GD32F1_PERSIST_BASE && addr <= REGION_END && len <= REGION_END - addr;
}

static void unlock(void)
{
    if (FMC_CTL & CTL_LK) {
        FMC_KEY = UNLOCK_KEY0;
        FMC_KEY = UNLOCK_KEY1;
    }
}

static void lock(void) { FMC_CTL = CTL_LK; }

/* From the write that starts the erase to the end of the busy-wait, nothing
 * may be fetched from flash: this runs from RAM and calls nothing. Interrupts
 * are masked for the whole erase, so no handler is fetched from flash either
 * (measured: an interrupt taken while flash erases can lock the chip, even
 * with the busy-wait in RAM, unless its vector and handler are in RAM too). */
FMC_RAMFUNC static uint32_t erase_in_ram(uint32_t addr)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
    FMC_CTL = CTL_PER;
    FMC_ADDR = addr;
    FMC_CTL = CTL_PER | CTL_START;
    while (FMC_STAT & STAT_BUSY) {
    }
    FMC_CTL = 0u;
    const uint32_t stat = FMC_STAT;
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
    return stat;
}

static bool erase_page(void* context, uint32_t addr)
{
    (void)context;
    if ((addr - GD32F1_PERSIST_BASE) % GD32F1_PERSIST_PAGE_SIZE != 0u ||
        !in_region(addr, GD32F1_PERSIST_PAGE_SIZE)) {
        return false;
    }
    unlock();
    FMC_STAT = STAT_CLEAR;
    const uint32_t stat = erase_in_ram(addr);
    lock();
    return (stat & (STAT_PGERR | STAT_WPERR)) == 0u;
}

static bool program(void* context, uint32_t addr, const uint8_t* data, size_t len)
{
    (void)context;
    if ((addr | len) % WRITE_UNIT != 0u || !in_region(addr, len)) {
        return false;
    }
    unlock();
    FMC_STAT = STAT_CLEAR;
    FMC_CTL = CTL_PG;
    uint32_t errors = 0u;
    for (size_t i = 0; i < len; i += 2u) {
        /* data may be unaligned; the flash takes one half-word store */
        const uint16_t half = (uint16_t)(data[i] | ((uint16_t)data[i + 1u] << 8));
        *(volatile uint16_t*)(addr + i) = half;
        while (FMC_STAT & STAT_BUSY) {
        }
        errors |= FMC_STAT & (STAT_PGERR | STAT_WPERR);
        if (errors != 0u) {
            break;
        }
    }
    lock();
    return errors == 0u;
}

static bool read(void* context, uint32_t addr, uint8_t* out, size_t len)
{
    (void)context;
    if (!in_region(addr, len)) {
        return false;
    }
    memcpy(out, (const void*)addr, len);
    return true;
}

knx_flash_hal_t gd32f1_fmc_hal(void)
{
    knx_flash_hal_t hal;
    hal.context = 0;
    hal.geometry.base = GD32F1_PERSIST_BASE;
    hal.geometry.page_size = GD32F1_PERSIST_PAGE_SIZE;
    hal.geometry.page_count = GD32F1_PERSIST_PAGE_COUNT;
    hal.geometry.write_unit = WRITE_UNIT;
    hal.ops.erase_page = erase_page;
    hal.ops.program = program;
    hal.ops.read = read;
    return hal;
}
