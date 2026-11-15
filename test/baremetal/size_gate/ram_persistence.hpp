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

class RamPersistence final : public knx::objects::ObjectPersistence {
public:
    knx::util::Result<void> init(const std::string&) override { return knx::util::Result<void>::ok(); }
    void close() override {}

    knx::util::Result<void> save(const std::string& key, std::span<const uint8_t> data) override {
        _named[key].assign(data.begin(), data.end());
        return knx::util::Result<void>::ok();
    }
    knx::util::Result<void> saveById(uint16_t id, std::span<const uint8_t> data) override {
        _byId[id].assign(data.begin(), data.end());
        return knx::util::Result<void>::ok();
    }
    knx::util::Result<void> load(const std::string& key, std::vector<uint8_t>& out) override {
        const auto it = _named.find(key);
        if (it == _named.end()) return knx::util::ErrorCode::NotFound;
        out = it->second;
        return knx::util::Result<void>::ok();
    }
    knx::util::Result<void> loadById(uint16_t id, std::vector<uint8_t>& out) override {
        const auto it = _byId.find(id);
        if (it == _byId.end()) return knx::util::ErrorCode::NotFound;
        out = it->second;
        return knx::util::Result<void>::ok();
    }
    knx::util::Result<bool> exists(const std::string& key) override { return _named.contains(key); }
    knx::util::Result<bool> existsById(uint16_t id) override { return _byId.contains(id); }
    knx::util::Result<void> erase(const std::string& key) override {
        _named.erase(key);
        return knx::util::Result<void>::ok();
    }
    knx::util::Result<void> eraseById(uint16_t id) override {
        _byId.erase(id);
        return knx::util::Result<void>::ok();
    }
    knx::util::Result<void> eraseAll() override {
        _named.clear();
        _byId.clear();
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
