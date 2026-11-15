// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file flash_log_persistence.hpp
 * @brief ObjectPersistence over a FlashLog, for MCUs without NVS
 *
 * The stack stores its state through the id-keyed half of ObjectPersistence
 * (saveById, loadById and so on); this adapter maps those onto a FlashLog. The
 * string-keyed half is for hosts and NVS and returns OperationNotSupported.
 *
 * A product owns one FlashLog for its flash region and registers a factory
 * that returns an adapter over it:
 *
 * @code
 * static knx::objects::FlashLog g_log(board::flashHal());
 * objects::setPersistenceFactory([]() -> std::unique_ptr<objects::ObjectPersistence> {
 *     return std::make_unique<objects::FlashLogPersistence>(g_log);
 * });
 * // main loop: app->loop(); g_log.service();
 * @endcode
 *
 * Every adapter over the same FlashLog sees the same records: the region holds
 * one namespace, and init() ignores the name it is given. init() mounts the
 * log if nobody has.
 *
 * @thread_safety NOT thread-safe; as FlashLog.
 */

#pragma once

#include "knx/objects/flash_log.hpp"
#include "knx/objects/object_persistence.hpp"

namespace knx {
namespace objects {

class FlashLogPersistence final : public ObjectPersistence {
public:
    /// @param log Must outlive the adapter.
    explicit FlashLogPersistence(FlashLog& log) : _log(log) {}

    util::Result<void> init(const std::string& namespace_name) override;
    void close() override {}

    util::Result<void> saveById(uint16_t keyId, std::span<const uint8_t> data) override;
    util::Result<void> loadById(uint16_t keyId, std::vector<uint8_t>& data) override;
    util::Result<bool> existsById(uint16_t keyId) override;
    util::Result<void> eraseById(uint16_t keyId) override;
    util::Result<void> eraseAll() override;

    /// The ids that have a record, named as the other backends name them ("d" and four hex digits).
    std::vector<std::string> listKeys() override;

    /// String keys are not stored: OperationNotSupported.
    util::Result<void> save(const std::string& key, std::span<const uint8_t> data) override;
    /// String keys are not stored: OperationNotSupported.
    util::Result<void> load(const std::string& key, std::vector<uint8_t>& data) override;
    /// String keys are not stored: OperationNotSupported.
    util::Result<bool> exists(const std::string& key) override;
    /// String keys are not stored: OperationNotSupported.
    util::Result<void> erase(const std::string& key) override;

private:
    FlashLog& _log;
};

} // namespace objects
} // namespace knx
