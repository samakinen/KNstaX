// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file routing_table.hpp
 * @brief KNX routing table for line/area couplers
 * 
 * Manages routing decisions for KNX frames across network boundaries.
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
 * @brief Routing decision for a frame
 */
enum class RoutingDecision {
    Forward,      ///< Forward frame to destination
    ForwardLocal, ///< Forward to local network only
    Block,        ///< Block frame (filter rule)
    Drop          ///< Drop frame (invalid/expired)
};

/**
 * @brief Routing table entry
 * 
 * Defines how to route frames to specific areas/lines.
 */
struct RoutingEntry {
    IndividualAddress destination;  ///< Destination address or range
    IndividualAddress mask;          ///< Address mask (0=wildcard)
    uint8_t hopCount;                ///< Maximum hop count for this route
    EntryState enabled;              ///< Entry is active
    
    RoutingEntry()
        : destination(0)
        , mask(0xFFFF)
        , hopCount(6)
        , enabled(EntryState::Enabled)
    {
    }
    
    /**
     * @brief Check if address matches this entry
     * @param addr Address to check
     * @return true if address matches mask
     */
    bool matches(const IndividualAddress& addr) const {
        return (addr.raw & mask.raw) == (destination.raw & mask.raw);
    }
};

/**
 * @brief Routing table for network layer
 * 
 * Manages routing decisions for frames crossing network boundaries.
 * Used by line couplers and area couplers.
 */
class RoutingTable {
public:
    /// Maximum routing table entries
    static constexpr size_t MAX_ENTRIES = 32;
    
    RoutingTable();
    ~RoutingTable() = default;
    
    /**
     * @brief Add routing entry
     * @param entry Routing entry to add
     * @return Index of entry, or -1 if table full
     */
    int addEntry(const RoutingEntry& entry);
    
    /**
     * @brief Add routing entry (convenience method)
     * @param destination Destination address
     * @param addressMask Address mask
     * @param hopCount Maximum hop count
     * @param enabled Entry enabled
     * @return Result<void> indicating success or error
     */
    util::Result<void> addEntry(const IndividualAddress& destination, const IndividualAddress& addressMask, uint8_t hopCount, EntryState enabled) {
        RoutingEntry entry;
        entry.destination = destination;
        entry.mask = addressMask;
        entry.hopCount = hopCount;
        entry.enabled = enabled;
        return addEntry(entry) >= 0
            ? util::Result<void>::ok()
            : util::Result<void>::err(util::ErrorCode::QueueFull);
    }
    
    /**
     * @brief Remove routing entry
     * @param index Entry index to remove
    * @return Result<void> indicating success or error
     */
    util::Result<void> removeEntry(size_t index);
    
    /**
     * @brief Get routing entry
     * @param index Entry index
     * @return Pointer to entry, or nullptr if invalid index
     */
    const RoutingEntry* getEntry(size_t index) const;
    
    /**
     * @brief Update routing entry
     * @param index Entry index
     * @param entry New entry data
    * @return Result<void> indicating success or error
     */
    util::Result<void> updateEntry(size_t index, const RoutingEntry& entry);
    
    /**
     * @brief Update routing entry (convenience method)
     * @param index Entry index
     * @param destination Destination address
     * @param addressMask Address mask
     * @param hopCount Maximum hop count
     * @param enabled Entry enabled
     * @return true if updated
     */
    util::Result<void> updateEntry(size_t index, const IndividualAddress& destination, const IndividualAddress& addressMask, uint8_t hopCount, EntryState enabled) {
        RoutingEntry entry;
        entry.destination = destination;
        entry.mask = addressMask;
        entry.hopCount = hopCount;
        entry.enabled = enabled;
        return updateEntry(index, entry);
    }
    
    /**
     * @brief Get routing entry (optional-based, for tests)
     * @param index Entry index
     * @return Optional containing entry if found
     */
    std::optional<RoutingEntry> getEntryOpt(size_t index) const {
        const RoutingEntry* entry = getEntry(index);
        if (entry) {
            return *entry;
        }
        return std::nullopt;
    }
    
    /**
     * @brief Clear all routing entries
     */
    void clear();
    
    /**
     * @brief Get number of entries
     * @return Entry count
     */
    size_t getEntryCount() const { return _entries.size(); }
    
    /**
     * @brief Get number of entries
     * @return Entry count
     */
    size_t entryCount() const { return _entries.size(); }
    
    /**
     * @brief Determine routing decision for a frame
     * 
     * @param source Source individual address
     * @param dest Destination address (individual or group)
      * @param destinationType Destination address type
     * @param hopCount Current hop count
     * @param ownAddress Own device address
     * @return Routing decision
     */
    RoutingDecision route(
        const IndividualAddress& source,
        const GroupAddress& dest,
          AddressType destinationType,
        uint8_t hopCount,
        const IndividualAddress& ownAddress) const;
    
    /**
     * @brief Simplified routing decision (for testing)
     * @param dest Destination address
     * @param destinationType Destination address type
     * @param hopCount Current hop count
     * @param ownAddress Own device address
     * @return Routing decision
     */
    RoutingDecision route(const GroupAddress& dest, AddressType destinationType, uint8_t hopCount, const IndividualAddress& ownAddress) const {
        return route(
            IndividualAddress(0),  // Source not relevant for basic routing
            dest,
            destinationType,
            hopCount,
            ownAddress
        );
    }
    
    /**
     * @brief Check if destination is on same line
     * @param dest Destination address
     * @param ownAddress Own address
     * @return true if same line
     */
    static bool isOnSameLine(const IndividualAddress& dest, const IndividualAddress& ownAddress);
    
    /**
     * @brief Check if destination is on same area
     * @param dest Destination address
     * @param ownAddress Own address
     * @return true if same area
     */
    static bool isOnSameArea(const IndividualAddress& dest, const IndividualAddress& ownAddress);
    
    /**
     * @brief Load routing table from binary data
     * @param data Binary routing table data
    * @return Result<void> indicating success or error
     */
    util::Result<void> loadTable(std::span<const uint8_t> data);
    
    /**
     * @brief Save routing table to binary data
     * @param out Output buffer for routing table
    * @return Result<size_t> number of bytes written or error
     */
    util::Result<size_t> saveTable(std::span<uint8_t> out) const;

private:
    std::vector<RoutingEntry> _entries;
    
    /**
     * @brief Find matching routing entry
     * @param addr Address to match
     * @return Pointer to matching entry, or nullptr
     */
    const RoutingEntry* findEntry(const IndividualAddress& addr) const;
};

} // namespace network
} // namespace knx
