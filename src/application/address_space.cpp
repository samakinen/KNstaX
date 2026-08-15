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

AddressSpace::AddressSpace() : _regionCount(0) {
    std::fill_n(_regions, MAX_REGIONS, MemoryRegion{MemoryAddress(0), 0, MemoryAccessMode::NoAccess, nullptr});
}

util::Result<void> AddressSpace::addRegion(const MemoryRegion& region) {
    if (_regionCount >= MAX_REGIONS) {
        return util::ErrorCode::ResourceUnavailable;
    }

    // Check for invalid region
    if (region.size == 0) {
        return util::ErrorCode::InvalidParameter;
    }

    // Check for overlaps with existing regions
    for (size_t i = 0; i < _regionCount; i++) {
        if (_regions[i].overlaps(region)) {
            return util::ErrorCode::OperationFailed;
        }
    }

    _regions[_regionCount++] = region;
    return util::Result<void>::ok();
}

void AddressSpace::clearRegions() {
    _regionCount = 0;
    std::fill_n(_regions, MAX_REGIONS, MemoryRegion{MemoryAddress(0), 0, MemoryAccessMode::NoAccess, nullptr});
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
    for (size_t i = 0; i < _regionCount; i++) {
        const auto& region = _regions[i];
        if (region.contains(address)) {
            return &region;
        }
    }
    return nullptr;
}

const MemoryRegion* AddressSpace::getRegion(size_t index) const {
    if (index >= _regionCount) {
        return nullptr;
    }
    return &_regions[index];
}

} // namespace application
} // namespace knx
