// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file object_persistence.hpp
 * @brief Interface object persistence layer
 * 
 * Provides load/save operations for interface objects using platform-specific
 * non-volatile storage (NVS on ESP32, file-based on Linux).
 */

#pragma once

#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace knx {
namespace objects {

/**
 * @brief Persistence result codes
 */
enum class PersistenceResult {
    Success,
    NotFound,
    InvalidData,
    StorageFull,
    ReadError,
    WriteError,
    NotInitialized
};

/**
 * @brief Object persistence interface
 * 
 * Handles serialization and storage of interface object state.
 * Platform-specific implementations handle actual storage mechanism.
 */
class ObjectPersistence {
public:
    virtual ~ObjectPersistence() = default;
    
    /**
     * @brief Initialize persistence layer
     * @param namespace_name Storage namespace (partition on ESP32, directory on Linux)
     * @return true on success
     */
    virtual util::Result<void> init(const std::string& namespace_name) = 0;
    
    /**
     * @brief Close persistence layer
     */
    virtual void close() = 0;
    
    /**
     * @brief Save binary data
     * @param key Unique identifier for the data
     * @param data Data to save
     * @return Result code
     */
    virtual PersistenceResult save(const std::string& key, std::span<const uint8_t> data) = 0;

    /**
     * @brief Save binary data by compact numeric identifier
     * @param keyId Compact key identifier
     * @param data Data to save
     * @return Result code
     */
    virtual PersistenceResult saveById(uint16_t keyId, std::span<const uint8_t> data) = 0;
    
    /**
     * @brief Load binary data
     * @param key Unique identifier for the data
     * @param data Output buffer for loaded data
     * @return Result code
     */
    virtual PersistenceResult load(const std::string& key, std::vector<uint8_t>& data) = 0;

    /**
     * @brief Load binary data by compact numeric identifier
     * @param keyId Compact key identifier
     * @param data Output buffer for loaded data
     * @return Result code
     */
    virtual PersistenceResult loadById(uint16_t keyId, std::vector<uint8_t>& data) = 0;
    
    /**
     * @brief Check if key exists
     * @param key Key to check
     * @return true if key exists in storage
     */
    virtual util::Result<bool> exists(const std::string& key) = 0;

    /**
     * @brief Check if compact key identifier exists
     * @param keyId Compact key identifier
     * @return true if key exists in storage
     */
    virtual util::Result<bool> existsById(uint16_t keyId) = 0;
    
    /**
     * @brief Delete stored data
     * @param key Key to delete
     * @return Result code
     */
    virtual PersistenceResult erase(const std::string& key) = 0;

    /**
     * @brief Delete stored data by compact numeric identifier
     * @param keyId Compact key identifier
     * @return Result code
     */
    virtual PersistenceResult eraseById(uint16_t keyId) = 0;
    
    /**
     * @brief Clear all data in namespace
     * @return Result code
     */
    virtual PersistenceResult eraseAll() = 0;
    
    /**
     * @brief Get list of all keys in namespace
     * @return Vector of key names
     */
    virtual std::vector<std::string> listKeys() = 0;
};

/**
 * @brief Create platform-specific persistence instance
 * @return Unique pointer to persistence implementation for current platform
 */
std::unique_ptr<ObjectPersistence> createPersistence();

/**
 * @brief Root directory backing the host (non-ESP) file persistence.
 *
 * Defaults to a per-process directory so that concurrently running test
 * binaries sharing a namespace cannot erase each other's state. Override with
 * the KNX_NVS_ROOT environment variable to pin a fixed location.
 *
 * On ESP-IDF, persistence uses NVS and this value is unused.
 *
 * @return Absolute path with no trailing separator.
 */
std::string persistenceRootDir();

/**
 * @brief Directory backing a single persistence namespace.
 *
 * Use this instead of hardcoding a path when a test needs to wipe storage.
 */
std::string persistenceNamespaceDir(const std::string& namespace_name);

/**
 * @brief Serializable interface object base
 * 
 * Interface objects that support persistence should implement this interface.
 */
class SerializableObject {
public:
    virtual ~SerializableObject() = default;
    
    /**
     * @brief Serialize object state to binary
     * @param data Output buffer
     * @return true on success
     */
    virtual util::Result<void> serialize(std::vector<uint8_t>& data) const = 0;
    
    /**
     * @brief Deserialize object state from binary
     * @param data Input buffer
     * @return true on success
     */
    virtual util::Result<void> deserialize(std::span<const uint8_t> data) = 0;
    
    /**
     * @brief Get unique key for this object
     * @return Storage key (e.g., "device_object", "address_table")
     */
    virtual std::string getStorageKey() const = 0;
};

/**
 * @brief Helper class for managing object persistence
 * 
 * Provides convenient save/load operations for serializable objects.
 */
class PersistenceManager {
public:
    explicit PersistenceManager(ObjectPersistence& persistence);
    ~PersistenceManager() = default;
    
    /**
     * @brief Save a serializable object
     * @param object Object to save
     * @return Result code
     */
    PersistenceResult saveObject(const SerializableObject& object);
    
    /**
     * @brief Load a serializable object
     * @param object Object to load into
     * @return Result code
     */
    PersistenceResult loadObject(SerializableObject& object);
    
    /**
     * @brief Check if object exists in storage
     * @param object Object to check
     * @return true if object data exists
     */
    util::Result<bool> objectExists(const SerializableObject& object);
    
    /**
     * @brief Delete object from storage
     * @param object Object to delete
     * @return Result code
     */
    PersistenceResult eraseObject(const SerializableObject& object);

private:
    ObjectPersistence& _persistence;
};

} // namespace objects
} // namespace knx
