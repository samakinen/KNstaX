// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file address_table_object.hpp
 * @brief KNX Address Table Object (Interface Object Type 1)
 * 
 * Maps group addresses to internal table indices.
 * Per KNX spec 3/7/2, provides group address management.
 */

#pragma once

#include "knx/objects/interface_object.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include "knx/util/fixed_vector.hpp"
#include <span>
#include <cstdint>

namespace knx {
namespace objects {

/**
 * @brief Address Table Object properties
 */
enum class AddressTableProperty : uint8_t {
    ObjectType = 1,          // PID_OBJECT_TYPE
    LoadState = 5,           // PID_LOAD_STATE_CONTROL
    TableReference = 7,      // PID_TABLE_REFERENCE
    TableData = 23           // PID_TABLE — standard table property, used by ETS SystemB load procedure
};

/**
 * @brief Domain model for the address table (0-based indices)
 *
 * KNX wire semantics are intentionally excluded.
 */
class AddressTableDomain {
public:
    using Index = DomainIndex; // 0-based domain index

    static constexpr uint16_t kMaxEntries = 256;
    static constexpr uint16_t kInvalidIndexValue = 0xFFFF;

    static constexpr Index invalidIndex() { return Index{kInvalidIndexValue}; }
    static constexpr bool isValidIndex(Index index) { return index.value != kInvalidIndexValue; }

    util::Result<Index> add(const GroupAddress& address);
    util::Result<void> remove(Index index);
    util::Result<void> set(Index index, const GroupAddress& address);
    util::Result<void> setExpand(Index index, const GroupAddress& address);

    GroupAddress get(Index index) const;
    Index findIndex(const GroupAddress& address) const;

    util::Result<void> load(std::span<const GroupAddress> addresses);
    void truncate(uint16_t size);
    void clear();
    void reserve(uint16_t capacity);

    uint16_t size() const;
    bool isValid() const;
    const util::FixedVector<GroupAddress, kMaxEntries>& entries() const { return _entries; }

    // KNX 03.05.02 load state (PID_LOAD_STATE_CONTROL), driven by the ETS
    // download procedure via LoadControlProperty.
    uint8_t loadState() const { return _loadState; }
    void setLoadState(uint8_t state) { _loadState = state; }
    void loadControlReset() { clear(); }

private:
    util::FixedVector<GroupAddress, kMaxEntries> _entries;
    uint8_t _loadState{1};  // loadstate::kLoaded — firmware boots with usable tables
};

/**
 * @brief Address Table Object - Interface Object Type 1
 * 
 * Contains the group address table for the device.
 * Maps group addresses to internal indices used by association table.
 */
class AddressTableObject : public InterfaceObject {
public:
    AddressTableObject();
    ~AddressTableObject() override = default;

    // === InterfaceObject interface ===
    InterfaceObjectType objectType() const override { return InterfaceObjectType::addressTable(); }
    KernelBinding kernelBinding() const override;

    // Disable copy, enable move
    AddressTableObject(const AddressTableObject&) = delete;
    AddressTableObject& operator=(const AddressTableObject&) = delete;
    AddressTableObject(AddressTableObject&&) = default;
    AddressTableObject& operator=(AddressTableObject&&) = default;

    // === Table Management ===

    /**
     * @brief Add a group address to the table
     * @param address Group address to add
        * @return Index of the added address, or AddressTableIndex::invalid() if table is full
     */
    AddressTableIndex addEntry(const GroupAddress& address);

    /**
     * @brief Remove an entry at the specified index
     * @param index Table index
    * @return Result<void> indicating success or error
     */
    util::Result<void> removeEntry(AddressTableIndex index);

    /**
     * @brief Clear all entries from the table
     */
    void clearEntries();

    /**
     * @brief Set entry at specific index
     * @param index Table index
     * @param address Group address
    * @return Result<void> indicating success or error
     */
    util::Result<void> setEntry(AddressTableIndex index, const GroupAddress& address);

    /**
     * @brief Set entry at 1-based index, expanding the table if needed
     *
     * Allows programming via property writes that target a subset of entries.
     *
     * @param index Table index (1-based)
     * @param address Group address
    * @return Result<void> indicating success or error
     */
    util::Result<void> setEntryExpand(AddressTableIndex index, const GroupAddress& address);

    // === Access ===

    /**
     * @brief Get group address at the specified index
     * @param index Table index (1-based for KNX compatibility)
     * @return Group address, or invalid address if index out of bounds
     */
    GroupAddress getAddress(AddressTableIndex index) const;

    /**
     * @brief Find index for a group address
     * @param address Group address to find
     * @return Table index (1-based), or 0 if not found
     */
    AddressTableIndex findIndex(const GroupAddress& address) const;

    /**
     * @brief Get number of entries in the table
     */
    uint16_t entryCount() const { return _table.size(); }

    /**
     * @brief Get maximum table capacity
     */
    uint16_t maxEntries() const { return AddressTableDomain::kMaxEntries; }

    /**
     * @brief Check if table is full
     */
    bool isFull() const { return _table.size() >= AddressTableDomain::kMaxEntries; }

    /**
     * @brief Check if table is empty
     */
    bool isEmpty() const { return _table.size() == 0; }

    // === Bulk Operations ===

    /**
     * @brief Load entire address table (from ETS programming)
    * @param addresses Group addresses to load
    * @return Result<void> indicating success or error
     */
    util::Result<void> loadTable(std::span<const GroupAddress> addresses);

    /**
     * @brief Get all addresses in the table
     */
    util::Result<size_t> getAllAddresses(std::span<GroupAddress> out) const;

    /**
     * @brief Reserve capacity for entries (optimization)
     */
    void reserve(uint16_t capacity);

    // === Validation ===

    bool isValid() const;

private:
    AddressTableDomain _table;
    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
};

} // namespace objects
} // namespace knx
