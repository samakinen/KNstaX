/* SPDX-License-Identifier: GPL-3.0-or-later */
/* GD32F1 reference port: the FMC as KNstaX's knx_flash_hal_t
 * (include/knx/objects/flash_hal.h), for FlashLog persistence.
 *
 * The region is GD32F1_PERSIST_PAGE_COUNT pages of GD32F1_PERSIST_PAGE_SIZE at
 * GD32F1_PERSIST_BASE (gd32f1_config.h), programmed in half-words. FlashLog
 * owns the format; this driver only erases, programs and reads, and refuses
 * any address outside the region.
 *
 * An erase stalls every flash read for the whole erase (37-39 ms for 1 KiB
 * on a GD32F103, measured), and an interrupt taken meanwhile from flash can
 * lock the chip. The driver starts the erase and waits for it from RAM
 * (.ramfunc, about 50 bytes) with interrupts masked, so vectors and handlers
 * can stay in flash. Octets the UART receives meanwhile are lost: pass the
 * TP-UART backend to FlashLog::setEraseGuard() so the chip answers BUSY to
 * frames addressed to the device for that time. Programming a half-word
 * stalls flash for about 100 us; that loop stays in flash with interrupts
 * enabled (measured safe).
 *
 * The linker script must place .ramfunc in the initialised data region.
 * Main context only (KNstaX calls the HAL from its loop); not reentrant.
 */
#pragma once

#include "gd32f1_config.h"
#include "knx/objects/flash_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

knx_flash_hal_t gd32f1_fmc_hal(void);

#ifdef __cplusplus
}
#endif
