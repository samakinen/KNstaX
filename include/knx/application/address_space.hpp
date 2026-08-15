// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file address_space.hpp
 * @brief KNX memory address space management
 * 
 * Defines the memory address space layout and provides validation
 * for memory access operations per KNX spec 3/5/1.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>

#include "knx/types.hpp"
#include "knx/util/result.hpp"

namespace knx {
namespace application {

/**
 * @brief Memory access modes
 */
enum class MemoryAccessMode : uint8_t {
    ReadOnly = 0,
    WriteOnly = 1,
    ReadWrite = 2,
    NoAccess = 3
};

/**
 * @brief Memory region descriptor
 */
struct MemoryRegion {
    MemoryAddress startAddress;   ///< Starting address
    uint32_t size;                ///< Size in bytes
    MemoryAccessMode accessMode;  ///< Access permissions
    const char* name;             ///< Region name for debugging

    constexpr bool contains(MemoryAddress address) const {
        const uint32_t start = static_cast<uint32_t>(startAddress.raw);
        const uint32_t addr = static_cast<uint32_t>(address.raw);
        return addr >= start && addr < start + size;
    }

    constexpr bool containsRange(MemoryAddress address, uint8_t length) const {
        if (length == 0) {
            return false;
        }
        const uint32_t addr = static_cast<uint32_t>(address.raw);
        const uint32_t endAddress = addr + length - 1;
        const uint32_t regionStart = static_cast<uint32_t>(startAddress.raw);
        const uint32_t regionEnd = regionStart + size - 1;
        return contains(address) &&
               endAddress >= regionStart &&
               endAddress <= regionEnd;
    }

    constexpr bool overlaps(const MemoryRegion& other) const {
        const uint32_t start = static_cast<uint32_t>(startAddress.raw);
        const uint32_t end = start + size - 1;
        const uint32_t otherStart = static_cast<uint32_t>(other.startAddress.raw);
        const uint32_t otherEnd = otherStart + other.size - 1;
        return (start <= otherEnd) && (otherStart <= end);
    }
};

/**
 * @brief Standard KNX memory regions (System 7)
 * 
 * Based on KNX spec 3/5/1 - Application Layer
 */
namespace MemoryRegions {
    // System region (0x0000 - 0x00FF)
    constexpr MemoryAddress SYSTEM_START = MemoryAddress(0x0000);
    constexpr uint32_t SYSTEM_SIZE = 0x0100;
    
    // Application program (0x0100 - varies by device)
    constexpr MemoryAddress APP_PROGRAM_START = MemoryAddress(0x0100);
    constexpr uint32_t APP_PROGRAM_MAX_SIZE = 0xF000;  // Up to 60KB typical
    
    // Parameter memory (device-specific)
    constexpr MemoryAddress PARAM_START = MemoryAddress(0x0000);  // Usually overlaps with app program
    constexpr uint32_t PARAM_MAX_SIZE = 0x1000;
    
    // EEPROM/User memory (device-specific)
    constexpr MemoryAddress USER_START = MemoryAddress(0x0000);
    constexpr uint32_t USER_MAX_SIZE = 0x10000;
}

/**
 * @brief Address space manager
 * 
 * Manages memory regions and validates access operations.
 */
class AddressSpace {
public:
    /// Maximum number of memory regions
    static constexpr size_t MAX_REGIONS = 16;
    
    /**
     * @brief Initialize address space with no regions
     */
    AddressSpace();
    
    /**
     * @brief Add a memory region
     * @param region Region descriptor
     * @return true if added successfully, false if table full or overlap
     */
    util::Result<void> addRegion(const MemoryRegion& region);
    
    /**
     * @brief Remove all regions
     */
    void clearRegions();
    
    /**
     * @brief Validate a read operation
     * @param address Starting address
     * @param length Number of bytes
    * @return Result<void> indicating success or error
     */
    util::Result<void> canRead(MemoryAddress address, uint8_t length) const;

    
    /**
     * @brief Validate a write operation
     * @param address Starting address
     * @param length Number of bytes
    * @return Result<void> indicating success or error
     */
    util::Result<void> canWrite(MemoryAddress address, uint8_t length) const;

    
    /**
     * @brief Get access mode for an address range
     * @param address Starting address
     * @param length Number of bytes
     * @return Access mode if entire range is in one region, NoAccess otherwise
     */
    MemoryAccessMode getAccessMode(MemoryAddress address, uint8_t length) const;
    
    /**
     * @brief Find region containing an address
     * @param address Address to lookup
     * @return Pointer to region or nullptr if not found
     */
    const MemoryRegion* findRegion(MemoryAddress address) const;
    
    /**
     * @brief Get number of registered regions
     */
    size_t getRegionCount() const { return _regionCount; }
    
    /**
     * @brief Get region by index
     * @param index Region index
     * @return Pointer to region or nullptr if invalid index
     */
    const MemoryRegion* getRegion(size_t index) const;
    
private:
    MemoryRegion _regions[MAX_REGIONS];
    size_t _regionCount;
    
};

} // namespace application
} // namespace knx
