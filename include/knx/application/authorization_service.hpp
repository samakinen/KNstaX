// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file authorization_service.hpp
 * @brief KNX Authorization Service (A_Authorize_Request/Response)
 * 
 * Implements authorization services per KNX spec 3/5/2 (Management Procedures).
 * Provides key-based authorization for secure device access.
 */

#pragma once

#include "knx/application/apci_services.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <array>
#include <functional>
#include <span>
#include <vector>

namespace knx {
namespace application {

/**
 * @brief Authorization key (4 bytes per KNX spec)
 */
using AuthorizationKey = std::array<uint8_t, 4>;

/**
 * @brief Authorization level
 */
enum class AuthorizationLevel : uint8_t {
    None = 0x00,          ///< No authorization
    Management = 0x01,    ///< Management level (read params)
    Configuration = 0x02, ///< Configuration level (read/write params)
    Maximum = 0x03        ///< Maximum level (unrestricted access)
};

/**
 * @brief Authorization request structure
 */
struct AuthorizationRequest {
    AuthorizationKey key;  ///< Authorization key (4 bytes)
};

/**
 * @brief Authorization response structure
 */
struct AuthorizationResponse {
    AuthorizationLevel level;  ///< Granted authorization level
};

/**
 * @brief Authorization result codes
 */
enum class AuthorizationResult : uint8_t {
    Success = 0,          ///< Authorization successful
    Denied = 1,           ///< Authorization denied (wrong key)
    NotSupported = 2,     ///< Authorization not supported
    InvalidKey = 3        ///< Invalid key format
};

/**
 * @brief Authorization Service Handler
 * 
 * Handles A_Authorize_Request and A_Authorize_Response services.
 * Manages device authorization keys and access levels.
 */
class AuthorizationService {
public:
    /// Default authorization key (all zeros)
    static constexpr AuthorizationKey DEFAULT_KEY = {0x00, 0x00, 0x00, 0x00};
    static constexpr uint32_t NO_TIMEOUT = 0u;
    
    /**
     * @brief Authorization validation callback
     * @param source Source address
     * @param key Authorization key
     * @param level Output authorization level
     * @return true if key is valid
     */
    using ValidationCallback = std::function<util::Result<AuthorizationLevel>(
        const IndividualAddress& source,
        const AuthorizationKey& key)>;
    
    /**
     * @brief Authorization response callback
     * @param dest Destination address
     * @param level Granted authorization level
     */
    using ResponseCallback = std::function<void(const IndividualAddress& dest, 
                                               AuthorizationLevel level)>;

    /**
     * @brief Time source for authorization timestamps.
     *
     * Returns a monotonically increasing tick value. When not set, the service
     * falls back to an internal logical clock so timestamps are never left as
     * placeholder zeros.
     */
    using TimeSource = std::function<uint32_t()>;
    
    /**
     * @brief Initialize authorization service
     */
    AuthorizationService();
    
    /**
     * @brief Set key validation callback
     */
    void setValidationCallback(ValidationCallback callback) { _validationCallback = callback; }
    
    /**
     * @brief Set response callback
     */
    void setResponseCallback(ResponseCallback callback) { _responseCallback = callback; }

    /**
     * @brief Set authorization time source.
     */
    void setTimeSource(TimeSource timeSource) { _timeSource = std::move(timeSource); }

    /**
     * @brief Set authorization timeout in ticks.
     *
     * `NO_TIMEOUT` disables expiry.
     */
    void setAuthorizationTimeout(uint32_t timeoutTicks) { _authorizationTimeout = timeoutTicks; }
    
    /**
     * @brief Set device authorization keys
     * @param managementKey Key for management level
     * @param configurationKey Key for configuration level
     * @param maximumKey Key for maximum level
     */
    void setKeys(const AuthorizationKey& managementKey,
                const AuthorizationKey& configurationKey,
                const AuthorizationKey& maximumKey);
    
    /**
     * @brief Handle A_Authorize_Request
     * @param source Source address
     * @param key Authorization key
    * @return Result<void> indicating success or error
     */
    util::Result<void> handleRequest(const IndividualAddress& source, const AuthorizationKey& key);
    
    /**
     * @brief Get current authorization level for a device
     * @param address Device address
     * @return Current authorization level
     */
    AuthorizationLevel getCurrentLevel(const IndividualAddress& address) const;
    
    /**
     * @brief Clear authorization for a device
     * @param address Device address
     */
    void clearAuthorization(const IndividualAddress& address);
    
    /**
     * @brief Clear all authorizations
     */
    void clearAllAuthorizations();
    
    /**
     * @brief Encode A_Authorize_Request
     * @param key Authorization key (4 bytes)
        * @param out Caller-managed output storage for the 6-byte TPDU
         * @return Result<void> indicating success or error
     */
        static constexpr size_t kEncodedRequestLength = 6;
        static constexpr size_t kEncodedResponseLength = 3;
        static util::Result<void> encodeRequest(const AuthorizationKey& key,
                                        std::span<uint8_t, kEncodedRequestLength> out);
    
    /**
     * @brief Encode A_Authorize_Response
     * @param level Authorization level granted
        * @param out Caller-managed output storage for the 3-byte TPDU
         * @return Result<void> indicating success or error
     */
        static util::Result<void> encodeResponse(AuthorizationLevel level,
                                        std::span<uint8_t, kEncodedResponseLength> out);
    
    /**
     * @brief Decode authorization request
     * @param data Encoded request
     * @param key Output authorization key
        * @return Result<void> indicating success or error
     */
        static util::Result<void> decodeRequest(std::span<const uint8_t> data, AuthorizationKey& key);
    
    /**
     * @brief Decode authorization response
     * @param data Encoded response
     * @param level Output authorization level
        * @return Result<void> indicating success or error
     */
        static util::Result<void> decodeResponse(std::span<const uint8_t> data, AuthorizationLevel& level);
    
    /**
     * @brief Validate authorization key
     * @param key Key to validate
     * @param level Output authorization level if valid
     * @return Validation result
     */
    AuthorizationResult validateKey(const AuthorizationKey& key, AuthorizationLevel& level) const;
    
private:
    struct AuthorizedDevice {
        IndividualAddress address;
        AuthorizationLevel level;
        uint32_t timestamp;
    };
    
    static constexpr size_t MAX_AUTHORIZED_DEVICES = 16;
    
    AuthorizationKey _managementKey;
    AuthorizationKey _configurationKey;
    AuthorizationKey _maximumKey;
    bool _keysConfigured{false};

    AuthorizedDevice _authorizedDevices[MAX_AUTHORIZED_DEVICES];
    size_t _authorizedCount;
    uint32_t _authorizationTimeout;
    mutable uint32_t _logicalClock;
    
    ValidationCallback _validationCallback;
    ResponseCallback _responseCallback;
    TimeSource _timeSource;
    
    void sendResponse(const IndividualAddress& dest, AuthorizationLevel level);
    void storeAuthorization(const IndividualAddress& address, AuthorizationLevel level);
    AuthorizedDevice* findAuthorizedDevice(const IndividualAddress& address);
    const AuthorizedDevice* findAuthorizedDevice(const IndividualAddress& address) const;
    uint32_t currentTime_() const;
    bool isExpired_(const AuthorizedDevice& device, uint32_t now) const;
    void removeExpiredAuthorizations_(uint32_t now);
    void removeAuthorizedDeviceAt_(size_t index);
    size_t oldestAuthorizationIndex_() const;
};

} // namespace application
} // namespace knx
