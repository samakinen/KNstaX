// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file filter_table.cpp
 * @brief Filter table implementation
 */

#include "knx/network/filter_table.hpp"
#include "knx/util/log.hpp"
#include "knx/util/bit_ops.hpp"
#include "knx/util/result.hpp"
#include <algorithm>

static const char* TAG = "KNX.Filter";
namespace bits = knx::util;

namespace knx {
namespace network {

FilterTable::FilterTable()
    : _defaultAction(FilterAction::Allow)
{
    _entries.reserve(8);  // Reserve common size
}

int FilterTable::addEntry(const FilterEntry& entry) {
    if (_entries.size() >= MAX_ENTRIES) {
        KNX_LOGW(TAG, "Filter table full");
        return -1;
    }
    
    _entries.push_back(entry);
    KNX_LOGD(TAG, "Added filter entry for %d/%d/%d (%s)",
             entry.address.main(), entry.address.middle(), entry.address.sub(),
             entry.action == FilterAction::Allow ? "allow" : "block");
    
    return static_cast<int>(_entries.size() - 1);
}

util::Result<void> FilterTable::removeEntry(size_t index) {
    if (index >= _entries.size()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    _entries.erase(_entries.begin() + static_cast<std::vector<FilterEntry>::difference_type>(index));
    KNX_LOGD(TAG, "Removed filter entry %zu", index);
    return util::Result<void>::ok();
}

const FilterEntry* FilterTable::getEntry(size_t index) const {
    if (index >= _entries.size()) {
        return nullptr;
    }
    return &_entries[index];
}

util::Result<void> FilterTable::updateEntry(size_t index, const FilterEntry& entry) {
    if (index >= _entries.size()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    _entries[index] = entry;
    return util::Result<void>::ok();
}

void FilterTable::clear() {
    _entries.clear();
    KNX_LOGI(TAG, "Cleared filter table");
}

FilterAction FilterTable::checkFilter(const GroupAddress& addr) const {
    // Check all entries in order
    for (const auto& entry : _entries) {
        if (isEnabled(entry.enabled) && entry.matches(addr)) {
            return entry.action;
        }
    }
    
    // No match - return default action
    return _defaultAction;
}

util::Result<void> FilterTable::loadTable(std::span<const uint8_t> data) {
    // Simple serialization format:
    // Default action (1 byte) + Entry count (2 bytes) + entries (5 bytes each)
    // Format: addr(2) + mask(2) + action(1)
    
    if (data.size() < 3) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    _defaultAction = (data[0] != 0) ? FilterAction::Block : FilterAction::Allow;
    
    uint16_t count = (static_cast<uint16_t>(data[1]) << 8) | data[2];
    const size_t expectedSize = 3u + (static_cast<size_t>(count) * 5u);
    if (data.size() != expectedSize) {
        KNX_LOGE(TAG, "Invalid filter table size");
        return util::Result<void>::err(util::ErrorCode::InvalidFrameSize);
    }
    
    clear();
    
    size_t offset = 3;
    for (uint16_t i = 0; i < count; i++) {
        FilterEntry entry;
        entry.address.raw = (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
        entry.mask.raw = (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
        entry.action = (data[offset + 4] != 0) ? FilterAction::Block : FilterAction::Allow;
        entry.enabled = EntryState::Enabled;
        
        addEntry(entry);
        offset += 5;
    }
    
    KNX_LOGI(TAG, "Loaded %zu filter entries (default: %s)",
             _entries.size(),
             _defaultAction == FilterAction::Allow ? "allow" : "block");
    return util::Result<void>::ok();
}

util::Result<size_t> FilterTable::saveTable(std::span<uint8_t> out) const {
    const size_t required = 3u + (_entries.size() * 5u);
    if (out.size() < required) {
        return util::Result<size_t>(util::ErrorCode::BufferTooSmall);
    }

    size_t idx = 0;
    out[idx++] = static_cast<uint8_t>(_defaultAction == FilterAction::Block ? 1u : 0u);

    uint16_t count = static_cast<uint16_t>(_entries.size());
    out[idx++] = bits::getHighByte(count);
    out[idx++] = bits::getLowByte(count);

    for (const auto& entry : _entries) {
        out[idx++] = bits::getHighByte(entry.address.raw);
        out[idx++] = bits::getLowByte(entry.address.raw);
        out[idx++] = bits::getHighByte(entry.mask.raw);
        out[idx++] = bits::getLowByte(entry.mask.raw);
        out[idx++] = entry.action == FilterAction::Block ? 1 : 0;
    }

    return util::Result<size_t>(required);
}

const FilterEntry* FilterTable::findEntry(const GroupAddress& addr) const {
    for (const auto& entry : _entries) {
        if (entry.matches(addr) && isEnabled(entry.enabled)) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace network
} // namespace knx
