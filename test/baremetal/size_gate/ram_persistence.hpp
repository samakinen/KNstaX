// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file ram_persistence.hpp
 * @brief Volatile ObjectPersistence for the size gate
 *
 * Stands in for the flash backend (K5) so the gate runs the real
 * initialisation path. It is test code: its own size is not part of the
 * KNstaX share the gate reports, and it keeps nothing across a reset.
 */

#pragma once

#include "knx/objects/object_persistence.hpp"

#include <map>
#include <string>
#include <vector>

namespace size_gate {

#if defined(KNX_SIZE_GATE_SEMIHOST)
// The emulator variant keeps the records addressed by id in static storage, as
// a flash log keeps them in flash: a save allocates nothing, so the allocations
// the run lists belong to the stack and not to this stand-in. (The heap-backed
// version below is what the measured image links; it is not on the heap path.)
inline constexpr std::size_t kRamSlotCount = 40;
inline constexpr std::size_t kRamSlotBytes = 700;
struct RamSlot {
    uint16_t id = 0;
    uint16_t length = 0;
    bool used = false;
    uint8_t bytes[kRamSlotBytes];
};
inline RamSlot g_ramSlots[kRamSlotCount];
inline RamSlot* findRamSlot(uint16_t id) {
    for (auto& s : g_ramSlots) {
        if (s.used && s.id == id) return &s;
    }
    return nullptr;
}
#endif

class RamPersistence final : public knx::objects::ObjectPersistence {
public:
    knx::util::Result<void> init(const std::string&) override { return knx::util::Result<void>::ok(); }
    void close() override {}

    knx::util::Result<void> save(const std::string& key, std::span<const uint8_t> data) override {
        _named[key].assign(data.begin(), data.end());
        return knx::util::Result<void>::ok();
    }
    knx::util::Result<void> saveById(uint16_t id, std::span<const uint8_t> data) override {
#if defined(KNX_SIZE_GATE_SEMIHOST)
        if (data.size() > kRamSlotBytes) return knx::util::ErrorCode::BufferTooSmall;
        RamSlot* slot = findRamSlot(id);
        for (auto& s : g_ramSlots) {
            if (slot == nullptr && !s.used) slot = &s;
        }
        if (slot == nullptr) return knx::util::ErrorCode::StorageFull;
        slot->used = true;
        slot->id = id;
        slot->length = static_cast<uint16_t>(data.size());
        for (std::size_t i = 0; i < data.size(); ++i) slot->bytes[i] = data[i];
        return knx::util::Result<void>::ok();
#else
        _byId[id].assign(data.begin(), data.end());
        return knx::util::Result<void>::ok();
#endif
    }

    knx::util::Result<void> load(const std::string& key, std::vector<uint8_t>& out) override {
        const auto it = _named.find(key);
        if (it == _named.end()) return knx::util::ErrorCode::NotFound;
        out = it->second;
        return knx::util::Result<void>::ok();
    }
    knx::util::Result<void> loadById(uint16_t id, std::vector<uint8_t>& out) override {
#if defined(KNX_SIZE_GATE_SEMIHOST)
        const RamSlot* slot = findRamSlot(id);
        if (slot == nullptr) return knx::util::ErrorCode::NotFound;
        out.assign(slot->bytes, slot->bytes + slot->length);
        return knx::util::Result<void>::ok();
#else
        const auto it = _byId.find(id);
        if (it == _byId.end()) return knx::util::ErrorCode::NotFound;
        out = it->second;
        return knx::util::Result<void>::ok();
#endif
    }

    knx::util::Result<bool> exists(const std::string& key) override { return _named.contains(key); }
    knx::util::Result<bool> existsById(uint16_t id) override {
#if defined(KNX_SIZE_GATE_SEMIHOST)
        return findRamSlot(id) != nullptr;
#else
        return _byId.contains(id);
#endif
    }

    knx::util::Result<void> erase(const std::string& key) override {
        _named.erase(key);
        return knx::util::Result<void>::ok();
    }
    knx::util::Result<void> eraseById(uint16_t id) override {
#if defined(KNX_SIZE_GATE_SEMIHOST)
        if (RamSlot* slot = findRamSlot(id)) slot->used = false;
#else
        _byId.erase(id);
#endif
        return knx::util::Result<void>::ok();
    }

    knx::util::Result<void> eraseAll() override {
        _named.clear();
#if defined(KNX_SIZE_GATE_SEMIHOST)
        for (auto& s : g_ramSlots) s.used = false;
#else
        _byId.clear();
#endif
        return knx::util::Result<void>::ok();
    }
    std::vector<std::string> listKeys() override {
        std::vector<std::string> keys;
        for (const auto& entry : _named) keys.push_back(entry.first);
        return keys;
    }

private:
    std::map<std::string, std::vector<uint8_t>> _named;
    std::map<uint16_t, std::vector<uint8_t>> _byId;
};

}  // namespace size_gate
