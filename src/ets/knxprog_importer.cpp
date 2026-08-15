// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/ets/knxprog_importer.hpp"
#include "knx/ets/ets_format_validator.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>
#if KNX_HAS_ZLIB
#include <zlib.h>
#endif

namespace knx::ets {

/**
 * Minimal ZIP reader for .knxprod containers, covering only what ETS product
 * files require: entries located via the central directory, stored (method 0)
 * or DEFLATE (method 8). DEFLATE needs zlib and is compiled in only when
 * KNX_HAS_ZLIB is set; without it, compressed entries fail to extract.
 * Zip64, encryption and multi-part archives are not supported.
 */
namespace zip_support {

// ZIP file format constants
constexpr uint32_t CENTRAL_DIR_SIGNATURE = 0x02014b50;
constexpr uint32_t LOCAL_FILE_SIGNATURE = 0x04034b50;
constexpr uint32_t END_CENTRAL_DIR_SIGNATURE = 0x06054b50;

/**
 * Simple ZIP entry information
 */
struct ZipEntry {
    std::string filename;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint32_t localHeaderOffset;
    uint16_t compressionMethod;
    bool isDirectory;
};

/**
 * Extract entries from ZIP central directory
 */
bool extractZipEntries(
    std::span<const uint8_t> buffer,
    std::vector<ZipEntry>& entries
) {
    if (buffer.size() < 22) return false;

    // Find end of central directory record (search backwards)
    const uint8_t* endCentralDir = nullptr;
    for (int i = static_cast<int>(buffer.size()) - 22; i >= 0; --i) {
        uint32_t sig = (buffer[i] << 0) | (buffer[i + 1] << 8) |
                      (buffer[i + 2] << 16) | (buffer[i + 3] << 24);
        if (sig == END_CENTRAL_DIR_SIGNATURE) {
            endCentralDir = buffer.data() + i;
            break;
        }
    }

    if (!endCentralDir) return false;

    // Parse end of central directory
    uint16_t numEntries = (endCentralDir[10] << 0) | (endCentralDir[11] << 8);
    uint32_t centralDirOffset = (endCentralDir[16] << 0) | (endCentralDir[17] << 8) |
                                (endCentralDir[18] << 16) | (endCentralDir[19] << 24);

    if (centralDirOffset >= buffer.size()) return false;

    // Parse central directory entries
    const uint8_t* pos = buffer.data() + centralDirOffset;
    for (uint16_t i = 0; i < numEntries && pos < endCentralDir; ++i) {
        uint32_t sig = (pos[0] << 0) | (pos[1] << 8) |
                      (pos[2] << 16) | (pos[3] << 24);
        if (sig != CENTRAL_DIR_SIGNATURE) break;

        uint16_t filenameLen = (pos[28] << 0) | (pos[29] << 8);
        uint16_t extraLen = (pos[30] << 0) | (pos[31] << 8);
        uint16_t commentLen = (pos[32] << 0) | (pos[33] << 8);

        ZipEntry entry;
        entry.filename.assign(reinterpret_cast<const char*>(pos + 46), filenameLen);
        entry.compressionMethod = (pos[10] << 0) | (pos[11] << 8);
        entry.compressedSize = (pos[20] << 0) | (pos[21] << 8) |
                              (pos[22] << 16) | (pos[23] << 24);
        entry.uncompressedSize = (pos[24] << 0) | (pos[25] << 8) |
                                 (pos[26] << 16) | (pos[27] << 24);
        entry.localHeaderOffset = (pos[42] << 0) | (pos[43] << 8) |
                                 (pos[44] << 16) | (pos[45] << 24);
        entry.isDirectory = !entry.filename.empty() && entry.filename.back() == '/';

        entries.push_back(entry);

        pos += 46 + filenameLen + extraLen + commentLen;
    }

    return !entries.empty();
}

/**
 * Extract file data from ZIP
 */
bool extractFileData(
    std::span<const uint8_t> buffer,
    const ZipEntry& entry,
    std::vector<uint8_t>& data
) {
    if (entry.localHeaderOffset + 30 > buffer.size()) return false;

    const uint8_t* localHeader = buffer.data() + entry.localHeaderOffset;
    uint32_t sig = (localHeader[0] << 0) | (localHeader[1] << 8) |
                  (localHeader[2] << 16) | (localHeader[3] << 24);
    if (sig != LOCAL_FILE_SIGNATURE) return false;

    uint16_t filenameLen = (localHeader[26] << 0) | (localHeader[27] << 8);
    uint16_t extraLen = (localHeader[28] << 0) | (localHeader[29] << 8);

    uint32_t dataOffset = entry.localHeaderOffset + 30 + filenameLen + extraLen;
    uint32_t dataSize = entry.compressedSize;

    if (dataOffset + dataSize > buffer.size()) return false;

    // Only support uncompressed (method 0) and DEFLATE (method 8)
    if (entry.compressionMethod == 0) {
        data.assign(buffer.data() + dataOffset, buffer.data() + dataOffset + dataSize);
        return true;
    }

    if (entry.compressionMethod == 8) {
#if KNX_HAS_ZLIB
        // DEFLATE (raw, no zlib/gzip wrapper) — used by most real ETS .knxprog files
        data.resize(entry.uncompressedSize);

        z_stream zs{};
        zs.next_in  = const_cast<Bytef*>(buffer.data() + dataOffset);
        zs.avail_in = entry.compressedSize;
        zs.next_out = data.data();
        zs.avail_out = entry.uncompressedSize;

        // inflateInit2 with -MAX_WBITS selects raw DEFLATE (no header/trailer)
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            data.clear();
            return false;
        }

        const int ret = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);

        if (ret != Z_STREAM_END) {
            data.clear();
            return false;
        }
        return true;
#else
        return false;
#endif
    }

    return false;  // Unsupported compression method
}

}  // namespace zip_support

// ============================================================================
// KnxprogImporter Implementation
// ============================================================================

KnxprogImportResult KnxprogImporter::importFromFile(
    std::string_view filePath,
    EtsConfigLoader& loader
) {
    KnxprogImportResult result;

    // Open file in binary mode
    std::ifstream file(std::string(filePath), std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        result.errorMessage = "Failed to open file: ";
        result.errorMessage += filePath;
        return result;
    }

    // Get file size
    std::streamsize fileSize = file.tellg();
    if (fileSize <= 0 || fileSize > 100 * 1024 * 1024) {  // Limit to 100MB
        result.errorMessage = "Invalid file size";
        return result;
    }
    file.seekg(0, std::ios::beg);

    // Read entire file into memory
    std::vector<uint8_t> buffer(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
        result.errorMessage = "Failed to read file";
        return result;
    }
    file.close();

    // Delegate to span-based import
    return importFromBuffer(std::span<const uint8_t>(buffer), loader);
}

KnxprogImportResult KnxprogImporter::importFromBuffer(
    std::span<const uint8_t> buffer,
    EtsConfigLoader& loader
) {
    (void)loader;
    KnxprogImportResult result;

    if (buffer.empty()) {
        result.errorMessage = "Invalid buffer";
        return result;
    }

    // Extract ZIP archive
    auto extractResult = extractZipArchive(buffer);
    if (extractResult.isError()) {
        result.errorMessage = "Failed to extract ZIP archive";
        return result;
    }

    // Look for manifest.xml
    auto manifestIt = _files.find("manifest.xml");
    if (manifestIt == _files.end()) {
            result.errorMessage = "No manifest.xml found in KNXprog file";
            return result;
    }

    // Parse manifest
        auto manifestResult = parseManifest(manifestIt->second);
        if (manifestResult.isError()) {
        result.errorMessage = "Failed to parse manifest.xml";
        return result;
    }

    // Look for project.xml (optional for basic imports)
    auto projectIt = _files.find("project.xml");
    if (projectIt != _files.end()) {
        // Parse project details if available
        auto projectResult = parseProjectXml(projectIt->second);
        if (projectResult.isError()) {
            result.errorMessage = "Failed to parse project.xml";
            return result;
        }
    }

    result.success = true;
    result.projectName = _projectName;
    result.projectDescription = _projectDescription;
    result.importedDevices = _exportedDevices;
    result.bytesProcessed = static_cast<uint32_t>(buffer.size());

    return result;
}

util::Result<void> KnxprogImporter::extractZipArchive(
    std::span<const uint8_t> buffer
) {
    if (buffer.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    std::vector<zip_support::ZipEntry> entries;

    if (!zip_support::extractZipEntries(buffer, entries)) {
        return util::ErrorCode::DecodeFailed;
    }

    for (const auto& entry : entries) {
        if (entry.isDirectory) continue;

        std::vector<uint8_t> data;
        if (zip_support::extractFileData(buffer, entry, data)) {
            _files[entry.filename].assign(
                reinterpret_cast<const char*>(data.data()),
                data.size()
            );
        }
    }

    if (_files.empty()) {
        return util::ErrorCode::DecodeFailed;
    }

    return util::Result<void>::ok();
}

util::Result<void> KnxprogImporter::parseManifest(std::string_view manifestXml) {
    if (manifestXml.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    // Extract basic project information from manifest
    // Format: <?xml version="1.0"?>
    //         <ManifestHeader ProjectName="MyProject" .../>

    std::string projectName;
    auto tagResult = parseXmlTag(manifestXml, "ManifestHeader", projectName);
    if (tagResult.isError()) {
        return tagResult.error();
    }

    // Extract ProjectName attribute
    std::string mstr(manifestXml);
    size_t namePos = mstr.find("ProjectName=\"");
    if (namePos != std::string::npos) {
        namePos += 13;  // length of 'ProjectName="'
        size_t endPos = mstr.find('"', namePos);
        if (endPos != std::string::npos) {
            _projectName = mstr.substr(namePos, endPos - namePos);
        }
    }

    return util::Result<void>::ok();
}

util::Result<void> KnxprogImporter::parseProjectXml(std::string_view projectXml) {
    if (projectXml.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    // Extract project description and device information
    _projectDescription = findXmlTagContent(projectXml, "Description");
    
    // Count group objects (basic extraction)
    size_t goCount = 0;
    size_t pos = 0;
    while ((pos = projectXml.find("<GroupObject", pos)) != std::string::npos) {
        goCount++;
        pos += 12;
    }
    _totalGroupObjects = static_cast<uint32_t>(goCount);

    // Extract device information
    pos = 0;
    while ((pos = projectXml.find("<Device", pos)) != std::string::npos) {
        size_t nameStart = projectXml.find("Name=\"", pos);
        if (nameStart != std::string::npos) {
            nameStart += 6;
            size_t nameEnd = projectXml.find('"', nameStart);
            if (nameEnd != std::string::npos) {
                auto sv = projectXml.substr(nameStart, nameEnd - nameStart);
                std::string deviceName(sv);
                _exportedDevices.push_back(deviceName);
            }
        }
        pos += 7;
    }

    return util::Result<void>::ok();
}

util::Result<void> KnxprogImporter::extractDeviceConfig(
    std::string_view deviceXml,
    EtsConfigLoader& loader
) {
    // INCOMPLETE: this is currently a validation-only pass. It confirms the
    // device section carries an <IndividualAddress> element and reports
    // DecodeFailed if not, but does not parse the address or any other device
    // data, and writes nothing into `loader`. Callers cannot rely on the loader
    // being populated by a successful return.
    //
    // Completing it means parsing the "area.line.device" address plus the
    // ComObject, group-address and parameter tables, and committing them to
    // `loader`.
    (void)loader;
    if (deviceXml.empty()) {
        return util::ErrorCode::InvalidParameter;
    }

    std::string addrStr = findXmlTagContent(deviceXml, "IndividualAddress");
    if (addrStr.empty()) {
        return util::ErrorCode::DecodeFailed;
    }

    return util::Result<void>::ok();
}

util::Result<void> KnxprogImporter::parseXmlTag(
    std::string_view xml,
    std::string_view tag,
    std::string& value
) {
    value.clear();
    if (xml.empty() || tag.empty()) {
        return util::ErrorCode::InvalidParameter;
    }
    std::string openTag = std::string("<") + std::string(tag);
    size_t pos = xml.find(openTag);
    if (pos == std::string::npos) {
        return util::ErrorCode::DecodeFailed;
    }
    return util::Result<void>::ok();
}

std::string KnxprogImporter::findXmlTagContent(
    std::string_view xml,
    std::string_view tag
) {
    std::string openTag = std::string("<") + std::string(tag) + ">";
    std::string closeTag = std::string("</") + std::string(tag) + ">";

    size_t startPos = xml.find(openTag);
    if (startPos == std::string::npos) {
        return "";
    }

    startPos += openTag.length();
    size_t endPos = xml.find(closeTag, startPos);
    if (endPos == std::string::npos) {
        return "";
    }

    return std::string(xml.substr(startPos, endPos - startPos));
}

}  // namespace knx::ets
