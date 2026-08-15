// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_memory.hpp
 * @brief ESP32 NVS memory implementation
 */

#pragma once

#include "knx/platform/memory_interface.hpp"

#include "nvs_flash.h"
#include "nvs.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace knx::platform {

/**
 * @brief ESP32 NVS-based memory implementation
 *
 * Uses ESP32 Non-Volatile Storage for KNX configuration data.
 *
 * NOTE:
 * Any std::span returned by getBuffer() is a non-owning view into
 * internal storage and is invalidated if the buffer reallocates.
 */
class Esp32Memory final : public MemoryInterface {
public:
    Esp32Memory();
    ~Esp32Memory() override;

    Esp32Memory(const Esp32Memory&) = delete;
    Esp32Memory& operator=(const Esp32Memory&) = delete;
    Esp32Memory(Esp32Memory&&) = delete;
    Esp32Memory& operator=(Esp32Memory&&) = delete;

    // ---------------------------------------------------------------------
    // MemoryInterface implementation
    // ---------------------------------------------------------------------

    [[nodiscard]] MemoryType type() const override { return MemoryType::NVS; }
    [[nodiscard]] size_t size() const override { return _buffer.size(); }
    [[nodiscard]] size_t pageSize() const override { return 32; }
    [[nodiscard]] size_t eraseBlockSize() const override { return 4096; }
    [[nodiscard]] util::Result<void> init() override;
    [[nodiscard]] uint32_t read(uint32_t address,
                  std::span<uint8_t> buffer) override;
    [[nodiscard]] uint32_t write(uint32_t address,
                   std::span<const uint8_t> buffer) override;
    [[nodiscard]] uint32_t write(uint32_t address,
                   uint8_t value,
                   size_t repeat) override;
    void commit() override;
    [[nodiscard]] util::Result<void> erase(uint32_t address,
                             size_t length) override;
    [[nodiscard]] std::span<uint8_t> getBuffer(uint32_t address,
                                 size_t length) override;
    [[nodiscard]] std::span<const uint8_t> getBuffer(uint32_t address,
                                       size_t length) const;

private:
    static constexpr const char* NVS_NAMESPACE = "knx";
    static constexpr const char* NVS_KEY_DATA  = "data";
    static constexpr size_t DEFAULT_SIZE       = 4096;

    bool loadFromNvs();
    bool saveToNvs();

private:
    nvs_handle_t _nvsHandle{};
    std::vector<uint8_t> _buffer;

    bool _dirty{false};
    bool _initialized{false};
};

} // namespace knx::platform