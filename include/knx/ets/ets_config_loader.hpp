// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once
#include <cstdint>
#include <vector>
#include <cstring>
#include <span>
#include <string>
#include "knx/types.hpp"
#include "knx/util/result.hpp"

namespace knx {
namespace ets {

/**
 * ETS Configuration Binary Loader
 * 
 * Parses ETS-generated .knxprog binaries and loads configuration tables:
 * - Device Object (individual address, manufacturer ID, etc.)
 * - Group Object Table (DPTs, flags)
 * - Address Table (Group Address → ASAP mappings)
 * - Association Table (ASAP → Group Object Index)
 * - Application Program Object
 */
class EtsConfigLoader
{
public:
    /**
     * @brief Validation level for configuration loading
     */
    enum class ValidationLevel {
        NoValidation = 0,    ///< Skip all validation (fast but unsafe)
        ChecksumOnly = 1,    ///< Validate checksums only
        Full = 2             ///< Full validation (version, format, checksums)
    };

    /**
     * @brief Detailed load result with diagnostics
     */
    struct LoadResult {
        bool success;                   ///< Overall success flag
        uint8_t formatVersion;          ///< Detected format version
        std::string diagnosticMessage;  ///< Human-readable diagnostic info
        uint32_t bytesConsumed;         ///< Number of bytes processed
        bool checksumValid;             ///< Checksum validation result
        
        LoadResult() 
            : success(false), formatVersion(0), bytesConsumed(0), checksumValid(false) {}
    };

    struct GroupObjectConfig
    {
        GroupObjectIndex index;     ///< Group object index (0-255)
        uint16_t dpt;               ///< Data Point Type
        uint8_t flags;              ///< Read/Write/Transmit/Receive flags
        uint16_t initialValue;      ///< Initial value
    };

    struct AddressTableEntry
    {
        GroupAddress groupAddress;  ///< KNX group address (e.g., 1/2/3)
        uint8_t asap;               ///< Association table SAP index
    };

    struct AssociationTableEntry
    {
        uint8_t asap;               ///< Association table index
        GroupObjectIndex groupObjectIndex;   ///< Group object index
        uint8_t priority;           ///< Communication priority
    };

    struct DeviceConfig
    {
        IndividualAddress individualAddress; ///< Individual address (e.g., 1.1.1)
        uint16_t manufacturerId;    ///< Manufacturer ID
        uint32_t serialNumber;      ///< Device serial number
        uint8_t appVersion;         ///< Application version
        uint8_t configured;         ///< Configured flag (1=yes, 0=no)
    };

    /**
     * Load ETS configuration from binary buffer
     * 
     * @param buffer ETS binary data (memory-mapped or in-memory)
     * @param size   Size of buffer in bytes
     * @param level  Validation level (default: Full)
    * @return Result<void> indicating success or error
     */
    util::Result<void> loadFromBuffer(std::span<const uint8_t> buffer, 
                   ValidationLevel level = ValidationLevel::Full);

    /**
     * Load ETS configuration with detailed diagnostics
     * 
     * @param buffer ETS binary data
     * @param size   Size of buffer in bytes
     * @param level  Validation level (default: Full)
     * @return LoadResult with detailed diagnostic information
     */
    LoadResult loadWithDiagnostics(std::span<const uint8_t> buffer,
                                   ValidationLevel level = ValidationLevel::Full);

    /**
     * Load ETS configuration from file (Linux/testing only)
     * 
     * @param filename Path to .knxprog or .bin file
     * @param level    Validation level (default: Full)
    * @return Result<void> indicating success or error
     */
    util::Result<void> loadFromFile(const char* filename, 
                 ValidationLevel level = ValidationLevel::Full);

    /**
     * Save ETS configuration to buffer in version 1 format
     * 
     * @param buffer Output buffer (must be pre-allocated)
     * @param size   Buffer size (updated with actual bytes written)
    * @return Result<void> indicating success or error
     */
    util::Result<void> saveToBuffer(std::span<uint8_t> buffer, uint32_t& size) const;

    /**
     * Save ETS configuration to file (Linux/testing only)
     * 
     * @param filename Path to output file
    * @return Result<void> indicating success or error
     */
    util::Result<void> saveToFile(const char* filename) const;

    /**
     * Calculate required buffer size for saving
     * 
     * @return Required buffer size in bytes
     */
    uint32_t calculateRequiredSize() const;

    // Accessors
    const DeviceConfig& deviceConfig() const { return _deviceConfig; }
    const std::vector<GroupObjectConfig>& groupObjects() const { return _groupObjects; }
    const std::vector<AddressTableEntry>& addressTable() const { return _addressTable; }
    const std::vector<AssociationTableEntry>& associationTable() const { return _associationTable; }

    // Utility: find group object by index
    const GroupObjectConfig* findGroupObject(GroupObjectIndex index) const;

    // Utility: find ASAP by group address
    int findAsapByAddress(const GroupAddress& groupAddr) const;

    // Utility: find group object index by ASAP
    GroupObjectIndex findGoIndexByAsap(uint8_t asap) const;

    // Check if configuration is valid and complete
    bool isValid() const { return _isValid; }

    // Get detected format version
    uint8_t formatVersion() const { return _detectedFormatVersion; }

    // Statistics
    uint32_t groupObjectCount() const { return static_cast<uint32_t>(_groupObjects.size()); }
    uint32_t addressTableSize() const { return static_cast<uint32_t>(_addressTable.size()); }
    uint32_t associationTableSize() const { return static_cast<uint32_t>(_associationTable.size()); }

private:
    DeviceConfig _deviceConfig = {};
    std::vector<GroupObjectConfig> _groupObjects;
    std::vector<AddressTableEntry> _addressTable;
    std::vector<AssociationTableEntry> _associationTable;
    bool _isValid = false;
    uint8_t _detectedFormatVersion = 0;

    // Binary format parsing
    util::Result<void> _parseDeviceConfig(std::span<const uint8_t> buffer, uint32_t offset);
    util::Result<void> _parseGroupObjectTable(std::span<const uint8_t> buffer, uint32_t offset);
    util::Result<void> _parseAddressTable(std::span<const uint8_t> buffer, uint32_t offset);
    util::Result<void> _parseAssociationTable(std::span<const uint8_t> buffer, uint32_t offset);
    
    // Format validation helpers
    util::Result<void> _parseFormatHeader(std::span<const uint8_t> buffer, uint32_t offset, uint32_t size);
    util::Result<void> _validateFormat(std::span<const uint8_t> buffer, ValidationLevel level);
    util::Result<void> _parseVersionedFormat(std::span<const uint8_t> buffer);
    
    // Binary format writing
    util::Result<void> _writeVersion1Format(std::span<uint8_t> buffer, uint32_t& offset) const;
    util::Result<void> _writeDeviceConfig(std::span<uint8_t> buffer, uint32_t& offset) const;
    util::Result<void> _writeAddressTable(std::span<uint8_t> buffer, uint32_t& offset) const;
    util::Result<void> _writeAssociationTable(std::span<uint8_t> buffer, uint32_t& offset) const;
    util::Result<void> _writeGroupObjectTable(std::span<uint8_t> buffer, uint32_t& offset) const;
};

} // namespace ets
} // namespace knx
