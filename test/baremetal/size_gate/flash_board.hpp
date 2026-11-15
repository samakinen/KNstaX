// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file flash_board.hpp
 * @brief The board's flash for the reference device: what FlashLog runs on
 *
 * The geometry is the H8i8o's: two banks of four 1 KiB pages, written in
 * half-words. The measured image gets a stand-in for a flash controller,
 * enough to link FlashLog as a product would (it is linked, not run). The
 * emulator variant keeps the region in RAM, so the device's persistence
 * really runs, through the ETS download and its save.
 */

#pragma once

#include "knx/objects/flash_hal.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace size_gate::flash {

inline constexpr uint32_t kPageSize = 1024u;
inline constexpr uint32_t kPageCount = 8u;
inline constexpr uint32_t kWriteUnit = 2u;

#if defined(KNX_SIZE_GATE_SEMIHOST)

alignas(4) inline uint8_t g_region[kPageSize * kPageCount];

inline uint8_t* at(uint32_t addr) { return reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(addr)); }

inline bool erasePage(void*, uint32_t addr) {
    std::memset(at(addr), 0xFF, kPageSize);
    return true;
}

inline bool program(void*, uint32_t addr, const uint8_t* data, size_t len) {
    uint8_t* target = at(addr);
    for (size_t i = 0; i < len; ++i) target[i] = static_cast<uint8_t>(target[i] & data[i]);
    return true;
}

inline uint32_t base() { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_region)); }

#else

// Stand-ins for a flash controller's address, data and control registers.
inline volatile uint32_t g_address = 0;
inline volatile uint16_t g_halfWord = 0;
inline volatile uint32_t g_control = 0;

inline bool erasePage(void*, uint32_t addr) {
    g_address = addr;
    g_control = 2u;
    return g_control == 0u;
}

inline bool program(void*, uint32_t addr, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i += 2u) {
        g_address = addr + static_cast<uint32_t>(i);
        g_halfWord = static_cast<uint16_t>(data[i] | (data[i + 1u] << 8));
    }
    return g_control == 0u;
}

// The last 8 KiB of a 128 KiB part.
inline uint32_t base() { return 0x0801E000u; }

#endif

inline bool read(void*, uint32_t addr, uint8_t* out, size_t len) {
    std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(addr)), len);
    return true;
}

inline knx_flash_hal_t makeHal() {
    knx_flash_hal_t hal{};
    hal.context = nullptr;
    hal.geometry = {base(), kPageSize, kPageCount, kWriteUnit};
    hal.ops.erase_page = &erasePage;
    hal.ops.program = &program;
    hal.ops.read = &read;
    return hal;
}

} // namespace size_gate::flash
