// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file flash_log_persistence.cpp
 * @brief FlashLogPersistence: ObjectPersistence over a FlashLog
 */

#include "knx/objects/flash_log_persistence.hpp"

namespace knx {
namespace objects {

util::Result<void> FlashLogPersistence::init(const std::string&)
{
    if (_log.mounted()) {
        return util::Result<void>::ok();
    }
    return _log.mount();
}

util::Result<void> FlashLogPersistence::saveById(uint16_t keyId, std::span<const uint8_t> data)
{
    return _log.save(keyId, data);
}

util::Result<void> FlashLogPersistence::loadById(uint16_t keyId, std::vector<uint8_t>& data)
{
    return _log.load(keyId, data);
}

util::Result<bool> FlashLogPersistence::existsById(uint16_t keyId) { return _log.exists(keyId); }

util::Result<void> FlashLogPersistence::eraseById(uint16_t keyId) { return _log.erase(keyId); }

util::Result<void> FlashLogPersistence::eraseAll() { return _log.eraseAll(); }

std::vector<std::string> FlashLogPersistence::listKeys()
{
    std::vector<uint16_t> ids;
    std::vector<std::string> keys;
    if (_log.listIds(ids).isError()) {
        return keys;
    }
    static constexpr char HEX[] = "0123456789ABCDEF";
    keys.reserve(ids.size());
    for (const uint16_t id : ids) {
        std::string key = "d0000";
        for (int digit = 0; digit < 4; ++digit) {
            key[static_cast<size_t>(4 - digit)] = HEX[(id >> (4 * digit)) & 0xFu];
        }
        keys.push_back(std::move(key));
    }
    return keys;
}

util::Result<void> FlashLogPersistence::save(const std::string&, std::span<const uint8_t>)
{
    return util::ErrorCode::OperationNotSupported;
}

util::Result<void> FlashLogPersistence::load(const std::string&, std::vector<uint8_t>&)
{
    return util::ErrorCode::OperationNotSupported;
}

util::Result<bool> FlashLogPersistence::exists(const std::string&) { return util::ErrorCode::OperationNotSupported; }

util::Result<void> FlashLogPersistence::erase(const std::string&) { return util::ErrorCode::OperationNotSupported; }

} // namespace objects
} // namespace knx
