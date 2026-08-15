// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file group_object_table_object.hpp
 * @brief KNX Group Object Table Object (Interface Object Type 9)
 * 
 * Manages all group objects in the device with full KNX compliance.
 */

#pragma once

#include "knx/objects/interface_object.hpp"
#include "knx/types.hpp"
#include "knx/application/group_object.hpp"
#include "knx/util/result.hpp"
#include "knx/util/fixed_vector.hpp"
#include <vector>
#include <span>
#include <memory>
#include <cstdint>

namespace knx {
namespace objects {

/**
 * @brief Group Object Table properties (KNX spec)
 */
enum class GroupObjectTableProperty : uint8_t {
    ObjectType = 1,           // Interface Object Type (9)
    ObjectName = 2,           // Name of the object
    LoadStateControl = 5,     // Load state control
    TableReference = 7,       // Reference to table data
    TableData = 9,            // Serialized table data
    ErrorCode = 11,           // Last error code
    /// PID_GO_DIAGNOSTICS (03/05/01 §4.8.1): PDT_FUNCTION, the service a
    /// management client uses to read or write a group value through this
    /// device.  Handled by BusAccessUnit, not by this object's property store.
    GoDiagnostics = 66,
};

/**
 * @brief Domain model for the group object table (0-based indices)
 *
 * KNX wire semantics are intentionally excluded.
 */
class GroupObjectTableDomain {
public:
    using Index = DomainIndex; // 0-based domain index

    static constexpr uint16_t kMaxObjects = 256;
    static constexpr uint16_t kMaxSerializedBytes = 0xFFFF;

    util::Result<Index> add(std::unique_ptr<application::GroupObject> obj);
    util::Result<void> remove(Index index);

    application::GroupObject* get(Index index);
    const application::GroupObject* get(Index index) const;

    uint16_t size() const;
    void clear();
    void reserve(uint16_t capacity);

    util::Result<void> load(std::vector<std::unique_ptr<application::GroupObject>> objects);

    application::GroupObject* findByAddress(const GroupAddress& address);
    const application::GroupObject* findByAddress(const GroupAddress& address) const;
    util::Result<size_t> findAllByAddress(const GroupAddress& address, std::span<GroupObjectIndex> out) const;
    bool hasAddress(const GroupAddress& address) const;

    util::Result<size_t> getAllAddresses(std::span<GroupAddress> out) const;
    bool isValid() const;

    struct Statistics {
        uint16_t totalObjects;
        uint16_t validObjects;
        uint16_t activeObjects;
        uint16_t uniqueAddresses;
    };
    Statistics getStatistics() const;

    const util::FixedVector<std::unique_ptr<application::GroupObject>, kMaxObjects>& objects() const { return _objects; }

    /// Nth populated object in table order, or nullptr when `slot` is past the
    /// end.  Table order is what PID_TABLE descriptor indices refer to.
    application::GroupObject* objectAt(size_t slot) {
        size_t seen = 0;
        for (auto& obj : _objects) {
            if (!obj) {
                continue;
            }
            if (seen == slot) {
                return obj.get();
            }
            ++seen;
        }
        return nullptr;
    }

    // KNX 03.05.02 load state (PID_LOAD_STATE_CONTROL), driven by the ETS
    // download procedure via LoadControlProperty. The group objects themselves
    // are firmware-defined (static ComObjectTable in the knxprod), so the load
    // procedure only tracks state and must never discard them.
    uint8_t loadState() const { return _loadState; }
    void setLoadState(uint8_t state) { _loadState = state; }
    void loadControlReset() {}

private:
    uint16_t countUniqueAddresses() const;
    util::FixedVector<std::unique_ptr<application::GroupObject>, kMaxObjects> _objects;
    uint8_t _loadState{1};  // loadstate::kLoaded — firmware boots with usable objects
};

/**
 * @brief Group Object Table Object (Interface Object Type 9)
 * 
 * Central runtime container for all communication objects (group objects).
 * Provides KNX-compliant object management with properties and state.
 */
class GroupObjectTableObject : public InterfaceObject {
public:
    GroupObjectTableObject();
    ~GroupObjectTableObject() override = default;

    // === InterfaceObject interface ===
    InterfaceObjectType objectType() const override { return InterfaceObjectType::groupObjectTable(); }
    KernelBinding kernelBinding() const override;

    // Disable copy, enable move
    GroupObjectTableObject(const GroupObjectTableObject&) = delete;
    GroupObjectTableObject& operator=(const GroupObjectTableObject&) = delete;
    GroupObjectTableObject(GroupObjectTableObject&&) = default;
    GroupObjectTableObject& operator=(GroupObjectTableObject&&) = default;

    // === Object Management ===
    
    /**
     * @brief Add a group object to the table
     * @param obj Group object (ownership transferred)
        * @return Object index (0-based) or `GroupObjectIndex::invalid()` if full/invalid
     */
    GroupObjectIndex addGroupObject(std::unique_ptr<application::GroupObject> obj);
    
    /**
     * @brief Remove object at index
     * @param index Object index (0-based)
    * @return Result<void> indicating success or error
     */
    util::Result<void> removeObject(GroupObjectIndex index);
    
    /**
     * @brief Get object by index
     * @param index Object index (0-based)
     * @return Pointer to object or nullptr
     */
    application::GroupObject* getGroupObject(GroupObjectIndex index);
    const application::GroupObject* getGroupObject(GroupObjectIndex index) const;
    
    /**
     * @brief Get object count
     */
    uint16_t objectCount() const { return _table.size(); }
    
    /**
     * @brief Maximum objects supported
     */
    uint16_t maxObjects() const { return MAX_OBJECTS; }
    
    /**
     * @brief Clear all objects
     */
    void clear();

    // === Address-Based Access ===
    
    /**
     * @brief Find object by group address
     * @param address Group address to find
     * @return Pointer to object or nullptr
     */
    application::GroupObject* findByAddress(const GroupAddress& address);
    const application::GroupObject* findByAddress(const GroupAddress& address) const;
    
    /**
     * @brief Find all objects with given address
     * @param address Group address
     * @return Vector of indices
     */
    util::Result<size_t> findAllByAddress(const GroupAddress& address, std::span<GroupObjectIndex> out) const;
    
    /**
     * @brief Check if address is registered
     */
    bool hasAddress(const GroupAddress& address) const;

    // === Bulk Operations ===
    
    /**
     * @brief Load entire table from vector
     * @param objects Vector of group objects (ownership transferred)
    * @return Result<void> indicating success or error
     */
    util::Result<void> loadTable(std::vector<std::unique_ptr<application::GroupObject>> objects);
    
    /**
     * @brief Reserve capacity for objects
     */
    void reserve(uint16_t capacity);

    // === Runtime State ===
    
    /**
     * @brief Get all addresses in use
     */
    util::Result<size_t> getAllAddresses(std::span<GroupAddress> out) const;
    
    /**
     * @brief Validate all objects
     */
    bool isValid() const;
    
    /**
     * @brief Get statistics
     */
    struct Statistics {
        uint16_t totalObjects;
        uint16_t validObjects;
        uint16_t activeObjects;     // Objects with valid data
        uint16_t uniqueAddresses;
    };
    Statistics getStatistics() const;

private:
    GroupObjectTableDomain _table;
    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
    static constexpr uint16_t MAX_OBJECTS = GroupObjectTableDomain::kMaxObjects;
};

} // namespace objects
} // namespace knx
