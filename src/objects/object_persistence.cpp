// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file object_persistence.cpp
 * @brief Object persistence implementation
 */

#include "knx/objects/object_persistence.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include <algorithm>
#include <array>
#include <cstdio>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
#else
#include <fstream>
#include <filesystem>
#include <map>
#include <cstdlib>
#include <string>
#include <unistd.h>
#endif

static const char* TAG = "KNX.Persistence";

namespace knx {
namespace objects {

#ifndef ESP_PLATFORM
namespace {

/// Default root when KNX_NVS_ROOT is unset. A pure function of the PID, so the
/// exit handler below can re-derive it without touching any static state.
std::string autoRootDir()
{
    return "/tmp/knx_nvs/p" + std::to_string(static_cast<long>(::getpid()));
}

/// Resolved once per process: wipes anything a recycled PID left behind and
/// arranges for the directory not to outlive the run.
const std::string& resolvedRootDir()
{
    static const std::string root = []() {
        if (const char* override_root = std::getenv("KNX_NVS_ROOT")) {
            if (override_root[0] != '\0') {
                // Caller-pinned location: leave its lifetime to the caller.
                return std::string(override_root);
            }
        }
        std::error_code ec;
        std::filesystem::remove_all(autoRootDir(), ec);
        std::atexit([]() {
            std::error_code cleanupEc;
            std::filesystem::remove_all(autoRootDir(), cleanupEc);
        });
        return autoRootDir();
    }();
    return root;
}

} // namespace
#endif

std::string persistenceRootDir()
{
#ifdef ESP_PLATFORM
    // NVS-backed; no filesystem root involved.
    return std::string();
#else
    // Per-process by default. Host test binaries share namespace names (several
    // use "knx_objects") and wipe the namespace on startup, so a single shared
    // root means concurrently running tests delete each other's state.
    return resolvedRootDir();
#endif
}

std::string persistenceNamespaceDir(const std::string& namespace_name)
{
    const std::string root = persistenceRootDir();
    if (root.empty()) {
        return namespace_name;
    }
    return root + "/" + namespace_name;
}

namespace {

constexpr std::array<char, 6> makeCompactKey(uint16_t keyId)
{
    std::array<char, 6> key{};
    std::snprintf(key.data(), key.size(), "d%04X", static_cast<unsigned>(keyId));
    return key;
}

} // namespace

#ifdef ESP_PLATFORM
/**
 * @brief ESP32 NVS-based persistence implementation
 */
class NvsPersistence : public ObjectPersistence {
public:
    NvsPersistence() : _handle(0), _initialized(false) {}
    
    ~NvsPersistence() override {
        close();
    }
    
    util::Result<void> init(const std::string& namespace_name) override {
        if (_initialized) {
            return util::Result<void>::ok();
        }
        
        // Initialize NVS
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            // NVS partition was truncated, erase and re-init
            ESP_ERROR_CHECK(nvs_flash_erase());
            err = nvs_flash_init();
        }
        
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to init NVS: %d", err);
            return util::Result<void>::err(util::ErrorCode::OperationFailed);
        }
        
        // Open NVS handle
        err = nvs_open(namespace_name.c_str(), NVS_READWRITE, &_handle);
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to open NVS namespace '%s': %s (%d)",
                     namespace_name.c_str(), esp_err_to_name(err), err);
            return util::Result<void>::err(util::ErrorCode::OperationFailed);
        }
        
        _namespace = namespace_name;
        _initialized = true;
        
        KNX_LOGD(TAG, "NVS persistence initialized (namespace: %s)", namespace_name.c_str());
        return util::Result<void>::ok();
    }
    
    void close() override {
        if (_initialized && _handle != 0) {
            nvs_close(_handle);
            _handle = 0;
            _initialized = false;
        }
    }
    
    PersistenceResult save(const std::string& key, std::span<const uint8_t> data) override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }

        static constexpr uint8_t kEmptyBlobByte = 0;
        const void* blobData = data.empty() ? static_cast<const void*>(&kEmptyBlobByte)
                                            : static_cast<const void*>(data.data());
        esp_err_t err = nvs_set_blob(_handle, key.c_str(), blobData, data.size());
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to save key '%s': %d", key.c_str(), err);
            return (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) ? 
                   PersistenceResult::StorageFull : PersistenceResult::WriteError;
        }
        
        // Commit to flash
        err = nvs_commit(_handle);
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to commit key '%s': %d", key.c_str(), err);
            return PersistenceResult::WriteError;
        }
        
        KNX_LOGD(TAG, "Saved key '%s' (%zu bytes)", key.c_str(), data.size());
        return PersistenceResult::Success;
    }

    PersistenceResult saveById(uint16_t keyId, std::span<const uint8_t> data) override {
        const auto key = makeCompactKey(keyId);
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }

        static constexpr uint8_t kEmptyBlobByte = 0;
        const void* blobData = data.empty() ? static_cast<const void*>(&kEmptyBlobByte)
                                            : static_cast<const void*>(data.data());
        esp_err_t err = nvs_set_blob(_handle, key.data(), blobData, data.size());
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to save id '%s': %d", key.data(), err);
            return (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) ?
                   PersistenceResult::StorageFull : PersistenceResult::WriteError;
        }

        err = nvs_commit(_handle);
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to commit id '%s': %d", key.data(), err);
            return PersistenceResult::WriteError;
        }

        KNX_LOGD(TAG, "Saved id '%s' (%zu bytes)", key.data(), data.size());
        return PersistenceResult::Success;
    }
    
    PersistenceResult load(const std::string& key, std::vector<uint8_t>& data) override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }
        
        // Get blob size
        size_t required_size = 0;
        esp_err_t err = nvs_get_blob(_handle, key.c_str(), nullptr, &required_size);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return PersistenceResult::NotFound;
        }
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to get size for key '%s': %d", key.c_str(), err);
            return PersistenceResult::ReadError;
        }
        
        // Read blob
        data.resize(required_size);
        err = nvs_get_blob(_handle, key.c_str(), data.data(), &required_size);
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to load key '%s': %d", key.c_str(), err);
            return PersistenceResult::ReadError;
        }
        
        KNX_LOGD(TAG, "Loaded key '%s' (%zu bytes)", key.c_str(), data.size());
        return PersistenceResult::Success;
    }

    PersistenceResult loadById(uint16_t keyId, std::vector<uint8_t>& data) override {
        const auto key = makeCompactKey(keyId);
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }

        size_t required_size = 0;
        esp_err_t err = nvs_get_blob(_handle, key.data(), nullptr, &required_size);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return PersistenceResult::NotFound;
        }
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to get size for id '%s': %d", key.data(), err);
            return PersistenceResult::ReadError;
        }

        data.resize(required_size);
        err = nvs_get_blob(_handle, key.data(), data.data(), &required_size);
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to load id '%s': %d", key.data(), err);
            return PersistenceResult::ReadError;
        }

        KNX_LOGD(TAG, "Loaded id '%s' (%zu bytes)", key.data(), data.size());
        return PersistenceResult::Success;
    }
    
    util::Result<bool> exists(const std::string& key) override {
        if (!_initialized) {
            return util::Result<bool>(util::ErrorCode::NotInitialized);
        }
        
        size_t required_size = 0;
        esp_err_t err = nvs_get_blob(_handle, key.c_str(), nullptr, &required_size);
        return util::Result<bool>(err == ESP_OK);
    }

    util::Result<bool> existsById(uint16_t keyId) override {
        const auto key = makeCompactKey(keyId);
        if (!_initialized) {
            return util::Result<bool>(util::ErrorCode::NotInitialized);
        }

        size_t required_size = 0;
        esp_err_t err = nvs_get_blob(_handle, key.data(), nullptr, &required_size);
        return util::Result<bool>(err == ESP_OK);
    }
    
    PersistenceResult erase(const std::string& key) override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }
        
        esp_err_t err = nvs_erase_key(_handle, key.c_str());
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return PersistenceResult::NotFound;
        }
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to erase key '%s': %d", key.c_str(), err);
            return PersistenceResult::WriteError;
        }
        
        nvs_commit(_handle);
        KNX_LOGD(TAG, "Erased key '%s'", key.c_str());
        return PersistenceResult::Success;
    }

    PersistenceResult eraseById(uint16_t keyId) override {
        const auto key = makeCompactKey(keyId);
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }

        esp_err_t err = nvs_erase_key(_handle, key.data());
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return PersistenceResult::NotFound;
        }
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to erase id '%s': %d", key.data(), err);
            return PersistenceResult::WriteError;
        }

        nvs_commit(_handle);
        KNX_LOGD(TAG, "Erased id '%s'", key.data());
        return PersistenceResult::Success;
    }
    
    PersistenceResult eraseAll() override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }
        
        esp_err_t err = nvs_erase_all(_handle);
        if (err != ESP_OK) {
            KNX_LOGE(TAG, "Failed to erase all: %d", err);
            return PersistenceResult::WriteError;
        }
        
        nvs_commit(_handle);
        KNX_LOGI(TAG, "Erased all keys in namespace");
        return PersistenceResult::Success;
    }
    
    std::vector<std::string> listKeys() override {
        std::vector<std::string> keys;
        if (!_initialized) {
            return keys;
        }

        // Enumerate BLOB keys in this namespace.
        // KNstaX stores object state using nvs_set_blob(), so BLOB is sufficient.
        nvs_iterator_t it = nullptr;
        if (nvs_entry_find("nvs", _namespace.c_str(), NVS_TYPE_BLOB, &it) != ESP_OK) {
            return keys;
        }

        while (it != nullptr) {
            nvs_entry_info_t info{};
            nvs_entry_info(it, &info);
            keys.emplace_back(info.key);
            if (nvs_entry_next(&it) != ESP_OK) {
                break;
            }
        }
        nvs_release_iterator(it);

        std::sort(keys.begin(), keys.end());
        return keys;
    }

private:
    nvs_handle_t _handle;
    std::string _namespace;
    bool _initialized;
};

#else // Linux/other platforms

/**
 * @brief File-based persistence implementation
 */
class FilePersistence : public ObjectPersistence {
public:
    FilePersistence() : _initialized(false) {}
    
    ~FilePersistence() override {
        close();
    }
    
    util::Result<void> init(const std::string& namespace_name) override {
        if (_initialized) {
            return util::Result<void>::ok();
        }
        
        _directory = persistenceNamespaceDir(namespace_name);

        // Create directory
        std::error_code ec;
        std::filesystem::create_directories(_directory, ec);
        if (ec) {
            KNX_LOGE(TAG, "Failed to create directory '%s': %s", 
                     _directory.c_str(), ec.message().c_str());
            return util::Result<void>::err(util::ErrorCode::OperationFailed);
        }
        
        _initialized = true;
        KNX_LOGD(TAG, "File persistence initialized (directory: %s)", _directory.c_str());
        return util::Result<void>::ok();
    }
    
    void close() override {
        _initialized = false;
    }
    
    PersistenceResult save(const std::string& key, std::span<const uint8_t> data) override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }
        
        std::string filepath = _directory + "/" + key + ".bin";
        std::ofstream file(filepath, std::ios::binary);
        if (!file) {
            KNX_LOGE(TAG, "Failed to open file '%s' for writing", filepath.c_str());
            return PersistenceResult::WriteError;
        }
        
        if (!data.empty()) {
            file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        if (!file) {
            KNX_LOGE(TAG, "Failed to write file '%s'", filepath.c_str());
            return PersistenceResult::WriteError;
        }
        
        KNX_LOGD(TAG, "Saved key '%s' (%zu bytes)", key.c_str(), data.size());
        return PersistenceResult::Success;
    }

    PersistenceResult saveById(uint16_t keyId, std::span<const uint8_t> data) override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }

        const auto key = makeCompactKey(keyId);
        std::string filepath = _directory + "/" + std::string(key.data()) + ".bin";
        std::ofstream file(filepath, std::ios::binary);
        if (!file) {
            KNX_LOGE(TAG, "Failed to open file '%s' for writing", filepath.c_str());
            return PersistenceResult::WriteError;
        }

        if (!data.empty()) {
            file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }
        if (!file) {
            KNX_LOGE(TAG, "Failed to write file '%s'", filepath.c_str());
            return PersistenceResult::WriteError;
        }

        KNX_LOGD(TAG, "Saved id '%s' (%zu bytes)", key.data(), data.size());
        return PersistenceResult::Success;
    }
    
    PersistenceResult load(const std::string& key, std::vector<uint8_t>& data) override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }
        
        std::string filepath = _directory + "/" + key + ".bin";
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file) {
            return PersistenceResult::NotFound;
        }
        
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        
        data.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
            KNX_LOGE(TAG, "Failed to read file '%s'", filepath.c_str());
            return PersistenceResult::ReadError;
        }
        
        KNX_LOGD(TAG, "Loaded key '%s' (%zu bytes)", key.c_str(), data.size());
        return PersistenceResult::Success;
    }

    PersistenceResult loadById(uint16_t keyId, std::vector<uint8_t>& data) override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }

        const auto key = makeCompactKey(keyId);
        std::string filepath = _directory + "/" + std::string(key.data()) + ".bin";
        std::ifstream file(filepath, std::ios::binary | std::ios::ate);
        if (!file) {
            return PersistenceResult::NotFound;
        }

        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);

        data.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
            KNX_LOGE(TAG, "Failed to read file '%s'", filepath.c_str());
            return PersistenceResult::ReadError;
        }

        KNX_LOGD(TAG, "Loaded id '%s' (%zu bytes)", key.data(), data.size());
        return PersistenceResult::Success;
    }
    
    util::Result<bool> exists(const std::string& key) override {
        if (!_initialized) {
            return util::Result<bool>(util::ErrorCode::NotInitialized);
        }
        
        std::string filepath = _directory + "/" + key + ".bin";
        return util::Result<bool>(std::filesystem::exists(filepath));
    }

    util::Result<bool> existsById(uint16_t keyId) override {
        if (!_initialized) {
            return util::Result<bool>(util::ErrorCode::NotInitialized);
        }

        const auto key = makeCompactKey(keyId);
        std::string filepath = _directory + "/" + std::string(key.data()) + ".bin";
        return util::Result<bool>(std::filesystem::exists(filepath));
    }
    
    PersistenceResult erase(const std::string& key) override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }
        
        std::string filepath = _directory + "/" + key + ".bin";
        std::error_code ec;
        if (!std::filesystem::remove(filepath, ec)) {
            if (ec) {
                KNX_LOGE(TAG, "Failed to erase key '%s': %s", key.c_str(), ec.message().c_str());
                return PersistenceResult::WriteError;
            }
            return PersistenceResult::NotFound;
        }
        
        KNX_LOGD(TAG, "Erased key '%s'", key.c_str());
        return PersistenceResult::Success;
    }

    PersistenceResult eraseById(uint16_t keyId) override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }

        const auto key = makeCompactKey(keyId);
        std::string filepath = _directory + "/" + std::string(key.data()) + ".bin";
        std::error_code ec;
        if (!std::filesystem::remove(filepath, ec)) {
            if (ec) {
                KNX_LOGE(TAG, "Failed to erase id '%s': %s", key.data(), ec.message().c_str());
                return PersistenceResult::WriteError;
            }
            return PersistenceResult::NotFound;
        }

        KNX_LOGD(TAG, "Erased id '%s'", key.data());
        return PersistenceResult::Success;
    }
    
    PersistenceResult eraseAll() override {
        if (!_initialized) {
            return PersistenceResult::NotInitialized;
        }
        
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(_directory, ec)) {
            std::filesystem::remove(entry.path(), ec);
        }
        
        KNX_LOGI(TAG, "Erased all keys in namespace");
        return PersistenceResult::Success;
    }
    
    std::vector<std::string> listKeys() override {
        std::vector<std::string> keys;
        if (!_initialized) {
            return keys;
        }
        
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(_directory, ec)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().stem().string();
                keys.push_back(filename);
            }
        }
        
        return keys;
    }

private:
    std::string _directory;
    bool _initialized;
};

#endif

// Factory function
std::unique_ptr<ObjectPersistence> createPersistence() {
#ifdef ESP_PLATFORM
    return std::make_unique<NvsPersistence>();
#else
    return std::make_unique<FilePersistence>();
#endif
}

// PersistenceManager implementation
PersistenceManager::PersistenceManager(ObjectPersistence& persistence)
    : _persistence(persistence)
{
}

PersistenceResult PersistenceManager::saveObject(const SerializableObject& object) {
    std::vector<uint8_t> data;
    auto serializeRes = object.serialize(data);
    if (serializeRes.isError()) {
        KNX_LOGE(TAG, "Failed to serialize object '%s'", object.getStorageKey().c_str());
        return PersistenceResult::InvalidData;
    }
    
    return _persistence.save(object.getStorageKey(), data);
}

PersistenceResult PersistenceManager::loadObject(SerializableObject& object) {
    std::vector<uint8_t> data;
    PersistenceResult result = _persistence.load(object.getStorageKey(), data);
    if (result != PersistenceResult::Success) {
        return result;
    }
    
    auto deserializeRes = object.deserialize(data);
    if (deserializeRes.isError()) {
        KNX_LOGE(TAG, "Failed to deserialize object '%s'", object.getStorageKey().c_str());
        return PersistenceResult::InvalidData;
    }
    
    return PersistenceResult::Success;
}

util::Result<bool> PersistenceManager::objectExists(const SerializableObject& object) {
    return _persistence.exists(object.getStorageKey());
}

PersistenceResult PersistenceManager::eraseObject(const SerializableObject& object) {
    return _persistence.erase(object.getStorageKey());
}

} // namespace objects
} // namespace knx
