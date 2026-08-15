// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file filter_table.hpp
 * @brief KNX filter table for group address filtering
 * 
 * Manages which group addresses are allowed/blocked at network layer.
 */

#pragma once

#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace knx {
namespace network {

/**
 * @brief Filter action
 */
enum class FilterAction {
    Allow,  ///< Allow frame to pass
    Block   ///< Block frame
};

/**
 * @brief Filter table entry
 * 
 * Defines filtering rules for group addresses.
 */
struct FilterEntry {
    GroupAddress address;   ///< Group address or range
    GroupAddress mask;      ///< Address mask (0=wildcard)
    FilterAction action;    ///< Allow or block
    EntryState enabled;     ///< Entry is active
    
    FilterEntry()
        : address(0)
        , mask(0xFFFF)
        , action(FilterAction::Allow)
        , enabled(EntryState::Enabled)
    {
    }
    
    /**
     * @brief Check if address matches this filter
     * @param addr Address to check
     * @return true if address matches mask
     */
    bool matches(const GroupAddress& addr) const {
        return (addr.raw & mask.raw) == (address.raw & mask.raw);
    }
};

/**
 * @brief Filter table for network layer
 * 
 * Manages group address filtering at network boundaries.
 * Used by line couplers to control which group telegrams cross lines.
 */
class FilterTable {
public:
    /// Maximum filter table entries
    static constexpr size_t MAX_ENTRIES = 64;
    
    FilterTable();
    ~FilterTable() = default;
    
    /**
     * @brief Set default filter action
     * @param action Default action when no rule matches
     */
    void setDefaultAction(FilterAction action) { _defaultAction = action; }
    
    /**
     * @brief Get default filter action
     * @return Default action
     */
    FilterAction defaultAction() const { return _defaultAction; }
    
    /**
     * @brief Add filter entry
     * @param entry Filter entry to add
     * @return Index of entry, or -1 if table full
     */
    int addEntry(const FilterEntry& entry);
    
    /**
     * @brief Add filter entry (convenience method)
     * @param address Group address
     * @param addressMask Address mask
     * @param action Filter action
     * @param enabled Entry enabled
     * @return Result<void> indicating success or error
     */
    util::Result<void> addEntry(const GroupAddress& address, const GroupAddress& addressMask, FilterAction action, EntryState enabled) {
        FilterEntry entry;
        entry.address = address;
        entry.mask = addressMask;
        entry.action = action;
        entry.enabled = enabled;
        return addEntry(entry) >= 0
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::QueueFull);
    }
    
    /**
     * @brief Remove filter entry
     * @param index Entry index to remove
    * @return Result<void> indicating success or error
     */
    util::Result<void> removeEntry(size_t index);
    
    /**
     * @brief Get filter entry
     * @param index Entry index
     * @return Pointer to entry, or nullptr if invalid index
     */
    const FilterEntry* getEntry(size_t index) const;
    
    /**
     * @brief Update filter entry
     * @param index Entry index
     * @param entry New entry data
    * @return Result<void> indicating success or error
     */
    util::Result<void> updateEntry(size_t index, const FilterEntry& entry);
    
    /**
     * @brief Update filter entry (convenience method)
     * @param index Entry index
     * @param address Group address
     * @param addressMask Address mask
     * @param action Filter action
     * @param enabled Entry enabled
     * @return true if updated
     */
    util::Result<void> updateEntry(size_t index, const GroupAddress& address, const GroupAddress& addressMask, FilterAction action, EntryState enabled) {
        FilterEntry entry;
        entry.address = address;
        entry.mask = addressMask;
        entry.action = action;
        entry.enabled = enabled;
        return updateEntry(index, entry);
    }
    
    /**
     * @brief Get filter entry (optional-based, for tests)
     * @param index Entry index
     * @return Optional containing entry if found
     */
    std::optional<FilterEntry> getEntryOpt(size_t index) const {
        const FilterEntry* entry = getEntry(index);
        if (entry) {
            return *entry;
        }
        return std::nullopt;
    }
    
    /**
     * @brief Clear all filter entries
     */
    void clear();
    
    /**
     * @brief Get number of entries
     * @return Entry count
     */
    size_t entryCount() const { return _entries.size(); }
    
    /**
     * @brief Check if group address should be filtered
     * 
     * @param addr Group address to check
     * @return FilterAction (Allow or Block)
     */
    FilterAction checkFilter(const GroupAddress& addr) const;
    
    
    /**
     * @brief Load filter table from binary data
     * @param data Binary filter table data
    * @return Result<void> indicating success or error
     */
    util::Result<void> loadTable(std::span<const uint8_t> data);
    
    /**
     * @brief Save filter table to binary data
     * @param out Output buffer for filter table
    * @return Result<size_t> number of bytes written or error
     */
    util::Result<size_t> saveTable(std::span<uint8_t> out) const;

private:
    std::vector<FilterEntry> _entries;
    FilterAction _defaultAction;
    
    /**
     * @brief Find matching filter entry
     * @param addr Address to match
     * @return Pointer to matching entry, or nullptr
     */
    const FilterEntry* findEntry(const GroupAddress& addr) const;
};

} // namespace network
} // namespace knx
