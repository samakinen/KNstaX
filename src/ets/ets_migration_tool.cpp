// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/ets/ets_migration_tool.hpp"
#include "knx/ets/ets_format_validator.hpp"
#include <cstring>
#include <span>

#ifdef __linux__
#include <fstream>
#include <vector>
#endif

namespace knx {
namespace ets {

EtsMigrationTool::MigrationResult EtsMigrationTool::migrateToV1(
    std::span<const uint8_t> sourceBuffer,
    std::span<uint8_t> targetBuffer)
{
    MigrationResult result;

    if (sourceBuffer.empty()) {
        result.errorMessage = "Invalid source buffer";
        return result;
    }

    // Load source configuration
    EtsConfigLoader loader;
    auto loadResult = loader.loadWithDiagnostics(sourceBuffer);

    if (!loadResult.success) {
        result.errorMessage = "Failed to load source: " + loadResult.diagnosticMessage;
        return result;
    }

    result.bytesRead = loadResult.bytesConsumed;
    result.sourceVersion = loadResult.formatVersion;

    // Only v1 is supported; reject anything else
    if (result.sourceVersion != 1) {
        result.errorMessage = "Unsupported source format version";
        return result;
    }

    if (targetBuffer.size() < sourceBuffer.size()) {
        result.errorMessage = "Target buffer too small";
        return result;
    }

    std::memcpy(targetBuffer.data(), sourceBuffer.data(), sourceBuffer.size());
    result.bytesWritten = static_cast<uint32_t>(sourceBuffer.size());
    result.success = true;
    result.errorMessage = "Already version 1 format";

    return result;
}

#ifdef __linux__
EtsMigrationTool::MigrationResult EtsMigrationTool::migrateFile(
    const char* sourcePath, const char* targetPath)
{
    MigrationResult result;
    
    // Read source file
    std::ifstream sourceFile(sourcePath, std::ios::binary | std::ios::ate);
    if (!sourceFile.is_open()) {
        result.errorMessage = "Failed to open source file";
        return result;
    }
    
    std::streamsize sourceSize = sourceFile.tellg();
    sourceFile.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> sourceBuffer(sourceSize);
    if (!sourceFile.read(reinterpret_cast<char*>(sourceBuffer.data()), sourceSize)) {
        result.errorMessage = "Failed to read source file";
        return result;
    }
    sourceFile.close();
    
    // Load and verify format
    EtsConfigLoader loader;
    if (loader.loadFromBuffer(sourceBuffer).isError()) {
        result.errorMessage = "Failed to parse source configuration";
        return result;
    }
    
    result.sourceVersion = loader.formatVersion();
    result.bytesRead = static_cast<uint32_t>(sourceSize);
    
    if (result.sourceVersion != 1) {
        result.errorMessage = "Unsupported source format version";
        return result;
    }
    
    // Save to target file (v1)
    if (loader.saveToFile(targetPath).isError()) {
        result.errorMessage = "Failed to write target file";
        return result;
    }
    
    // Read back to get size
    std::ifstream targetFile(targetPath, std::ios::binary | std::ios::ate);
    if (targetFile.is_open()) {
        result.bytesWritten = static_cast<uint32_t>(targetFile.tellg());
        targetFile.close();
    }
    
    result.success = true;
    result.errorMessage = "File migration successful";
    
    return result;
}
#else
EtsMigrationTool::MigrationResult EtsMigrationTool::migrateFile(
    const char* sourcePath, const char* targetPath)
{
    MigrationResult result;
    (void)sourcePath;
    (void)targetPath;
    result.errorMessage = "File operations not supported on this platform";
    return result;
}
#endif

util::Result<void> EtsMigrationTool::validateMigration(
    std::span<const uint8_t> originalBuffer,
    std::span<const uint8_t> migratedBuffer)
{
    if (originalBuffer.empty() || migratedBuffer.empty()) {
        return util::ErrorCode::InvalidParameter;
    }

    // Load both configurations
    EtsConfigLoader originalLoader, migratedLoader;

    if (originalLoader.loadFromBuffer(originalBuffer).isError()) {
        return util::ErrorCode::DecodeFailed;
    }

    if (migratedLoader.loadFromBuffer(migratedBuffer).isError()) {
        return util::ErrorCode::DecodeFailed;
    }

    return compareConfigurations(originalLoader, migratedLoader);
}

util::Result<void> EtsMigrationTool::compareConfigurations(
    const EtsConfigLoader& loader1, const EtsConfigLoader& loader2)
{
    // Compare device config
    const auto& dev1 = loader1.deviceConfig();
    const auto& dev2 = loader2.deviceConfig();
    
    if (dev1.individualAddress != dev2.individualAddress ||
        dev1.manufacturerId != dev2.manufacturerId ||
        dev1.serialNumber != dev2.serialNumber ||
        dev1.appVersion != dev2.appVersion ||
        dev1.configured != dev2.configured) {
        return util::ErrorCode::OperationFailed;
    }
    
    // Compare address tables
    const auto& addr1 = loader1.addressTable();
    const auto& addr2 = loader2.addressTable();
    
    if (addr1.size() != addr2.size()) {
        return util::ErrorCode::OperationFailed;
    }
    
    for (size_t i = 0; i < addr1.size(); ++i) {
        if (addr1[i].groupAddress != addr2[i].groupAddress ||
            addr1[i].asap != addr2[i].asap) {
            return util::ErrorCode::OperationFailed;
        }
    }
    
    // Compare association tables
    const auto& assoc1 = loader1.associationTable();
    const auto& assoc2 = loader2.associationTable();
    
    if (assoc1.size() != assoc2.size()) {
        return util::ErrorCode::OperationFailed;
    }
    
    for (size_t i = 0; i < assoc1.size(); ++i) {
        if (assoc1[i].asap != assoc2[i].asap ||
            assoc1[i].groupObjectIndex != assoc2[i].groupObjectIndex ||
            assoc1[i].priority != assoc2[i].priority) {
            return util::ErrorCode::OperationFailed;
        }
    }
    
    // Compare group object tables
    const auto& go1 = loader1.groupObjects();
    const auto& go2 = loader2.groupObjects();
    
    if (go1.size() != go2.size()) {
        return util::ErrorCode::OperationFailed;
    }
    
    for (size_t i = 0; i < go1.size(); ++i) {
        if (go1[i].index != go2[i].index ||
            go1[i].dpt != go2[i].dpt ||
            go1[i].flags != go2[i].flags ||
            go1[i].initialValue != go2[i].initialValue) {
            return util::ErrorCode::OperationFailed;
        }
    }

    return util::Result<void>::ok();
}

} // namespace ets
} // namespace knx
