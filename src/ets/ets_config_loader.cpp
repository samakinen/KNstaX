// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/ets/ets_config_loader.hpp"
#include "knx/ets/ets_format_validator.hpp"
#include "knx/util/bit_ops.hpp"
#include <cstring>
#include <algorithm>
#include <limits>

#ifdef __linux__
#include <fstream>
#endif

namespace bits = knx::util;

namespace knx {
namespace ets {

// Format header structure for version 1
struct EtsConfigV1Header {
    uint16_t formatMagic;           // 0xAEDD
    uint8_t version;                // 1
    uint8_t minorVersion;           // 0
    uint8_t flags;
    uint8_t reserved;
    uint16_t headerChecksum;        // CRC-16 of first 6 bytes
    uint32_t dataChecksum;          // CRC-32 of payload
    uint16_t deviceConfigSize;
    uint16_t addressTableSize;
    uint16_t associationTableSize;
    uint16_t groupObjectTableSize;
} __attribute__((packed));

util::Result<void> EtsConfigLoader::loadFromBuffer(std::span<const uint8_t> buffer,
                                    ValidationLevel level)
{
    if (buffer.data() == nullptr || buffer.size() == 0) {
        return util::ErrorCode::InvalidParameter;
    }
    LoadResult result = loadWithDiagnostics(buffer, level);
    if (!result.success) {
        if (result.formatVersion != 0 && result.formatVersion != 1) {
            return util::ErrorCode::OperationNotSupported;
        }
        return util::ErrorCode::DecodeFailed;
    }
    return util::Result<void>::ok();
}

EtsConfigLoader::LoadResult EtsConfigLoader::loadWithDiagnostics(
    std::span<const uint8_t> buffer, ValidationLevel level)
{
    LoadResult result;
    
    if (buffer.data() == nullptr) {
        result.diagnosticMessage = "Buffer is null";
        return result;
    }

    // Perform validation if requested
    if (level != ValidationLevel::NoValidation) {
        auto validateResult = _validateFormat(buffer, level);
        if (validateResult.isError()) {
            result.diagnosticMessage = "Format validation failed";
            return result;
        }
    }

    // Detect format version
    _detectedFormatVersion = FormatValidator::detectFormatVersion(buffer);
    result.formatVersion = _detectedFormatVersion;

    bool parseSuccess = false;

    // Only version 1 is supported
    if (_detectedFormatVersion != 1) {
        result.diagnosticMessage = "Unsupported format version";
        return result;
    }

    parseSuccess = _parseVersionedFormat(buffer).isOk();
    result.diagnosticMessage = "Version 1 format loaded";
    result.checksumValid = true;

    result.success = parseSuccess;
    result.bytesConsumed = static_cast<uint32_t>(buffer.size());
    _isValid = parseSuccess;
    
    return result;
}

util::Result<void> EtsConfigLoader::_validateFormat(std::span<const uint8_t> buffer, 
                                      ValidationLevel level)
{
    if (level == ValidationLevel::Full) {
        // Full validation including checksums
        auto validationResult = FormatValidator::validateFull(buffer);
        return validationResult.valid ? util::Result<void>::ok() : util::ErrorCode::DecodeFailed;
    } else if (level == ValidationLevel::ChecksumOnly) {
        // Only validate if format has checksums (version 1)
        uint8_t version = FormatValidator::detectFormatVersion(buffer);
        if (version == 1) {
            auto validationResult = FormatValidator::validateFull(buffer);
            return validationResult.checksumValid ? util::Result<void>::ok() : util::ErrorCode::ChecksumError;
        }
        return util::ErrorCode::OperationNotSupported;
    }

    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_parseVersionedFormat(std::span<const uint8_t> buffer)
{
    // Parse version 1 format with header
    if (buffer.size() < 20) {  // sizeof(EtsConfigV1Header)
        return util::ErrorCode::InvalidFrameSize;
    }
    // Read header fields as big-endian (since we write them as big-endian)
    uint16_t deviceConfigSize = bits::makeWord(buffer[12], buffer[13]);
    uint16_t addressTableSize = bits::makeWord(buffer[14], buffer[15]);
    uint16_t associationTableSize = bits::makeWord(buffer[16], buffer[17]);
    uint16_t groupObjectTableSize = bits::makeWord(buffer[18], buffer[19]);

    uint32_t offset = 20;  // Start after header

    // Parse device config
    if (deviceConfigSize > 0 && offset + deviceConfigSize <= buffer.size()) {
        auto deviceResult = _parseDeviceConfig(buffer, offset);
        if (deviceResult.isError()) {
            return deviceResult.error();
        }
        offset += deviceConfigSize;
    }

    // Parse address table
    if (addressTableSize > 0 && offset < buffer.size()) {
        auto addressResult = _parseAddressTable(buffer, offset);
        if (addressResult.isError()) {
            return addressResult.error();
        }
        offset += addressTableSize;
    }

    // Parse association table
    if (associationTableSize > 0 && offset < buffer.size()) {
        auto associationResult = _parseAssociationTable(buffer, offset);
        if (associationResult.isError()) {
            return associationResult.error();
        }
        offset += associationTableSize;
    }

    // Parse group object table
    if (groupObjectTableSize > 0 && offset < buffer.size()) {
        auto groupResult = _parseGroupObjectTable(buffer, offset);
        if (groupResult.isError()) {
            return groupResult.error();
        }
    }

    if (_groupObjects.empty() && _addressTable.empty()) {
        return util::ErrorCode::DecodeFailed;
    }
    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_parseDeviceConfig(std::span<const uint8_t> buffer, uint32_t offset)
{
    if (offset + 10 > buffer.size()) return util::ErrorCode::InvalidFrameSize;

    // Individual Address (big-endian)
    _deviceConfig.individualAddress = IndividualAddress(bits::makeWord(buffer[offset], buffer[offset + 1]));

    // Manufacturer ID (big-endian)
    _deviceConfig.manufacturerId = bits::makeWord(buffer[offset + 2], buffer[offset + 3]);

    // Serial Number (big-endian)
    _deviceConfig.serialNumber = bits::makeDword(buffer[offset + 4], buffer[offset + 5],
                                           buffer[offset + 6], buffer[offset + 7]);

    // Application Version
    _deviceConfig.appVersion = buffer[offset + 8];

    // Configured flag
    _deviceConfig.configured = buffer[offset + 9];

    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_parseAddressTable(std::span<const uint8_t> buffer, uint32_t offset)
{
    if (offset + 2 > buffer.size()) return util::ErrorCode::InvalidFrameSize;

    uint16_t count = bits::makeWord(buffer[offset], buffer[offset + 1]);
    offset += 2;

    if (offset + (count * 2) > buffer.size()) return util::ErrorCode::InvalidFrameSize;

    _addressTable.clear();
    for (uint16_t i = 0; i < count; ++i) {
        AddressTableEntry entry;
        entry.groupAddress = GroupAddress(bits::makeWord(buffer[offset], buffer[offset + 1]));
        entry.asap = static_cast<uint8_t>(i);  // ASAP is implicit index
        _addressTable.push_back(entry);
        offset += 2;
    }

    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_parseAssociationTable(std::span<const uint8_t> buffer, uint32_t offset)
{
    if (offset + 2 > buffer.size()) return util::ErrorCode::InvalidFrameSize;

    uint16_t count = bits::makeWord(buffer[offset], buffer[offset + 1]);
    offset += 2;

    if (offset + (count * 2) > buffer.size()) return util::ErrorCode::InvalidFrameSize;

    _associationTable.clear();
    for (uint16_t i = 0; i < count; ++i) {
        AssociationTableEntry entry;
        entry.asap = static_cast<uint8_t>(i);
        entry.groupObjectIndex = GroupObjectIndex(static_cast<uint16_t>(buffer[offset]));
        entry.priority = buffer[offset + 1];
        _associationTable.push_back(entry);
        offset += 2;
    }

    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_parseGroupObjectTable(std::span<const uint8_t> buffer, uint32_t offset)
{
    if (offset + 2 > buffer.size()) return util::ErrorCode::InvalidFrameSize;

    uint16_t count = bits::makeWord(buffer[offset], buffer[offset + 1]);
    offset += 2;

    if (offset + (count * 5) > buffer.size()) return util::ErrorCode::InvalidFrameSize;  // 5 bytes per entry

    _groupObjects.clear();
    for (uint16_t i = 0; i < count; ++i) {
        GroupObjectConfig go;
        go.index = GroupObjectIndex(static_cast<uint16_t>(i));
        go.dpt = bits::makeWord(buffer[offset], buffer[offset + 1]);
        go.flags = buffer[offset + 2];
        go.initialValue = bits::makeWord(buffer[offset + 3], buffer[offset + 4]);
        _groupObjects.push_back(go);
        offset += 5;  // Advance by 5 bytes per entry
    }

    return util::Result<void>::ok();
}

const EtsConfigLoader::GroupObjectConfig* EtsConfigLoader::findGroupObject(GroupObjectIndex index) const
{
    for (const auto& go : _groupObjects) {
        if (go.index == index) {
            return &go;
        }
    }
    return nullptr;
}

int EtsConfigLoader::findAsapByAddress(const GroupAddress& groupAddr) const
{
    for (const auto& entry : _addressTable) {
        if (entry.groupAddress == groupAddr) {
            return entry.asap;
        }
    }
    return -1;
}

GroupObjectIndex EtsConfigLoader::findGoIndexByAsap(uint8_t asap) const
{
    for (const auto& entry : _associationTable) {
        if (entry.asap == asap) {
            return entry.groupObjectIndex;
        }
    }
    return GroupObjectIndex::invalid();
}

uint32_t EtsConfigLoader::calculateRequiredSize() const
{
    uint32_t size = sizeof(EtsConfigV1Header);
    size += 10;  // Device config
    size += 2u + static_cast<uint32_t>(_addressTable.size() * 2u);  // Address table
    size += 2u + static_cast<uint32_t>(_associationTable.size() * 2u);  // Association table
    size += 2u + static_cast<uint32_t>(_groupObjects.size() * 5u);  // Group object table
    return size;
}

util::Result<void> EtsConfigLoader::saveToBuffer(std::span<uint8_t> buffer, uint32_t& size) const
{
    if (buffer.data() == nullptr) {
        return util::ErrorCode::InvalidParameter;
    }
    if (!_isValid) {
        return util::ErrorCode::NotInitialized;
    }
    uint32_t offset = 0;

    auto writeResult = _writeVersion1Format(buffer, offset);
    if (writeResult.isError()) {
        return writeResult.error();
    }
    size = offset;  // Update with actual bytes written
    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_writeVersion1Format(std::span<uint8_t> buffer, uint32_t& offset) const
{
    // Reserve space for header (will write it at the end after calculating checksums)
    uint32_t maxSize = static_cast<uint32_t>(buffer.size());
    if (maxSize < sizeof(EtsConfigV1Header)) {
        return util::ErrorCode::BufferTooSmall;
    }
    
    uint32_t headerOffset = offset;
    offset += sizeof(EtsConfigV1Header);
    uint32_t payloadStart = offset;
    
    // Write device config
    uint32_t deviceConfigStart = offset;
    auto deviceResult = _writeDeviceConfig(buffer, offset);
    if (deviceResult.isError()) {
        return deviceResult.error();
    }
    const uint32_t deviceConfigSize32 = offset - deviceConfigStart;
    if (deviceConfigSize32 > std::numeric_limits<uint16_t>::max()) {
        return util::ErrorCode::InvalidFrameSize;
    }
    const uint16_t deviceConfigSize = static_cast<uint16_t>(deviceConfigSize32);
    
    // Write address table
    uint32_t addressTableStart = offset;
    auto addressResult = _writeAddressTable(buffer, offset);
    if (addressResult.isError()) {
        return addressResult.error();
    }
    const uint32_t addressTableSize32 = offset - addressTableStart;
    if (addressTableSize32 > std::numeric_limits<uint16_t>::max()) {
        return util::ErrorCode::InvalidFrameSize;
    }
    const uint16_t addressTableSize = static_cast<uint16_t>(addressTableSize32);
    
    // Write association table
    uint32_t associationTableStart = offset;
    auto associationResult = _writeAssociationTable(buffer, offset);
    if (associationResult.isError()) {
        return associationResult.error();
    }
    const uint32_t associationTableSize32 = offset - associationTableStart;
    if (associationTableSize32 > std::numeric_limits<uint16_t>::max()) {
        return util::ErrorCode::InvalidFrameSize;
    }
    const uint16_t associationTableSize = static_cast<uint16_t>(associationTableSize32);
    
    // Write group object table
    uint32_t groupObjectTableStart = offset;
    auto groupResult = _writeGroupObjectTable(buffer, offset);
    if (groupResult.isError()) {
        return groupResult.error();
    }
    const uint32_t groupObjectTableSize32 = offset - groupObjectTableStart;
    if (groupObjectTableSize32 > std::numeric_limits<uint16_t>::max()) {
        return util::ErrorCode::InvalidFrameSize;
    }
    const uint16_t groupObjectTableSize = static_cast<uint16_t>(groupObjectTableSize32);
    
    // Calculate checksums
    uint32_t payloadSize = offset - payloadStart;
    uint32_t payloadCrc = FormatValidator::crc32(buffer.subspan(payloadStart, payloadSize));
    
    // Build header
    EtsConfigV1Header header;
    header.formatMagic = 0xAEDD;
    header.version = 1;
    header.minorVersion = 0;
    header.flags = 0;
    header.reserved = 0;
    header.headerChecksum = 0;  // Will calculate after writing header fields
    header.dataChecksum = payloadCrc;
    header.deviceConfigSize = deviceConfigSize;
    header.addressTableSize = addressTableSize;
    header.associationTableSize = associationTableSize;
    header.groupObjectTableSize = groupObjectTableSize;
    
    // Write header fields (first 6 bytes for checksum calculation)
    buffer[headerOffset + 0] = bits::getHighByte(header.formatMagic);
    buffer[headerOffset + 1] = bits::getLowByte(header.formatMagic);
    buffer[headerOffset + 2] = header.version;
    buffer[headerOffset + 3] = header.minorVersion;
    buffer[headerOffset + 4] = header.flags;
    buffer[headerOffset + 5] = header.reserved;
    
    // Calculate header checksum
    uint16_t headerCrc = FormatValidator::crc16_ccitt(buffer.subspan(headerOffset, 6));
    
    // Write complete header
    buffer[headerOffset + 6] = bits::getHighByte(headerCrc);
    buffer[headerOffset + 7] = bits::getLowByte(headerCrc);
    buffer[headerOffset + 8] = bits::getByte(payloadCrc, 3);
    buffer[headerOffset + 9] = bits::getByte(payloadCrc, 2);
    buffer[headerOffset + 10] = bits::getByte(payloadCrc, 1);
    buffer[headerOffset + 11] = bits::getByte(payloadCrc, 0);
    buffer[headerOffset + 12] = bits::getHighByte(deviceConfigSize);
    buffer[headerOffset + 13] = bits::getLowByte(deviceConfigSize);
    buffer[headerOffset + 14] = bits::getHighByte(addressTableSize);
    buffer[headerOffset + 15] = bits::getLowByte(addressTableSize);
    buffer[headerOffset + 16] = bits::getHighByte(associationTableSize);
    buffer[headerOffset + 17] = bits::getLowByte(associationTableSize);
    buffer[headerOffset + 18] = bits::getHighByte(groupObjectTableSize);
    buffer[headerOffset + 19] = bits::getLowByte(groupObjectTableSize);
    
    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_writeDeviceConfig(std::span<uint8_t> buffer, uint32_t& offset) const
{
    uint32_t maxSize = static_cast<uint32_t>(buffer.size());
    if (offset + 10 > maxSize) {
        return util::ErrorCode::BufferTooSmall;
    }
    
    // Individual Address (big-endian)
    buffer[offset++] = bits::getHighByte(_deviceConfig.individualAddress.raw);
    buffer[offset++] = bits::getLowByte(_deviceConfig.individualAddress.raw);
    
    // Manufacturer ID (big-endian)
    buffer[offset++] = bits::getHighByte(_deviceConfig.manufacturerId);
    buffer[offset++] = bits::getLowByte(_deviceConfig.manufacturerId);
    
    // Serial Number (big-endian)
    buffer[offset++] = bits::getByte(_deviceConfig.serialNumber, 3);
    buffer[offset++] = bits::getByte(_deviceConfig.serialNumber, 2);
    buffer[offset++] = bits::getByte(_deviceConfig.serialNumber, 1);
    buffer[offset++] = bits::getByte(_deviceConfig.serialNumber, 0);
    
    // Application Version
    buffer[offset++] = _deviceConfig.appVersion;
    
    // Configured flag
    buffer[offset++] = _deviceConfig.configured;
    
    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_writeAddressTable(std::span<uint8_t> buffer, uint32_t& offset) const
{
    uint16_t count = static_cast<uint16_t>(_addressTable.size());
    uint32_t maxSize = static_cast<uint32_t>(buffer.size());
    
    if (offset + 2 + (count * 2) > maxSize) {
        return util::ErrorCode::BufferTooSmall;
    }
    
    // Write count
    buffer[offset++] = bits::getHighByte(count);
    buffer[offset++] = bits::getLowByte(count);
    
    // Write entries
    for (const auto& entry : _addressTable) {
        buffer[offset++] = bits::getHighByte(entry.groupAddress.raw);
        buffer[offset++] = bits::getLowByte(entry.groupAddress.raw);
    }
    
    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_writeAssociationTable(std::span<uint8_t> buffer, uint32_t& offset) const
{
    uint16_t count = static_cast<uint16_t>(_associationTable.size());
    uint32_t maxSize = static_cast<uint32_t>(buffer.size());
    
    if (offset + 2 + (count * 2) > maxSize) {
        return util::ErrorCode::BufferTooSmall;
    }
    
    // Write count
    buffer[offset++] = static_cast<uint8_t>((count >> 8) & 0xFFu);
    buffer[offset++] = static_cast<uint8_t>(count & 0xFFu);
    
    // Write entries
    for (const auto& entry : _associationTable) {
        buffer[offset++] = static_cast<uint8_t>(entry.groupObjectIndex.value() & 0xFFu);
        buffer[offset++] = entry.priority;
    }
    
    return util::Result<void>::ok();
}

util::Result<void> EtsConfigLoader::_writeGroupObjectTable(std::span<uint8_t> buffer, uint32_t& offset) const
{
    uint16_t count = static_cast<uint16_t>(_groupObjects.size());
    uint32_t maxSize = static_cast<uint32_t>(buffer.size());
    
    if (offset + 2 + (count * 5) > maxSize) {
        return util::ErrorCode::BufferTooSmall;
    }
    
    // Write count
    buffer[offset++] = bits::getHighByte(count);
    buffer[offset++] = bits::getLowByte(count);
    
    // Write entries (5 bytes each)
    for (const auto& go : _groupObjects) {
        buffer[offset++] = bits::getHighByte(go.dpt);
        buffer[offset++] = bits::getLowByte(go.dpt);
        buffer[offset++] = go.flags;
        buffer[offset++] = bits::getHighByte(go.initialValue);
        buffer[offset++] = bits::getLowByte(go.initialValue);
    }
    
    return util::Result<void>::ok();
}

#ifdef __linux__
util::Result<void> EtsConfigLoader::loadFromFile(const char* filename, ValidationLevel level)
{
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return util::ErrorCode::ResourceUnavailable;
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        return util::ErrorCode::OperationFailed;
    }

    return loadFromBuffer(buffer, level);
}

util::Result<void> EtsConfigLoader::saveToFile(const char* filename) const
{
    if (!_isValid) {
        return util::ErrorCode::NotInitialized;
    }
    
    uint32_t size = calculateRequiredSize();
    std::vector<uint8_t> buffer(size);
    
    auto saveResult = saveToBuffer(buffer, size);
    if (saveResult.isError()) {
        return saveResult.error();
    }
    
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        return util::ErrorCode::ResourceUnavailable;
    }
    
    file.write(reinterpret_cast<const char*>(buffer.data()), size);
    if (!file.good()) {
        return util::ErrorCode::OperationFailed;
    }
    return util::Result<void>::ok();
}
#else
util::Result<void> EtsConfigLoader::loadFromFile(const char* filename, ValidationLevel level)
{
    // Not supported on embedded platforms
    (void)filename;
    (void)level;
    return util::ErrorCode::OperationNotSupported;
}

util::Result<void> EtsConfigLoader::saveToFile(const char* filename) const
{
    // Not supported on embedded platforms
    (void)filename;
    return util::ErrorCode::OperationNotSupported;
}
#endif

} // namespace ets
} // namespace knx
