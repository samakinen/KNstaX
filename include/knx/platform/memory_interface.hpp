// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file memory_interface.hpp
 * @brief Memory abstraction interface
 * 
 * Provides abstraction for non-volatile memory access (Flash, EEPROM, etc.)
 */

#pragma once

#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <cstddef>
#include <span>

namespace knx {
namespace platform {

/**
 * @brief Memory type enumeration
 */
enum class MemoryType {
    EEPROM,     ///< EEPROM (byte-writable)
    Flash,      ///< Flash memory (page/sector erase required)
    RAM,        ///< RAM-backed (for testing)
    NVS         ///< Non-Volatile Storage (ESP32 NVS, etc.)
};

/**
 * @brief Memory interface abstraction
 * 
 * This interface abstracts non-volatile memory operations across different
 * memory types. Implementations handle the specifics of Flash page erase,
 * EEPROM emulation, NVS APIs, etc.
 */
class MemoryInterface {
public:
    virtual ~MemoryInterface() = default;
    
    /**
     * @brief Get memory type
     */
    virtual MemoryType type() const = 0;
    
    /**
     * @brief Get total memory size in bytes
     */
    virtual size_t size() const = 0;
    
    /**
     * @brief Get page/sector size (for Flash)
     */
    virtual size_t pageSize() const = 0;
    
    /**
     * @brief Get erase block size (for Flash)
     */
    virtual size_t eraseBlockSize() const = 0;
    
    /**
     * @brief Initialize memory (Result-based)
     * @return Result<void> indicating success or error
     */
    virtual util::Result<void> init() = 0;
    
    /**
     * @brief Read data from memory
     * @param address Offset from start of user memory
     * @param buffer Destination buffer span
     * @return Number of bytes read
     */
    virtual uint32_t read(uint32_t address, std::span<uint8_t> buffer) = 0;
    
    /**
     * @brief Write data to memory
     * @param address Offset from start of user memory
     * @param buffer Source buffer span
     * @return Number of bytes written
     */
    virtual uint32_t write(uint32_t address, std::span<const uint8_t> buffer) = 0;
    
    /**
     * @brief Write repeated byte value
     * @param address Offset from start of user memory
     * @param value Byte value to write
     * @param repeat Number of times to repeat
     * @return Number of bytes written
     */
    virtual uint32_t write(uint32_t address, uint8_t value, size_t repeat) = 0;
    
    /**
     * @brief Commit changes to non-volatile storage
     * 
     * For Flash: Writes buffered page to flash
     * For EEPROM emulation: Persists changes
     * For NVS: Commits transaction
     */
    virtual void commit() = 0;
    
    /**
     * @brief Erase memory region (Result-based)
     * @param address Start address (should be aligned to erase block)
     * @param length Number of bytes (should be multiple of erase block size)
     * @return Result<void> indicating success or error
     */
    virtual util::Result<void> erase(uint32_t address, size_t length) = 0;
    
    /**
     * @brief Get direct pointer to memory buffer (if available)
     * @return Pointer to memory or nullptr if not accessible
     * 
     * This is useful for read-only access to avoid copying.
     * Only valid for EEPROM emulation or RAM-backed storage.
     */
    virtual std::span<uint8_t> getBuffer(uint32_t address, size_t length) = 0;
    
protected:
    MemoryInterface() = default;
};

/**
 * @brief Flash memory interface
 * 
 * Extended interface for Flash-specific operations
 */
class FlashMemoryInterface : public MemoryInterface {
public:
    /**
     * @brief Get number of erase blocks
     */
    virtual size_t eraseBlockCount() const = 0;
    
    /**
     * @brief Erase specific erase block
     * @param blockNumber Block number (0-based)
     */
    virtual void eraseBlock(uint16_t blockNumber) = 0;
    
    /**
     * @brief Write a page
     * @param pageNumber Page number relative to user flash start
     * @param data Page data span
     */
    virtual void writePage(uint16_t pageNumber, std::span<const uint8_t> data) = 0;
    
    /**
     * @brief Check if erase block is dirty (needs writing)
     */
    virtual bool isEraseBlockDirty() const = 0;
    
    /**
     * @brief Flush dirty erase block to flash
     */
    virtual void flushEraseBlock() = 0;
};

/**
 * @brief Memory callback interface
 * 
 * For platforms that provide custom memory management callbacks
 */
struct MemoryCallbacks {
    using SizeCallback = uint32_t (*)();
    using ReadCallback = std::span<uint8_t> (*)();
    using WriteCallback = uint32_t (*)(uint32_t relativeAddress, std::span<const uint8_t> buffer);
    using CommitCallback = void (*)();
    
    SizeCallback getSize;
    ReadCallback getBuffer;
    WriteCallback write;
    CommitCallback commit;
};

} // namespace platform
} // namespace knx
