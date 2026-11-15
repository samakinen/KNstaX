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

// Results are util::Result<void>. The codes a backend returns:
//   NotFound        the key is not stored
//   StorageFull     no room for the write
//   StorageError    the read, write or erase failed
//   NotInitialized  init() has not succeeded
// PersistenceManager adds EncodeFailed / DecodeFailed for the object itself.

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
     * @return ok, or the error code (see above)
     */
    virtual util::Result<void> save(const std::string& key, std::span<const uint8_t> data) = 0;

    /**
     * @brief Save binary data by compact numeric identifier
     * @param keyId Compact key identifier
     * @param data Data to save
     * @return ok, or the error code (see above)
     */
    virtual util::Result<void> saveById(uint16_t keyId, std::span<const uint8_t> data) = 0;
    
    /**
     * @brief Load binary data
     * @param key Unique identifier for the data
     * @param data Output buffer for loaded data
     * @return ok, or the error code (see above)
     */
    virtual util::Result<void> load(const std::string& key, std::vector<uint8_t>& data) = 0;

    /**
     * @brief Load binary data by compact numeric identifier
     * @param keyId Compact key identifier
     * @param data Output buffer for loaded data
     * @return ok, or the error code (see above)
     */
    virtual util::Result<void> loadById(uint16_t keyId, std::vector<uint8_t>& data) = 0;
    
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
     * @return ok, or the error code (see above)
     */
    virtual util::Result<void> erase(const std::string& key) = 0;

    /**
     * @brief Delete stored data by compact numeric identifier
     * @param keyId Compact key identifier
     * @return ok, or the error code (see above)
     */
    virtual util::Result<void> eraseById(uint16_t keyId) = 0;
    
    /**
     * @brief Clear all data in namespace
     * @return ok, or the error code (see above)
     */
    virtual util::Result<void> eraseAll() = 0;
    
    /**
     * @brief Get list of all keys in namespace
     * @return Vector of key names
     */
    virtual std::vector<std::string> listKeys() = 0;
};

/// Creates a persistence backend; see setPersistenceFactory().
using PersistenceFactory = std::unique_ptr<ObjectPersistence> (*)();

/**
 * @brief Make createPersistence() return the product's own storage backend
 *
 * Registered before the stack starts. It replaces the platform default (NVS
 * on ESP-IDF, files on hosts) and is the only source of storage on a
 * bare-metal build, which has no default. Pass nullptr to restore the default.
 *
 * @thread_safety NOT thread-safe: call during initialisation, before
 *                `BusAccessUnit::init()` or `startCommissionedProduct()`.
 */
void setPersistenceFactory(PersistenceFactory factory);

/**
 * @brief Create the persistence instance for this build
 * @return The registered factory's backend, else the platform default.
 *         `nullptr` on a bare-metal build with no factory registered.
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
     * @return ok, or the error code (see above)
     */
    util::Result<void> saveObject(const SerializableObject& object);
    
    /**
     * @brief Load a serializable object
     * @param object Object to load into
     * @return ok, or the error code (see above)
     */
    util::Result<void> loadObject(SerializableObject& object);
    
    /**
     * @brief Check if object exists in storage
     * @param object Object to check
     * @return true if object data exists
     */
    util::Result<bool> objectExists(const SerializableObject& object);
    
    /**
     * @brief Delete object from storage
     * @param object Object to delete
     * @return ok, or the error code (see above)
     */
    util::Result<void> eraseObject(const SerializableObject& object);

private:
    ObjectPersistence& _persistence;
};

} // namespace objects
} // namespace knx
