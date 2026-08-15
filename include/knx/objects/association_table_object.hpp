// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file association_table_object.hpp
 * @brief KNX Association Table Object (Interface Object Type 2)
 * 
 * Maps group addresses to communication objects (group objects).
 * Per KNX spec 3/7/2, provides association management for group communication.
 */

#pragma once

#include "knx/objects/interface_object.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include "knx/util/fixed_vector.hpp"
#include <cstdint>
#include <span>

namespace knx {
namespace objects {

/**
 * @brief Association Table Object properties
 */
enum class AssociationTableProperty : uint8_t {
    ObjectType = 1,          // PID_OBJECT_TYPE
    LoadState = 5,           // PID_LOAD_STATE_CONTROL
    TableReference = 7,      // PID_TABLE_REFERENCE
    TableData = 23           // PID_TABLE — standard table property, used by ETS SystemB load procedure
};

/**
 * @brief Association entry linking address table indices to group objects
 * 
 * Per KNX spec, association table maps:
 * - Address table index (group address reference)
 * - Group object number (communication object)
 */
struct AssociationEntry {
    AddressTableIndex addressIndex;      ///< Index into address table (1-based)
    GroupObjectIndex groupObjectNumber; ///< Group object index

    AssociationEntry() : addressIndex(AddressTableIndex::invalid()), groupObjectNumber(GroupObjectIndex::invalid()) {}
    AssociationEntry(AddressTableIndex addrIdx, GroupObjectIndex goNum)
        : addressIndex(addrIdx), groupObjectNumber(goNum) {}

    bool isValid() const { return addressIndex.isValid() && groupObjectNumber.isValid(); }

    bool operator==(const AssociationEntry& other) const {
        return addressIndex == other.addressIndex && 
               groupObjectNumber == other.groupObjectNumber;
    }
};

/**
 * @brief Domain model for the association table (0-based indices)
 *
 * KNX wire semantics are intentionally excluded.
 */
class AssociationTableDomain {
public:
    using Index = DomainIndex; // 0-based domain index

    static constexpr uint16_t kMaxEntries = 512;

    util::Result<void> add(const AssociationEntry& entry);
    util::Result<void> remove(Index index);
    util::Result<void> set(Index index, const AssociationEntry& entry);
    util::Result<void> setExpand(Index index, const AssociationEntry& entry);

    AssociationEntry get(Index index) const;
    const AssociationEntry* getPtr(Index index) const;

    uint16_t removeEntriesForAddress(AddressTableIndex addressIndex);
    uint16_t removeEntriesForGroupObject(GroupObjectIndex groupObjectNumber);

    util::Result<size_t> findGroupObjects(AddressTableIndex addressIndex, std::span<GroupObjectIndex> out) const;
    util::Result<size_t> findAddressIndices(GroupObjectIndex groupObjectNumber, std::span<AddressTableIndex> out) const;
    bool hasAssociation(AddressTableIndex addressIndex, GroupObjectIndex groupObjectNumber) const;

    util::Result<void> load(std::span<const AssociationEntry> entries);
    void truncate(uint16_t size);
    void clear();
    void reserve(uint16_t capacity);

    uint16_t size() const;
    bool isValid() const;
    const util::FixedVector<AssociationEntry, kMaxEntries>& entries() const { return _entries; }

    // KNX 03.05.02 load state (PID_LOAD_STATE_CONTROL), driven by the ETS
    // download procedure via LoadControlProperty.
    uint8_t loadState() const { return _loadState; }
    void setLoadState(uint8_t state) { _loadState = state; }
    void loadControlReset() { clear(); }

private:
    util::FixedVector<AssociationEntry, kMaxEntries> _entries;
    uint8_t _loadState{1};  // loadstate::kLoaded — firmware boots with usable tables
};

/**
 * @brief Association Table Object - Interface Object Type 2
 * 
 * Maintains mapping between group addresses (via address table indices)
 * and communication objects (group objects).
 * 
 * This is the core of KNX group communication:
 * Group Address → Address Table Index → Association → Group Object
 */
class AssociationTableObject : public InterfaceObject {
public:
    AssociationTableObject();
    ~AssociationTableObject() override = default;

    // === InterfaceObject interface ===
    InterfaceObjectType objectType() const override { return InterfaceObjectType::associationTable(); }
    KernelBinding kernelBinding() const override;

    // Disable copy, enable move
    AssociationTableObject(const AssociationTableObject&) = delete;
    AssociationTableObject& operator=(const AssociationTableObject&) = delete;
    AssociationTableObject(AssociationTableObject&&) = default;
    AssociationTableObject& operator=(AssociationTableObject&&) = default;

    // === Table Management ===

    /**
     * @brief Add an association entry
     * @param entry Association entry to add
    * @return Result<void> indicating success or error
     */
    util::Result<void> addEntry(const AssociationEntry& entry);

    /**
     * @brief Remove entry at specific index
     * @param index Entry index (0-based)
    * @return Result<void> indicating success or error
     */
    util::Result<void> removeEntry(uint16_t index);

    /**
     * @brief Remove all entries for a specific address table index
     * @param addressIndex Address table index
     * @return Number of entries removed
     */
    uint16_t removeEntriesForAddress(AddressTableIndex addressIndex);

    /**
     * @brief Remove all entries for a specific group object
    * @param groupObjectNumber Group object index
     * @return Number of entries removed
     */
    uint16_t removeEntriesForGroupObject(GroupObjectIndex groupObjectNumber);

    /**
     * @brief Clear all entries
     */
    void clearEntries();

    /**
     * @brief Set entry at specific index
     * @param index Entry index (0-based)
     * @param entry Association entry
    * @return Result<void> indicating success or error
     */
    util::Result<void> setEntry(uint16_t index, const AssociationEntry& entry);

    /**
     * @brief Set entry at 1-based index, expanding the table if needed (no validity check)
     *
     * Allows programming via property writes that target a subset of entries.
     *
     * @param index Entry index (1-based)
     * @param entry Association entry
    * @return Result<void> indicating success or error
     */
    util::Result<void> setEntryExpandUnchecked(uint16_t index, const AssociationEntry& entry);

    // === Access ===

    /**
     * @brief Find group objects associated with an address table index
     * @param addressIndex Address table index (1-based)
     * @return Vector of group object numbers
     */
    util::Result<size_t> findGroupObjects(AddressTableIndex addressIndex, std::span<GroupObjectIndex> out) const;

    /**
     * @brief Find address table indices for a group object
    * @param groupObjectNumber Group object index
     * @return Vector of address table indices
     */
    util::Result<size_t> findAddressIndices(GroupObjectIndex groupObjectNumber, std::span<AddressTableIndex> out) const;

    /**
     * @brief Check if association exists
     * @param addressIndex Address table index
    * @param groupObjectNumber Group object index
     * @return true if association exists
     */
    bool hasAssociation(AddressTableIndex addressIndex, GroupObjectIndex groupObjectNumber) const;

    /**
     * @brief Get entry at specific index
     * @param index Entry index (0-based)
     * @return Pointer to entry, or nullptr if index invalid
     */
    const AssociationEntry* getEntry(uint16_t index) const;

    /**
     * @brief Get number of entries
     */
    uint16_t entryCount() const { return _table.size(); }

    /**
     * @brief Get maximum table capacity
     */
    uint16_t maxEntries() const { return AssociationTableDomain::kMaxEntries; }

    /**
     * @brief Check if table is full
     */
    bool isFull() const { return _table.size() >= AssociationTableDomain::kMaxEntries; }

    /**
     * @brief Check if table is empty
     */
    bool isEmpty() const { return _table.size() == 0; }

    // === Bulk Operations ===

    /**
     * @brief Load entire association table (from ETS programming)
    * @param entries Association entries to load
    * @return Result<void> indicating success or error
     */
    util::Result<void> loadTable(std::span<const AssociationEntry> entries);

    /**
     * @brief Get all entries
     */
    util::Result<size_t> getAllEntries(std::span<AssociationEntry> out) const;

    /**
     * @brief Reserve capacity for entries (optimization)
     */
    void reserve(uint16_t capacity);

    // === Validation ===

    bool isValid() const;

private:
    AssociationTableDomain _table;
    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
};

} // namespace objects
} // namespace knx
