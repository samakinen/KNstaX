// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file flash_erase_guard.hpp
 * @brief Lets a medium prepare for a flash page erase that stops its receive path
 *
 * On a small MCU an erase can stop code running from flash for tens of
 * milliseconds. A product that does not keep its receive interrupt in RAM
 * (KNX_ISR_RAM) masks interrupts for the erase instead, and the medium then
 * loses whatever arrives meanwhile. A medium that can make the bus repeat
 * those frames implements this interface; FlashLog calls it around every
 * page erase (FlashLog::setEraseGuard()).
 *
 * The TP-UART2 backend is one: it puts the chip into busy mode, so frames
 * addressed to the device are answered BUSY and repeated after the erase.
 *
 * @thread_safety Main context only, like FlashLog.
 */

#pragma once

namespace knx {
namespace platform {

class FlashEraseGuard {
public:
    virtual ~FlashEraseGuard() = default;

    /**
     * @brief Prepare for an erase; called just before the board erases a page
     *
     * @param mustProceed The caller cannot wait (formatting at mount, a
     *                    factory reset): prepare as well as possible now and
     *                    return true.
     * @return true when the erase may start; false asks the caller to try
     *         again later (only when mustProceed is false). After true,
     *         endFlashErase() follows once the erase is over.
     */
    virtual bool beginFlashErase(bool mustProceed) = 0;

    /// The erase is over and interrupts run again.
    virtual void endFlashErase() = 0;
};

} // namespace platform
} // namespace knx
