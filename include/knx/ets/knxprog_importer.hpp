// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <map>
#include <span>
#include "knx/util/result.hpp"
#include "ets_config_loader.hpp"

namespace knx::ets {

/**
 * Result of KNXprog import operation
 */
struct KnxprogImportResult {
    bool success = false;
    std::string errorMessage;
    std::string projectName;
    uint32_t bytesProcessed = 0;
    
    // Diagnostics
    std::string projectDescription;
    std::vector<std::string> importedDevices;
    uint32_t totalGroupObjects = 0;
};

/**
 * KNXprog file importer
 * 
 * Handles .knxprog files (ZIP archives) containing ETS project exports.
 * KNXprog is a standard KNX project file format containing:
 * - Manifest XML (project metadata)
 * - Project XML (configuration details)
 * - Device configurations
 * - Group object mappings
 */
class KnxprogImporter {
public:
    /**
     * Constructor
     */
    KnxprogImporter() = default;
    
    /**
     * Destructor
     */
    ~KnxprogImporter() = default;

    /**
     * Import ETS configuration from KNXprog file
     * 
     * @param filePath Path to .knxprog file
     * @param loader   EtsConfigLoader to populate with imported data
     * @return Import result with diagnostics
     */
    KnxprogImportResult importFromFile(
        std::string_view filePath,
        EtsConfigLoader& loader
    );

    /**
     * Import ETS configuration from KNXprog binary buffer
     * 
     * @param buffer KNXprog file data
     * @param size   Size of buffer
     * @param loader EtsConfigLoader to populate
     * @return Import result with diagnostics
     */
    KnxprogImportResult importFromBuffer(
        std::span<const uint8_t> buffer,
        EtsConfigLoader& loader
    );

    /**
     * Parse project manifest XML
     * 
     * @param manifestXml XML manifest content
    * @return Result<void> indicating success or error
     */
    util::Result<void> parseManifest(std::string_view manifestXml);

    /**
     * Get project name from manifest
     * 
     * @return Project name
     */
    const std::string& getProjectName() const { return _projectName; }

    /**
     * Get list of exported devices
     * 
     * @return Vector of device names
     */
    const std::vector<std::string>& getExportedDevices() const {
        return _exportedDevices;
    }

    /**
     * Parse XML element with simple tag matching
     * 
     * @param xml      XML content
     * @param tag      Tag name to find
     * @param value    Output value
     * @return Result<void> indicating success or error
     */
    static util::Result<void> parseXmlTag(
        std::string_view xml,
        std::string_view tag,
        std::string& value
    );

    /**
     * Find XML element content by tag (public for testing)
     * 
     * @param xml XML content
     * @param tag Tag name
     * @return Tag content or empty string
     */
    static std::string findXmlTagContent(
        std::string_view xml,
        std::string_view tag
    );

private:
    std::string _projectName;
    std::string _projectDescription;
    std::vector<std::string> _exportedDevices;
    uint32_t _totalGroupObjects = 0;
    std::map<std::string, std::string> _files;  // filename -> content

    /**
     * Extract ZIP archive from buffer
     * 
     * @param buffer ZIP file data
     * @param size   Buffer size
     * @return true if extraction successful
     */
    util::Result<void> extractZipArchive(
        std::span<const uint8_t> buffer
    );

    /**
     * Parse project.xml from KNXprog
     * 
     * @param projectXml XML content
     * @return true if parsing successful
     */
    util::Result<void> parseProjectXml(std::string_view projectXml);

    /**
     * Extract device configuration from XML
     * 
     * @param deviceXml Device XML element
     * @param loader    EtsConfigLoader to populate
     * @return true if extraction successful
     */
    util::Result<void> extractDeviceConfig(
        std::string_view deviceXml,
        EtsConfigLoader& loader
    );
};

}  // namespace knx::ets
