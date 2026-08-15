// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file restart_service.hpp
 * @brief KNX Restart Service (A_Restart)
 * 
 * Implements restart service per KNX spec 3/5/1 (Application Layer).
 * Provides device restart functionality with different modes.
 */

#pragma once

#include "knx/application/apci_services.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace knx {
namespace application {

/**
 * @brief Restart request types
 */
enum class RestartType : uint8_t {
    Basic = 0x00,           ///< Basic restart (reset to normal operation)
    MasterReset = 0x01      ///< Master reset (factory defaults)
};

/**
 * @brief Restart result codes
 */
enum class RestartResult : uint8_t {
    Success = 0,           ///< Restart initiated successfully
    Denied = 1,            ///< Restart denied (not authorized)
    NotSupported = 2,      ///< Restart type not supported
    InvalidRequest = 3     ///< Invalid restart request
};

/**
 * @brief Restart Service Handler
 * 
 * Handles A_Restart requests with optional authorization checking
 * and cleanup callback before restart.
 */
class RestartService {
public:
    /**
     * @brief Restart execution callback
     * @param type Type of restart requested
     * @return true if restart can be performed
     */
    using RestartCallback = std::function<util::Result<void>(RestartType type)>;
    
    /**
     * @brief Authorization check callback
     * @param source Source address requesting restart
     * @return true if restart is authorized
     */
    using AuthorizationCallback = std::function<util::Result<void>(const IndividualAddress& source)>;
    
    /**
     * @brief Pre-restart cleanup callback
     * Called before restart to save state, close connections, etc.
     */
    using CleanupCallback = std::function<void()>;
    
    /**
     * @brief Initialize restart service
     */
    RestartService();
    
    /**
     * @brief Set restart execution callback
     */
    void setRestartCallback(RestartCallback callback) { _restartCallback = callback; }
    
    /**
     * @brief Set authorization check callback
     */
    void setAuthorizationCallback(AuthorizationCallback callback) { _authorizationCallback = callback; }
    
    /**
     * @brief Set cleanup callback
     */
    void setCleanupCallback(CleanupCallback callback) { _cleanupCallback = callback; }
    
    /**
     * @brief Handle A_Restart request
     * @param source Source address
     * @param type Restart type (basic or master reset)
    * @return Result<void> indicating success or error
     */
    util::Result<void> handleRequest(const IndividualAddress& source, RestartType type);
    
    /**
     * @brief Encode A_Restart request
     * @param type Restart type
        * @param out Caller-managed output storage for the 3-byte TPDU
         * @return Result<void> indicating success or error
     */
        static constexpr size_t kEncodedRequestLength = 3;
        static util::Result<void> encodeRequest(RestartType type, std::span<uint8_t, kEncodedRequestLength> out);
    
    /**
     * @brief Decode A_Restart request
     * @param data Encoded request
     * @param type Output restart type
        * @return Result<void> indicating success or error
     */
        static util::Result<void> decodeRequest(std::span<const uint8_t> data, RestartType& type);
    
    /**
     * @brief Check if restart is pending
     * @return true if restart has been requested and is pending
     */
    bool isRestartPending() const { return _restartPending; }
    
    /**
     * @brief Get pending restart type
     * @return Restart type if pending, otherwise Basic
     */
    RestartType getPendingRestartType() const { return _pendingRestartType; }
    
    /**
     * @brief Execute pending restart
     * 
     * This should be called from main loop when restart is pending.
     * It performs cleanup and executes the restart callback.
     */
    void executePendingRestart();
    
    /**
     * @brief Cancel pending restart
     */
    void cancelRestart() { _restartPending = false; }
    
private:
    RestartCallback _restartCallback;
    AuthorizationCallback _authorizationCallback;
    CleanupCallback _cleanupCallback;
    bool _restartPending;
    RestartType _pendingRestartType;
};

} // namespace application
} // namespace knx
