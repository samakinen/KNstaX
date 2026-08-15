// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property.hpp
 * @brief KNX Property definitions and types
 * 
 * Properties are the primary mechanism for device configuration and status.
 * Each interface object has a set of properties that can be read/written.
 * Per KNX spec 03/05/01 (Resources).
 */

#pragma once

#include <cstdint>
#include <span>
#include <vector>
#include <string>

namespace knx {
namespace application {

/**
 * @brief Property Data Type (PDT)
 * 
 * Defines the data type of a property value.
 */
// PropertyDataType generated from pdt_catalog.inc to keep single-source-of-truth
#define KNX_PDT_CATALOG_ENTRY(enumName, token, value, size, desc) enumName = value,
enum class PropertyDataType : uint8_t {
#include "knx/application/pdt_catalog.inc"
};
#undef KNX_PDT_CATALOG_ENTRY

/**
 * @brief Property Access Level
 */
enum class PropertyAccess : uint8_t {
    ReadOnly = 0,       ///< Read-only
    ReadWrite = 1,      ///< Read and write
    WriteOnly = 2       ///< Write-only (rare)
};

/**
 * @brief Standard Property IDs
 * 
 * Common property IDs defined by KNX specification.
 */
enum class PropertyID : uint8_t {
    // General properties (0-10)
    ObjectType = 1,                 ///< Interface object type
    ObjectName = 2,                 ///< Object name
    Semaphor = 3,                   ///< Semaphore
    GroupObjectReference = 4,       ///< Group object reference
    LoadStateControl = 5,           ///< Load state
    RunStateControl = 6,            ///< Run state
    TableReference = 7,             ///< Table reference
    ServiceControl = 8,             ///< Service control
    FirmwareRevision = 9,           ///< Firmware revision
    ServicesSupported = 10,         ///< Supported services
    SerialNumber = 11,              ///< Serial number
    ManufacturerId = 12,            ///< Manufacturer ID
    ProgramVersion = 13,            ///< Application version
    DeviceControl = 14,             ///< Device control
    OrderInfo = 15,                 ///< Order information
    PeiType = 16,                   ///< PEI type
    PortConfiguration = 17,         ///< Port config
    PollGroupSettings = 18,         ///< Poll group
    ManufacturerData = 19,          ///< Manufacturer data
    Enable = 20,                    ///< Enable flag
    Description = 21,               ///< Description
    File = 22,                      ///< File property
    Table = 23,                     ///< Table property
    Enrol = 24,                     ///< Enrollment
    Version = 25,                   ///< Version
    GroupObjectLink = 26,           ///< GO link
    McbTable = 27,                  ///< MCB table
    ErrorCode = 28,                 ///< Error code
    ObjectIndex = 29,               ///< Object index
    
    // Device object specific (51-60)
    RoutingCount = 51,              ///< Max routing count
    MaxRetryCount = 52,             ///< Max retry count
    ErrorFlags = 53,                ///< Error flags
    ProgMode = 54,                  ///< Programming mode
    ProductId = 55,                 ///< Product identification
    MaxApduLength = 56,             ///< Max APDU length
    SubnetAddress = 57,             ///< Subnet address (area.line)
    DeviceAddress = 58,             ///< Device address
    PbConfig = 59,                  ///< PB config
    AddressReport = 60,             ///< Address report
    AddressCheck = 61,              ///< Address check
    ObjectValue = 62,               ///< Object value
    ObjectLink = 63,                ///< Object link
    Application = 64,               ///< Application
    Parameter = 65,                 ///< Parameter
    ObjectAddress = 66,             ///< Object address
    PsuType = 67,                   ///< PSU type
    PsuStatus = 68,                 ///< PSU status
    PsuEnable = 69,                 ///< PSU enable
    DomainAddress = 70,             ///< Domain address (KNX RF)
    IoList = 71,                    ///< I/O list
    MgtDescriptor01 = 72,           ///< Management descriptor
    
    // Custom/manufacturer-specific (200+)
    CustomStart = 200               ///< Start of custom properties
};

/**
 * @brief Property descriptor
 * 
 * Metadata describing a property.
 */
struct PropertyDescriptor {
    PropertyID id;                  ///< Property ID
    PropertyDataType type;          ///< Data type
    PropertyAccess access;          ///< Access rights
    uint16_t maxElements;           ///< Maximum number of elements
    uint8_t readLevel;              ///< Read access level (0-15)
    uint8_t writeLevel;             ///< Write access level (0-15)
    
    /**
     * @brief Get element size in bytes
     * @return Size of one element
     */
    uint8_t getElementSize() const {
        switch (type) {
#define KNX_PDT_CATALOG_ENTRY(enumName, token, value, size, desc) case PropertyDataType::enumName: return size;
#include "knx/application/pdt_catalog.inc"
#undef KNX_PDT_CATALOG_ENTRY
            default:
                return 0;  // Variable or unknown
        }
    }
};

/**
 * @brief Property value container
 * 
 * Holds the value of a property with its metadata.
 */
struct PropertyValue {
    PropertyID id;                  ///< Property ID
    PropertyDataType type;          ///< Data type
    uint16_t elementCount;          ///< Number of elements
    std::vector<uint8_t> data;      ///< Raw property data
    
    /**
     * @brief Create from raw data
     */
    static PropertyValue create(PropertyID id, PropertyDataType type, 
                                std::span<const uint8_t> data) {
        PropertyValue value;
        value.id = id;
        value.type = type;
        value.data.assign(data.begin(), data.end());
        value.elementCount = 1;  // Default to single element
        return value;
    }
    
    /**
     * @brief Get as uint8_t
     */
    uint8_t asUInt8() const {
        return data.empty() ? 0 : data[0];
    }
    
    /**
     * @brief Get as uint16_t
     */
    uint16_t asUInt16() const {
        if (data.size() < 2) return 0;
        return (static_cast<uint16_t>(data[0]) << 8) | data[1];
    }
    
    /**
     * @brief Get as uint32_t
     */
    uint32_t asUInt32() const {
        if (data.size() < 4) return 0;
        return (static_cast<uint32_t>(data[0]) << 24) |
               (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8) |
               data[3];
    }
    
    /**
     * @brief Create from uint8_t
     */
    static PropertyValue fromUInt8(PropertyID id, uint8_t value) {
        PropertyValue prop;
        prop.id = id;
        prop.type = PropertyDataType::UnsignedChar;
        prop.data = {value};
        prop.elementCount = 1;
        return prop;
    }
    
    /**
     * @brief Create from uint16_t
     */
    static PropertyValue fromUInt16(PropertyID id, uint16_t value) {
        PropertyValue prop;
        prop.id = id;
        prop.type = PropertyDataType::UnsignedInt;
        prop.data = {
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
        prop.elementCount = 1;
        return prop;
    }
    
    /**
     * @brief Create from uint32_t
     */
    static PropertyValue fromUInt32(PropertyID id, uint32_t value) {
        PropertyValue prop;
        prop.id = id;
        prop.type = PropertyDataType::UnsignedLong;
        prop.data = {
            static_cast<uint8_t>((value >> 24) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>(value & 0xFF)
        };
        prop.elementCount = 1;
        return prop;
    }
};

} // namespace application
} // namespace knx
