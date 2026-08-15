// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file routing_table.cpp
 * @brief Routing table implementation
 */

#include "knx/network/routing_table.hpp"
#include "knx/util/log.hpp"
#include "knx/util/bit_ops.hpp"
#include "knx/util/result.hpp"
#include <algorithm>

static const char* TAG = "KNX.Routing";
namespace bits = knx::util;

namespace knx {
namespace network {

RoutingTable::RoutingTable() {
    _entries.reserve(8);  // Reserve common size
}

int RoutingTable::addEntry(const RoutingEntry& entry) {
    if (_entries.size() >= MAX_ENTRIES) {
        KNX_LOGW(TAG, "Routing table full");
        return -1;
    }
    
    _entries.push_back(entry);
    KNX_LOGD(TAG, "Added routing entry for %d.%d.%d (hop=%d)",
             entry.destination.area(), entry.destination.line(), entry.destination.device(),
             entry.hopCount);
    
    return static_cast<int>(_entries.size() - 1);
}

util::Result<void> RoutingTable::removeEntry(size_t index) {
    if (index >= _entries.size()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    _entries.erase(_entries.begin() + static_cast<std::vector<RoutingEntry>::difference_type>(index));
    KNX_LOGD(TAG, "Removed routing entry %zu", index);
    return util::Result<void>::ok();
}

const RoutingEntry* RoutingTable::getEntry(size_t index) const {
    if (index >= _entries.size()) {
        return nullptr;
    }
    return &_entries[index];
}

util::Result<void> RoutingTable::updateEntry(size_t index, const RoutingEntry& entry) {
    if (index >= _entries.size()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    _entries[index] = entry;
    return util::Result<void>::ok();
}

void RoutingTable::clear() {
    _entries.clear();
    KNX_LOGI(TAG, "Cleared routing table");
}

RoutingDecision RoutingTable::route(
    const IndividualAddress& source,
    const GroupAddress& dest,
    AddressType destinationType,
    uint8_t hopCount,
    const IndividualAddress& ownAddress) const
{
    // Check hop count
    if (hopCount == 0) {
        KNX_LOGD(TAG, "Frame dropped: hop count exhausted");
        return RoutingDecision::Drop;
    }
    
    // Group address routing
    if (destinationType == AddressType::Group) {
        // Group telegrams are broadcast - forward to all connected networks
        // Filter table handles group address filtering
        return RoutingDecision::Forward;
    }
    
    // Individual address routing
    IndividualAddress destAddr(dest.raw);
    
    // Don't route to self
    if (destAddr.raw == ownAddress.raw) {
        KNX_LOGD(TAG, "Frame for self, not routing");
        return RoutingDecision::Drop;
    }
    
    // Source and destination identical: a malformed or looped-back frame, since
    // no device addresses a telegram to itself. Dropped rather than forwarded so
    // it cannot circulate.
    if (destAddr.raw == source.raw) {
        return RoutingDecision::Drop;
    }
    
    // Check if on same line - no routing needed
    if (isOnSameLine(destAddr, ownAddress)) {
        return RoutingDecision::ForwardLocal;
    }
    
    // Check if on same area - route within area
    if (isOnSameArea(destAddr, ownAddress)) {
        return RoutingDecision::Forward;
    }
    
    // Cross-area routing - check routing table
    const RoutingEntry* entry = findEntry(destAddr);
    if (entry && isEnabled(entry->enabled)) {
        // Check hop count limit for this route
        if (hopCount > entry->hopCount) {
            KNX_LOGD(TAG, "Frame dropped: hop count %d exceeds route limit %d",
                     hopCount, entry->hopCount);
            return RoutingDecision::Drop;
        }
        return RoutingDecision::Forward;
    }
    
    // No routing entry found - drop by default (could be configured)
    KNX_LOGD(TAG, "No routing entry for %d.%d.%d",
             destAddr.area(), destAddr.line(), destAddr.device());
    return RoutingDecision::Block;
}

bool RoutingTable::isOnSameLine(const IndividualAddress& dest, const IndividualAddress& ownAddress) {
    return (dest.area() == ownAddress.area()) && (dest.line() == ownAddress.line());
}

bool RoutingTable::isOnSameArea(const IndividualAddress& dest, const IndividualAddress& ownAddress) {
    return dest.area() == ownAddress.area();
}

util::Result<void> RoutingTable::loadTable(std::span<const uint8_t> data) {
    // Simple serialization format:
    // Entry count (2 bytes) + entries (6 bytes each)
    // Format: dest_addr(2) + mask(2) + hopCount(1) + enabled(1)
    
    if (data.size() < 2) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    uint16_t count = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    const size_t expectedSize = 2u + (static_cast<size_t>(count) * 6u);
    if (data.size() != expectedSize) {
        KNX_LOGE(TAG, "Invalid routing table size");
        return util::Result<void>::err(util::ErrorCode::InvalidFrameSize);
    }
    
    clear();
    
    size_t offset = 2;
    for (uint16_t i = 0; i < count; i++) {
        RoutingEntry entry;
        entry.destination.raw = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        entry.mask.raw = (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
        entry.hopCount = data[offset + 4];
        entry.enabled = (data[offset + 5] != 0) ? EntryState::Enabled : EntryState::Disabled;
        
        addEntry(entry);
        offset += 6;
    }
    
    KNX_LOGI(TAG, "Loaded %zu routing entries", _entries.size());
    return util::Result<void>::ok();
}

util::Result<size_t> RoutingTable::saveTable(std::span<uint8_t> out) const {
    const size_t required = 2u + (_entries.size() * 6u);
    if (out.size() < required) {
        return util::Result<size_t>(util::ErrorCode::BufferTooSmall);
    }

    size_t idx = 0;
    uint16_t count = static_cast<uint16_t>(_entries.size());
    out[idx++] = bits::getHighByte(count);
    out[idx++] = bits::getLowByte(count);

    for (const auto& entry : _entries) {
        out[idx++] = bits::getHighByte(entry.destination.raw);
        out[idx++] = bits::getLowByte(entry.destination.raw);
        out[idx++] = bits::getHighByte(entry.mask.raw);
        out[idx++] = bits::getLowByte(entry.mask.raw);
        out[idx++] = entry.hopCount;
        out[idx++] = static_cast<uint8_t>(isEnabled(entry.enabled) ? 1u : 0u);
    }

    return util::Result<size_t>(required);
}

const RoutingEntry* RoutingTable::findEntry(const IndividualAddress& addr) const {
    for (const auto& entry : _entries) {
        if (entry.matches(addr) && isEnabled(entry.enabled)) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace network
} // namespace knx
