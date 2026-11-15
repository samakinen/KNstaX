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

#include "knx/config.hpp"
#include "knx/objects/interface_object.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/shared_group_address_list.hpp"
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

    /// KNX_ADDRESS_TABLE_ENTRIES.
    static constexpr uint16_t kMaxEntries = static_cast<uint16_t>(config::ADDRESS_TABLE_ENTRIES);
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

    /// True when the entries are in strictly ascending order, so findIndex()
    /// can binary-search. Exposed for tests.
    bool isSorted() const { return _sorted; }

    /// The entries for a reader in another context (an interrupt deciding the
    /// L2 acknowledge); every mutator brackets its change for it.
    util::SharedGroupAddressView sharedView() const { return _shared.view(_entries.data(), kMaxEntries); }

private:
    /// Brackets one change of the entries: readers discard while it lasts, and
    /// its end recomputes _sorted and publishes the count. Every mutator holds
    /// one, so lookups never recompute and no change goes unannounced.
    class Change {
    public:
        explicit Change(AddressTableDomain& domain)
            : _domain(domain)
        {
            _domain._shared.beginChange();
        }
        ~Change()
        {
            _domain.refreshSorted();
            _domain._shared.endChange(_domain._entries.size());
        }
        Change(const Change&) = delete;
        Change& operator=(const Change&) = delete;

    private:
        AddressTableDomain& _domain;
    };

    /// Recompute _sorted.
    void refreshSorted();

    util::FixedVector<GroupAddress, kMaxEntries> _entries;
    bool _sorted{true};
    util::SharedGroupAddressState _shared;
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

    /// PID_LOAD_STATE_CONTROL state (03/05/01 §4.23: 3 = error).
    uint8_t loadState() const { return _table.loadState(); }
    /// Record a failed load: ETS reads the error state and reports the download as failed.
    void setLoadState(uint8_t state) { _table.setLoadState(state); }

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
     * @brief The entries, in table order (entry i is index i + 1), where they are stored
     *
     * The one copy of the device's group address list: the data link layers
     * filter and acknowledge on this storage (BusAccessStackPort::attachAddressTable)
     * instead of holding lists of their own. The span stays valid for the
     * object's lifetime; its length is the count now.
     */
    std::span<const GroupAddress> addresses() const {
        return std::span<const GroupAddress>(_table.entries().data(), _table.entries().size());
    }

    /// True when addresses() is in strictly ascending order (a binary search applies).
    bool isSorted() const { return _table.isSorted(); }

    /// The same storage for a reader in an interrupt (the TP-UART backend's
    /// acknowledge decision), safe against changes made while it reads.
    util::SharedGroupAddressView sharedGroupAddressView() const { return _table.sharedView(); }

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
