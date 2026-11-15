// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file flash_hal_virtual.hpp
 * @brief Host flash simulator behind knx_flash_hal_t, with power cuts
 *
 * Host only (KNX_SOURCES_VIRTUAL). It holds the region in memory and enforces
 * the flash_hal.h contract: aligned programs inside one page, and no write
 * unit programmed twice between erases (a violation is counted, and the
 * program fails).
 *
 * Power cuts. Every programmed write unit and every page erase is one step.
 * cutPowerAfter(n) lets n steps complete; the next one is torn and every call
 * after it fails until powerOn(), which is the reboot. A torn program leaves
 * its write unit with only some of the bits cleared that it was clearing; a
 * torn erase leaves the first half of the page erased and the rest as it was.
 * The memory survives the reboot, so a test mounts a fresh FlashLog on it and
 * checks what was kept.
 *
 * @thread_safety NOT thread-safe; one test thread.
 */

#pragma once

#include "knx/objects/flash_hal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace knx {
namespace objects {

class FlashHalVirtual {
public:
    explicit FlashHalVirtual(const knx_flash_hal_geometry_t& geometry);

    FlashHalVirtual(const FlashHalVirtual&) = delete;
    FlashHalVirtual& operator=(const FlashHalVirtual&) = delete;

    /// The HAL to hand to a FlashLog; points at this object.
    [[nodiscard]] knx_flash_hal_t hal();

    /// Allow @p steps more steps, then tear the next one and fail every call.
    void cutPowerAfter(uint64_t steps);
    /// The reboot: lifts a cut (or a pending one). Memory is kept.
    void powerOn();
    [[nodiscard]] bool powerCut() const { return _cut; }

    /// The next program call fails without changing anything.
    void failNextProgram() { _failNextProgram = true; }

    /// Steps since construction (programmed write units plus page erases).
    [[nodiscard]] uint64_t steps() const { return _steps; }
    [[nodiscard]] uint64_t contractViolations() const { return _violations; }
    [[nodiscard]] uint32_t eraseCount(uint32_t page) const { return _erases.at(page); }

    [[nodiscard]] std::span<uint8_t> memory() { return _memory; }

private:
    static bool eraseThunk(void* context, uint32_t addr);
    static bool programThunk(void* context, uint32_t addr, const uint8_t* data, size_t len);
    static bool readThunk(void* context, uint32_t addr, uint8_t* out, size_t len);

    bool erasePage(uint32_t addr);
    bool program(uint32_t addr, const uint8_t* data, size_t len);
    bool read(uint32_t addr, uint8_t* out, size_t len) const;
    bool inRegion(uint32_t addr, size_t len) const;
    /// Takes one step; false when the power is (now) off.
    bool step();

    knx_flash_hal_geometry_t _geometry;
    std::vector<uint8_t> _memory;
    std::vector<uint32_t> _erases;
    uint64_t _steps = 0;
    uint64_t _violations = 0;
    std::optional<uint64_t> _stepsLeft;
    bool _cut = false;
    bool _tearThisStep = false;
    bool _failNextProgram = false;
};

} // namespace objects
} // namespace knx
