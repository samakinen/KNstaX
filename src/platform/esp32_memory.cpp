// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_memory.cpp
 * @brief ESP32 NVS-backed memory implementation
 */

#include "esp32_memory.hpp"

#include "knx/util/log.hpp"

#include <algorithm>
#include <cstring>
#include <ranges>
#include <span>

static const char* TAG = "KNX.Memory";

namespace knx::platform {

Esp32Memory::Esp32Memory()
    : _nvsHandle(0)
    , _dirty(false)
    , _initialized(false)
{
    _buffer.assign(DEFAULT_SIZE, 0xFF);
}

Esp32Memory::~Esp32Memory()
{
    if (_initialized) {
        commit();
        nvs_close(_nvsHandle);
    }
}

util::Result<void> Esp32Memory::init()
{
    if (_initialized) {
        return util::Result<void>::ok();
    }

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &_nvsHandle);
    if (err != ESP_OK) {
        KNX_LOGE(TAG, "Failed to open NVS namespace '%s': %s", NVS_NAMESPACE, esp_err_to_name(err));
        return util::ErrorCode::ResourceUnavailable;
    }

    // First boot may not have persisted data yet; erased defaults are valid.
    (void)loadFromNvs();

    _initialized = true;
    return util::Result<void>::ok();
}

std::span<uint8_t> Esp32Memory::getBuffer(uint32_t address, size_t length)
{
    if (!_initialized || length == 0) {
        return {};
    }

    const auto bufferSpan = std::span(_buffer);
    if (address >= bufferSpan.size()) {
        return {};
    }

    const size_t addr = static_cast<size_t>(address);
    return bufferSpan.subspan(addr, std::min(length, bufferSpan.size() - addr));
}

std::span<const uint8_t> Esp32Memory::getBuffer(uint32_t address, size_t length) const
{
    if (!_initialized || length == 0) {
        return {};
    }

    const auto bufferSpan = std::span(_buffer);
    if (address >= bufferSpan.size()) {
        return {};
    }

    const size_t addr = static_cast<size_t>(address);
    return bufferSpan.subspan(addr, std::min(length, bufferSpan.size() - addr));
}

uint32_t Esp32Memory::read(uint32_t address, std::span<uint8_t> out)
{
    if (!_initialized || out.empty()) {
        return 0;
    }

    const auto src = getBuffer(address, out.size());
    if (src.empty()) {
        return 0;
    }

    std::memcpy(out.data(), src.data(), src.size());
    return static_cast<uint32_t>(src.size());
}

uint32_t Esp32Memory::write(uint32_t address, std::span<const uint8_t> in)
{
    if (!_initialized || in.empty()) {
        return 0;
    }

    auto dst = getBuffer(address, in.size());
    if (dst.empty()) {
        return 0;
    }

    std::memcpy(dst.data(), in.data(), dst.size());
    _dirty = true;
    return static_cast<uint32_t>(dst.size());
}

uint32_t Esp32Memory::write(uint32_t address, uint8_t value, size_t count)
{
    if (!_initialized || count == 0) {
        return 0;
    }

    auto dst = getBuffer(address, count);
    if (dst.empty()) {
        return 0;
    }

    std::ranges::fill(dst, value);
    _dirty = true;
    return static_cast<uint32_t>(dst.size());
}

util::Result<void> Esp32Memory::erase(uint32_t address, size_t length)
{
    if (!_initialized || length == 0 || static_cast<size_t>(address) + length > _buffer.size()) {
        return util::ErrorCode::InvalidParameter;
    }

    std::ranges::fill(std::span(_buffer).subspan(address, length), 0xFF);
    _dirty = true;
    return util::Result<void>::ok();
}

void Esp32Memory::commit()
{
    if (!_initialized || !_dirty) {
        return;
    }

    if (!saveToNvs()) {
        KNX_LOGE(TAG, "Commit failed");
        return;
    }

    _dirty = false;
}

bool Esp32Memory::loadFromNvs()
{
    std::size_t requiredSize = _buffer.size();
    esp_err_t err = nvs_get_blob(_nvsHandle, NVS_KEY_DATA, _buffer.data(), &requiredSize);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }

    if (err != ESP_OK) {
        KNX_LOGE(TAG, "Failed to read from NVS: %s", esp_err_to_name(err));
        return false;
    }

    if (requiredSize != _buffer.size()) {
        KNX_LOGW(TAG, "NVS blob size mismatch (%u != %u), keeping defaults",
                 static_cast<unsigned>(requiredSize),
                 static_cast<unsigned>(_buffer.size()));
        return false;
    }

    return true;
}

bool Esp32Memory::saveToNvs()
{
    esp_err_t err = nvs_set_blob(_nvsHandle, NVS_KEY_DATA, _buffer.data(), _buffer.size());
    if (err != ESP_OK) {
        KNX_LOGE(TAG, "Failed to write to NVS: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_commit(_nvsHandle);
    if (err != ESP_OK) {
        KNX_LOGE(TAG, "Failed to commit NVS write: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

} // namespace knx::platform
