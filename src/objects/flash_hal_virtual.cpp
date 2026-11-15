// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file flash_hal_virtual.cpp
 * @brief FlashHalVirtual: host flash simulator (see flash_hal_virtual.hpp)
 */

#include "knx/objects/flash_hal_virtual.hpp"

#include <algorithm>
#include <cstring>

namespace knx {
namespace objects {

namespace {
constexpr uint8_t ERASED = 0xFFu;
// The bits a torn program has not cleared yet: alternate ones.
constexpr uint8_t TORN_PROGRAM_MASK = 0xAAu;
}  // namespace

FlashHalVirtual::FlashHalVirtual(const knx_flash_hal_geometry_t& geometry)
    : _geometry(geometry),
      _memory(static_cast<size_t>(geometry.page_size) * geometry.page_count, ERASED),
      _erases(geometry.page_count, 0u)
{
}

knx_flash_hal_t FlashHalVirtual::hal()
{
    knx_flash_hal_t hal{};
    hal.context = this;
    hal.geometry = _geometry;
    hal.ops.erase_page = &eraseThunk;
    hal.ops.program = &programThunk;
    hal.ops.read = &readThunk;
    return hal;
}

void FlashHalVirtual::cutPowerAfter(uint64_t steps) { _stepsLeft = steps; }

void FlashHalVirtual::powerOn()
{
    _stepsLeft.reset();
    _cut = false;
    _tearThisStep = false;
}

bool FlashHalVirtual::step()
{
    if (_cut) {
        return false;
    }
    ++_steps;
    if (_stepsLeft.has_value()) {
        if (*_stepsLeft == 0u) {
            _cut = true;
            _tearThisStep = true;
            return false;
        }
        --*_stepsLeft;
    }
    return true;
}

bool FlashHalVirtual::inRegion(uint32_t addr, size_t len) const
{
    return addr >= _geometry.base && addr - _geometry.base + len <= _memory.size();
}

bool FlashHalVirtual::erasePage(uint32_t addr)
{
    if (!inRegion(addr, _geometry.page_size) || (addr - _geometry.base) % _geometry.page_size != 0u) {
        ++_violations;
        return false;
    }
    const size_t offset = addr - _geometry.base;
    const uint32_t page = static_cast<uint32_t>(offset / _geometry.page_size);
    if (!step()) {
        if (_tearThisStep) {
            _tearThisStep = false;
            ++_erases[page];
            std::fill_n(_memory.begin() + static_cast<std::ptrdiff_t>(offset), _geometry.page_size / 2u, ERASED);
        }
        return false;
    }
    ++_erases[page];
    std::fill_n(_memory.begin() + static_cast<std::ptrdiff_t>(offset), _geometry.page_size, ERASED);
    return true;
}

bool FlashHalVirtual::program(uint32_t addr, const uint8_t* data, size_t len)
{
    const uint32_t unit = _geometry.write_unit;
    const size_t offset = addr - _geometry.base;
    if (data == nullptr || len == 0u || !inRegion(addr, len) || offset % unit != 0u || len % unit != 0u ||
        offset / _geometry.page_size != (offset + len - 1u) / _geometry.page_size) {
        ++_violations;
        return false;
    }
    if (_failNextProgram) {
        _failNextProgram = false;
        return false;
    }
    for (size_t done = 0; done < len; done += unit) {
        uint8_t* target = _memory.data() + offset + done;
        if (std::any_of(target, target + unit, [](uint8_t b) { return b != ERASED; })) {
            ++_violations;  // programmed twice without an erase
            return false;
        }
        if (!step()) {
            if (_tearThisStep) {
                _tearThisStep = false;
                for (uint32_t i = 0; i < unit; ++i) {
                    target[i] = static_cast<uint8_t>(target[i] & (data[done + i] | TORN_PROGRAM_MASK));
                }
            }
            return false;
        }
        for (uint32_t i = 0; i < unit; ++i) {
            target[i] = static_cast<uint8_t>(target[i] & data[done + i]);
        }
    }
    return true;
}

bool FlashHalVirtual::read(uint32_t addr, uint8_t* out, size_t len) const
{
    if (_cut || out == nullptr || !inRegion(addr, len)) {
        return false;
    }
    std::memcpy(out, _memory.data() + (addr - _geometry.base), len);
    return true;
}

bool FlashHalVirtual::eraseThunk(void* context, uint32_t addr)
{
    return static_cast<FlashHalVirtual*>(context)->erasePage(addr);
}

bool FlashHalVirtual::programThunk(void* context, uint32_t addr, const uint8_t* data, size_t len)
{
    return static_cast<FlashHalVirtual*>(context)->program(addr, data, len);
}

bool FlashHalVirtual::readThunk(void* context, uint32_t addr, uint8_t* out, size_t len)
{
    return static_cast<FlashHalVirtual*>(context)->read(addr, out, len);
}

} // namespace objects
} // namespace knx
