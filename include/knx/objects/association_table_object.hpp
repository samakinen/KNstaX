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

#include "knx/config.hpp"
#include "knx/objects/interface_object.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

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
 * The entries are held once, in the form ETS loads them: the association
 * table's load segment (a big-endian entry count, then per entry the TSAP,
 * which is the address table index, and the ASAP, the group object number,
 * both big-endian u16). A_Memory_Write lands in that storage, A_Memory_Read
 * reads it, persistence saves the entries straight from it, and lookups
 * decode it in place. There is no parsed copy and no index beside it.
 *
 * What is in force is the first size() entries. The load procedure keeps
 * that apart from what ETS is writing: Unload and StartLoading empty the
 * table (size 0) while the octets ETS streams after them land in the same
 * storage, and LoadCompleted (applyLoadSegment()) takes the count ETS wrote.
 * So a verification read before LoadCompleted returns what was written, and
 * the table takes effect at LoadCompleted, as when the segment was a copy.
 */
class AssociationTableDomain {
public:
    using Index = DomainIndex; // 0-based domain index

    /// KNX_ASSOCIATION_TABLE_ENTRIES.
    static constexpr uint16_t kMaxEntries = static_cast<uint16_t>(config::ASSOCIATION_TABLE_ENTRIES);
    /// One entry in the load segment: TSAP and ASAP, big-endian.
    static constexpr size_t kEntryBytes = 4;
    /// The load segment: the entry count, then the entries.
    static constexpr size_t kImageBytes = 2u + kEntryBytes * kMaxEntries;

    util::Result<void> add(const AssociationEntry& entry);
    util::Result<void> remove(Index index);
    util::Result<void> set(Index index, const AssociationEntry& entry);
    util::Result<void> setExpand(Index index, const AssociationEntry& entry);

    AssociationEntry get(Index index) const;

    uint16_t removeEntriesForAddress(AddressTableIndex addressIndex);
    uint16_t removeEntriesForGroupObject(GroupObjectIndex groupObjectNumber);

    util::Result<size_t> findGroupObjects(AddressTableIndex addressIndex, std::span<GroupObjectIndex> out) const;
    util::Result<size_t> findAddressIndices(GroupObjectIndex groupObjectNumber, std::span<AddressTableIndex> out) const;
    bool hasAssociation(AddressTableIndex addressIndex, GroupObjectIndex groupObjectNumber) const;

    /**
     * @brief Call @p visit for every group object associated with @p addressIndex
     *
     * No buffer and no limit on how many objects share an address. Objects
     * are visited in table order. A linear scan of the entries in place,
     * O(n): a device's table is a few hundred entries at most, and a scan of
     * the 80 of a 60-object device is a few microseconds on a Cortex-M3, once
     * per group telegram. An index beside the table cost 2 octets per entry
     * of capacity and had to follow every change, including the octets ETS
     * writes.
     */
    template <typename Visitor>
    void forEachGroupObject(AddressTableIndex addressIndex, Visitor&& visit) const {
        for (uint16_t position = 0; position < _size; ++position) {
            if (tsapAt(position) == addressIndex.raw) {
                visit(GroupObjectIndex(asapAt(position)));
            }
        }
    }

    /// True if at least one group object is associated with @p addressIndex. O(n), as forEachGroupObject().
    bool hasGroupObjects(AddressTableIndex addressIndex) const;

    util::Result<void> load(std::span<const AssociationEntry> entries);
    void truncate(uint16_t size);
    void clear();
    void reserve(uint16_t capacity);

    uint16_t size() const;
    bool isValid() const;

    /// The load segment ETS reads and writes through A_Memory (PID_TABLE_REFERENCE).
    std::span<uint8_t> loadSegment() { return _image; }
    std::span<const uint8_t> loadSegment() const { return _image; }

    /**
     * @brief Put the entries ETS wrote into force (the LoadCompleted event)
     *
     * Takes the count from the segment. The table a load leaves is the one
     * add() would build from the same entries: invalid entries and repeats
     * are dropped, the order is kept (in place, so the segment then reads
     * back without them). A count over the capacity is refused, the table is
     * left empty: a capacity is a promise, not a cut.
     *
     * @return the number of entries now in force, or InvalidParameter
     */
    util::Result<uint16_t> applyLoadSegment();

    /// The persisted form of PID_TABLE (the entries in force, 4 octets each),
    /// which is the segment's own storage: a save needs no buffer for it.
    std::span<const uint8_t> persistenceRecord() const {
        return std::span<const uint8_t>(_image.data() + 2u, kEntryBytes * _size);
    }

    // KNX 03.05.02 load state (PID_LOAD_STATE_CONTROL), driven by the ETS
    // download procedure via LoadControlProperty.
    uint8_t loadState() const { return _loadState; }
    void setLoadState(uint8_t state) { _loadState = state; }
    void loadControlReset() { clear(); }

private:
    uint16_t u16At(size_t offset) const {
        return static_cast<uint16_t>((static_cast<uint16_t>(_image[offset]) << 8) | _image[offset + 1u]);
    }
    uint16_t tsapAt(uint16_t position) const { return u16At(2u + kEntryBytes * position); }
    uint16_t asapAt(uint16_t position) const { return u16At(4u + kEntryBytes * position); }
    AssociationEntry at(uint16_t position) const {
        return AssociationEntry(AddressTableIndex(tsapAt(position)), GroupObjectIndex(asapAt(position)));
    }
    void put(uint16_t position, const AssociationEntry& entry);
    /// Entries in force, and the segment's count with them.
    void setSize(uint16_t size);
    /// While ETS loads the table, the storage past the entries in force holds
    /// what it is writing; an append by the firmware would land on it.
    bool loading() const;

    std::array<uint8_t, kImageBytes> _image{};
    uint16_t _size{0};
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

    /// PID_LOAD_STATE_CONTROL state (03/05/01 §4.23: 3 = error).
    uint8_t loadState() const { return _table.loadState(); }
    /// Record a failed load: ETS reads the error state and reports the download as failed.
    void setLoadState(uint8_t state) { _table.setLoadState(state); }

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

    /// Visit every group object associated with @p addressIndex, with no
    /// limit on how many there are. See AssociationTableDomain::forEachGroupObject.
    template <typename Visitor>
    void forEachGroupObject(AddressTableIndex addressIndex, Visitor&& visit) const {
        _table.forEachGroupObject(addressIndex, std::forward<Visitor>(visit));
    }

    /// True if at least one group object is associated with @p addressIndex.
    bool hasGroupObjects(AddressTableIndex addressIndex) const { return _table.hasGroupObjects(addressIndex); }

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
     * @return The entry, or nullopt if index invalid. A copy: the entries are
     *         held in their load segment form, not as AssociationEntry.
     */
    std::optional<AssociationEntry> getEntry(uint16_t index) const;

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

    // === ETS download and persistence ===

    /// The table's storage, as the memory-mapped load segment (see AssociationTableDomain).
    std::span<uint8_t> loadSegment() { return _table.loadSegment(); }
    std::span<const uint8_t> loadSegment() const { return _table.loadSegment(); }
    /// LoadCompleted: put the entries ETS wrote into force. See AssociationTableDomain::applyLoadSegment().
    util::Result<uint16_t> applyLoadSegment() { return _table.applyLoadSegment(); }
    /// The persisted PID_TABLE record, read in place.
    std::span<const uint8_t> persistenceRecord() const { return _table.persistenceRecord(); }

private:
    AssociationTableDomain _table;
    ValidationPolicy _validationPolicy{ValidationPolicy::OnWrite};
};

} // namespace objects
} // namespace knx
