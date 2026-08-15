// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/ets/ets_config_loader.hpp"
#include "knx/util/result.hpp"
#include <string>

namespace knx {
namespace ets {

/**
 * @brief Utility for ensuring ETS configurations are in the v1 format
 * 
 * Provides helpers to copy or validate configurations that are already
 * emitted in version 1 format with checksums and validation.
 */
class EtsMigrationTool {
public:
    /**
     * @brief Migration result with detailed information
     */
    struct MigrationResult {
        bool success;                   ///< Overall migration success
        std::string errorMessage;       ///< Error description if failed
        uint32_t bytesRead;             ///< Bytes read from source
        uint32_t bytesWritten;          ///< Bytes written to destination
        uint8_t sourceVersion;          ///< Detected source format version
        uint8_t targetVersion;          ///< Target format version (1)
        
        MigrationResult() 
            : success(false), bytesRead(0), bytesWritten(0), 
              sourceVersion(0), targetVersion(1) {}
    };
    
    /**
    * @brief Ensure configuration is in version 1 format (copy if already v1)
    * 
    * @param sourceBuffer Version 1 configuration data
     * @param sourceSize Size of source buffer
     * @param targetBuffer Output buffer for version 1 format (must be pre-allocated)
     * @param targetSize Size of target buffer (updated with actual bytes written)
     * @return MigrationResult with detailed information
     */
    static MigrationResult migrateToV1(std::span<const uint8_t> sourceBuffer,
                                       std::span<uint8_t> targetBuffer);
    
    /**
    * @brief Copy configuration file ensuring it remains version 1 format
    * 
    * @param sourcePath Path to source file (v1 format)
    * @param targetPath Path to output file (v1 format)
     * @return MigrationResult with detailed information
     */
    static MigrationResult migrateFile(const char* sourcePath, const char* targetPath);
    
    /**
     * @brief Validate migration by comparing original and migrated data
     * 
     * Loads both configurations and verifies all data matches (excluding format headers)
     * 
     * @param originalBuffer Original configuration buffer
     * @param originalSize Original buffer size
     * @param migratedBuffer Migrated configuration buffer
     * @param migratedSize Migrated buffer size
     * @return Result<void> indicating success or error
     */
    static util::Result<void> validateMigration(std::span<const uint8_t> originalBuffer,
                                  std::span<const uint8_t> migratedBuffer);
    
private:
    static util::Result<void> compareConfigurations(const EtsConfigLoader& loader1, 
                                     const EtsConfigLoader& loader2);
};

} // namespace ets
} // namespace knx
