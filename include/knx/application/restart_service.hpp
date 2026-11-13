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
#include "knx/util/inplace_function.hpp"
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
 * @brief Erase Code of a Master Reset (03/05/02 §3.7.1.2.3.1 Table 4)
 *
 * 00h and 09h…FFh are reserved; 08h ("erase persistently stored application
 * data") is defined but this stack keeps no such data, so it is answered as
 * unsupported like the reserved values.
 */
enum class EraseCode : uint8_t {
    ConfirmedRestart = 0x01,                 ///< No Resource is reset
    FactoryReset = 0x02,                     ///< Ex-factory state, including the IA
    ResetIndividualAddress = 0x03,           ///< IA back to the medium default
    ResetApplicationProgram = 0x04,          ///< Default application
    ResetParameters = 0x05,                  ///< Application parameters to default
    ResetLinks = 0x06,                       ///< Group Address and Association tables
    FactoryResetWithoutIndividualAddress = 0x07,
};

/**
 * @brief Error Code of A_Restart_Response (03/05/02 §3.7.1.2.3.1 Table 5)
 */
enum class RestartErrorCode : uint8_t {
    NoError = 0x00,
    AccessDenied = 0x01,
    UnsupportedEraseCode = 0x02,
    InvalidChannelNumber = 0x03,
    /// Not a Table 5 value (04h…FFh are reserved). Kept for a device with no
    /// restart handler installed, which has always answered FFh here — ETS
    /// reports it as "ConfirmedRestart failed: 255", which is at least
    /// recognisable in a bug report.
    NotReady = 0xFF,
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
    using RestartCallback = util::InplaceFunction<util::Result<void>(RestartType type), 32>;
    
    /**
     * @brief Authorization check callback
     * @param source Source address requesting restart
     * @return true if restart is authorized
     */
    using AuthorizationCallback = util::InplaceFunction<util::Result<void>(const IndividualAddress& source), 32>;
    
    /**
     * @brief Pre-restart cleanup callback
     * Called before restart to save state, close connections, etc.
     */
    using CleanupCallback = util::InplaceFunction<void(), 32>;

    /**
     * @brief Resets the Resources an Erase Code names.
     *
     * Called from executePendingRestart(), after the A_Restart_Response is on
     * the wire (03/05/02 §3.7.1.2.2: the reset runs only once the response has
     * been sent) and before the restart callback takes the device down. Never
     * called for ConfirmedRestart, which resets nothing.
     *
     * Erase Codes 02h…07h are only offered while this is installed; without it
     * they are answered "Unsupported Erase Code", which is the honest answer
     * for a device that would otherwise report a reset it did not perform.
     */
    using MasterResetCallback = util::InplaceFunction<util::Result<void>(EraseCode eraseCode), 32>;

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
     * @brief Set the Master Reset callback that performs Erase Codes 02h…07h
     */
    void setMasterResetCallback(MasterResetCallback callback) { _masterResetCallback = std::move(callback); }

    /**
     * @brief Handle A_Restart request
     * @param source Source address
     * @param type Restart type (basic or master reset)
    * @return Result<void> indicating success or error
     */
    util::Result<void> handleRequest(const IndividualAddress& source, RestartType type);

    /**
     * @brief Handle an A_Restart Master Reset request
     *
     * Decides the Error Code of the A_Restart_Response and, when it is
     * NoError, leaves the reset pending for executePendingRestart(). Checks
     * run in the order 03/05/02 §3.7.1.2.3.1 implies: the Channel Number is
     * "not defined" for a reserved Erase Code, so an unsupported Erase Code is
     * reported as such whatever the channel says.
     *
     * @param source      requester
     * @param eraseCode   octet 8 of the A_Restart-PDU
     * @param channel     octet 9; this stack has no application channels, so
     *                    anything but 00h is an Invalid Channel Number
     */
    RestartErrorCode handleMasterResetRequest(const IndividualAddress& source,
                                              uint8_t eraseCode,
                                              uint8_t channel);

    /// True when @p eraseCode is one this service will carry out.
    bool isEraseCodeSupported(uint8_t eraseCode) const;

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
     * @brief Get the Erase Code of the pending Master Reset
     * @return ConfirmedRestart for a pending basic restart or when nothing is pending
     */
    EraseCode getPendingEraseCode() const { return _pendingEraseCode; }

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
    MasterResetCallback _masterResetCallback;
    bool _restartPending;
    RestartType _pendingRestartType;
    EraseCode _pendingEraseCode{EraseCode::ConfirmedRestart};
};

} // namespace application
} // namespace knx
