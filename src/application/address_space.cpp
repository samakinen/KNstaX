// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file address_space.cpp
 * @brief KNX memory address space implementation
 */

#include "knx/application/address_space.hpp"
#include <algorithm>

namespace knx {
namespace application {

util::Result<void> AddressSpace::addRegion(const MemoryRegion& region, std::span<uint8_t> storage,
                                           MemoryWriteGuard writeGuard) {
    if (_entries.size() >= MAX_REGIONS) {
        return util::ErrorCode::ResourceUnavailable;
    }

    // Check for invalid region
    if (region.size == 0) {
        return util::ErrorCode::InvalidParameter;
    }
    if (!storage.empty() && storage.size() < region.size) {
        return util::ErrorCode::InvalidParameter;
    }

    // Check for overlaps with existing regions
    for (const auto& entry : _entries) {
        if (entry.region.overlaps(region)) {
            return util::ErrorCode::OperationFailed;
        }
    }

    (void)_entries.push_back(Entry{region, storage, std::move(writeGuard)});
    return util::Result<void>::ok();
}

void AddressSpace::clearRegions() {
    _entries.clear();
}

util::Result<void> AddressSpace::canRead(MemoryAddress address, uint8_t length) const {
    if (length == 0) {
        return util::ErrorCode::InvalidParameter;
    }
    
    const MemoryRegion* region = findRegion(address);
    if (!region) {
        return util::ErrorCode::InvalidAddress;
    }
    
    // Check if entire range is within region
    if (!region->containsRange(address, length)) {
        return util::ErrorCode::InvalidAddress;
    }
    
    // Check access mode
    if (region->accessMode == MemoryAccessMode::ReadOnly ||
        region->accessMode == MemoryAccessMode::ReadWrite) {
        return util::Result<void>::ok();
    }

    return util::ErrorCode::OperationNotSupported;
}

util::Result<void> AddressSpace::canWrite(MemoryAddress address, uint8_t length) const {
    if (length == 0) {
        return util::ErrorCode::InvalidParameter;
    }
    
    const MemoryRegion* region = findRegion(address);
    if (!region) {
        return util::ErrorCode::InvalidAddress;
    }
    
    // Check if entire range is within region
    if (!region->containsRange(address, length)) {
        return util::ErrorCode::InvalidAddress;
    }
    
    // Check access mode
    if (region->accessMode == MemoryAccessMode::WriteOnly ||
        region->accessMode == MemoryAccessMode::ReadWrite) {
        return util::Result<void>::ok();
    }

    return util::ErrorCode::OperationNotSupported;
}

util::Result<void> AddressSpace::read(MemoryAddress address, std::span<uint8_t> out) const {
    const Entry* entry = findEntry(address, out.size());
    if (entry == nullptr) {
        return util::ErrorCode::InvalidAddress;
    }
    const uint32_t offset = static_cast<uint32_t>(address.raw)
                          - static_cast<uint32_t>(entry->region.startAddress.raw);
    std::copy_n(entry->storage.data() + offset, out.size(), out.data());
    return util::Result<void>::ok();
}

util::Result<void> AddressSpace::write(MemoryAddress address, std::span<const uint8_t> data) {
    const Entry* entry = findEntry(address, data.size());
    if (entry == nullptr) {
        return util::ErrorCode::InvalidAddress;
    }
    if (entry->writeGuard && !entry->writeGuard()) {
        return util::ErrorCode::AccessDenied;
    }
    const uint32_t offset = static_cast<uint32_t>(address.raw)
                          - static_cast<uint32_t>(entry->region.startAddress.raw);
    std::copy(data.begin(), data.end(), entry->storage.data() + offset);
    return util::Result<void>::ok();
}

MemoryAccessMode AddressSpace::getAccessMode(MemoryAddress address, uint8_t length) const {
    if (length == 0) {
        return MemoryAccessMode::NoAccess;
    }
    
    const MemoryRegion* region = findRegion(address);
    if (!region) {
        return MemoryAccessMode::NoAccess;
    }
    
    // Verify entire range is within region
    if (!region->containsRange(address, length)) {
        return MemoryAccessMode::NoAccess;
    }
    
    return region->accessMode;
}

const MemoryRegion* AddressSpace::findRegion(MemoryAddress address) const {
    for (const auto& entry : _entries) {
        if (entry.region.contains(address)) {
            return &entry.region;
        }
    }
    return nullptr;
}

const MemoryRegion* AddressSpace::getRegion(size_t index) const {
    if (index >= _entries.size()) {
        return nullptr;
    }
    return &_entries[index].region;
}

const AddressSpace::Entry* AddressSpace::findEntry(MemoryAddress address, size_t length) const {
    if (length == 0) {
        return nullptr;
    }
    const uint32_t first = static_cast<uint32_t>(address.raw);
    const uint32_t last = first + static_cast<uint32_t>(length) - 1u;
    for (const auto& entry : _entries) {
        const uint32_t start = static_cast<uint32_t>(entry.region.startAddress.raw);
        const uint32_t end = start + entry.region.size - 1u;
        if (first >= start && last <= end) {
            return entry.storage.empty() ? nullptr : &entry;
        }
    }
    return nullptr;
}

} // namespace application
} // namespace knx
