// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file authorization_service.cpp
 * @brief KNX Authorization Service implementation
 */

#include "knx/application/authorization_service.hpp"
#include "knx/util/log.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/util/result.hpp"
#include <algorithm>
#include <cstring>
#include <span>

namespace knx {
namespace application {

static const char* TAG = "KNX.App.Auth";

AuthorizationService::AuthorizationService()
    : _managementKey(DEFAULT_KEY),
      _configurationKey(DEFAULT_KEY),
      _maximumKey(DEFAULT_KEY),
      _authorizedCount(0),
      _authorizationTimeout(NO_TIMEOUT),
      _logicalClock(0) {
    std::fill_n(_authorizedDevices, MAX_AUTHORIZED_DEVICES, AuthorizedDevice{});
}

void AuthorizationService::setKeys(const AuthorizationKey& managementKey,
                                   const AuthorizationKey& configurationKey,
                                   const AuthorizationKey& maximumKey) {
    _managementKey = managementKey;
    _configurationKey = configurationKey;
    _maximumKey = maximumKey;
    _keysConfigured = true;
    KNX_LOGD(TAG, "Authorization keys configured");
}

util::Result<void> AuthorizationService::handleRequest(const IndividualAddress& source,
                                                     const AuthorizationKey& key) {
    AuthorizationLevel level = AuthorizationLevel::None;

    if (_validationCallback) {
        auto validationRes = _validationCallback(source, key);
        if (validationRes.isError()) {
            KNX_LOGW(TAG,
                     "Authorization denied for %d.%d.%d by callback",
                     source.area(),
                     source.line(),
                     source.device());
            sendResponse(source, AuthorizationLevel::None);
            return validationRes.error();
        }
        level = validationRes.value();
    } else {
        auto result = validateKey(key, level);
        if (result != AuthorizationResult::Success) {
            KNX_LOGW(TAG,
                     "Authorization denied for %d.%d.%d: invalid key",
                     source.area(),
                     source.line(),
                     source.device());
            sendResponse(source, AuthorizationLevel::None);
            return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
        }
    }

    storeAuthorization(source, level);
    sendResponse(source, level);

    KNX_LOGI(TAG,
             "Authorized %d.%d.%d at level %d",
             source.area(),
             source.line(),
             source.device(),
             static_cast<int>(level));
    return util::Result<void>::ok();
}

AuthorizationLevel AuthorizationService::getCurrentLevel(const IndividualAddress& address) const {
    const auto* device = findAuthorizedDevice(address);
    if (device == nullptr) {
        return AuthorizationLevel::None;
    }

    const uint32_t now = currentTime_();
    if (isExpired_(*device, now)) {
        return AuthorizationLevel::None;
    }

    return device->level;
}

uint8_t AuthorizationService::currentAccessLevel(const IndividualAddress& address) const {
    if (!_keysConfigured) {
        return toWireLevel(AuthorizationLevel::Maximum);
    }

    const auto* device = findAuthorizedDevice(address);
    if (device != nullptr && !isExpired_(*device, currentTime_())) {
        return toWireLevel(device->level);
    }

    // Not authorised: the most privileged level still on the default key.
    static constexpr AuthorizationKey kUnprotected = {0xFF, 0xFF, 0xFF, 0xFF};
    if (_maximumKey == kUnprotected) {
        return toWireLevel(AuthorizationLevel::Maximum);
    }
    if (_configurationKey == kUnprotected) {
        return toWireLevel(AuthorizationLevel::Configuration);
    }
    if (_managementKey == kUnprotected) {
        return toWireLevel(AuthorizationLevel::Management);
    }
    return toWireLevel(AuthorizationLevel::None);
}

void AuthorizationService::clearAuthorization(const IndividualAddress& address) {
    auto* device = findAuthorizedDevice(address);
    if (device == nullptr) {
        return;
    }

    removeAuthorizedDeviceAt_(static_cast<size_t>(device - _authorizedDevices));
    KNX_LOGD(TAG,
             "Cleared authorization for %d.%d.%d",
             address.area(),
             address.line(),
             address.device());
}

void AuthorizationService::clearAllAuthorizations() {
    _authorizedCount = 0;
    std::fill_n(_authorizedDevices, MAX_AUTHORIZED_DEVICES, AuthorizedDevice{});
    KNX_LOGD(TAG, "Cleared all authorizations");
}

AuthorizationResult AuthorizationService::validateKey(const AuthorizationKey& key, 
                                                      AuthorizationLevel& level) const {
    // Unsecured device: no keys configured means free access per the KNX
    // default (ETS authorizes with the default key FF FF FF FF and expects
    // access level 0). Configuring keys enables real key checks below.
    if (!_keysConfigured) {
        level = AuthorizationLevel::Maximum;
        return AuthorizationResult::Success;
    }

    // Check maximum level first (highest privilege)
    if (std::equal(key.begin(), key.end(), _maximumKey.begin())) {
        level = AuthorizationLevel::Maximum;
        return AuthorizationResult::Success;
    }
    
    // Check configuration level
    if (std::equal(key.begin(), key.end(), _configurationKey.begin())) {
        level = AuthorizationLevel::Configuration;
        return AuthorizationResult::Success;
    }
    
    // Check management level
    if (std::equal(key.begin(), key.end(), _managementKey.begin())) {
        level = AuthorizationLevel::Management;
        return AuthorizationResult::Success;
    }
    
    // No matching key
    level = AuthorizationLevel::None;
    return AuthorizationResult::Denied;
}

void AuthorizationService::sendResponse(const IndividualAddress& dest, 
                                        AuthorizationLevel level) {
    if (_responseCallback) {
        _responseCallback(dest, level);
    }
}

void AuthorizationService::storeAuthorization(const IndividualAddress& address,
                                               AuthorizationLevel level) {
    const uint32_t now = currentTime_();
    removeExpiredAuthorizations_(now);

    auto* device = findAuthorizedDevice(address);
    if (device != nullptr) {
        device->level = level;
        device->timestamp = now;
        return;
    }

    if (_authorizedCount < MAX_AUTHORIZED_DEVICES) {
        _authorizedDevices[_authorizedCount] = AuthorizedDevice{address, level, now};
        ++_authorizedCount;
        return;
    }

    const size_t evictionIndex = oldestAuthorizationIndex_();
    const auto& evicted = _authorizedDevices[evictionIndex];
    KNX_LOGW(TAG,
             "Authorization table full, evicting %d.%d.%d",
             evicted.address.area(),
             evicted.address.line(),
             evicted.address.device());
    _authorizedDevices[evictionIndex] = AuthorizedDevice{address, level, now};
}

AuthorizationService::AuthorizedDevice* AuthorizationService::findAuthorizedDevice(
    const IndividualAddress& address) {
    for (size_t i = 0; i < _authorizedCount; ++i) {
        auto& dev = _authorizedDevices[i];
        if (dev.address.area() == address.area() &&
            dev.address.line() == address.line() &&
            dev.address.device() == address.device()) {
            return &dev;
        }
    }
    return nullptr;
}

const AuthorizationService::AuthorizedDevice* AuthorizationService::findAuthorizedDevice(
    const IndividualAddress& address) const {
    for (size_t i = 0; i < _authorizedCount; ++i) {
        const auto& dev = _authorizedDevices[i];
        if (dev.address.area() == address.area() &&
            dev.address.line() == address.line() &&
            dev.address.device() == address.device()) {
            return &dev;
        }
    }
    return nullptr;
}

uint32_t AuthorizationService::currentTime_() const {
    if (_timeSource) {
        return _timeSource();
    }
    return ++_logicalClock;
}

bool AuthorizationService::isExpired_(const AuthorizedDevice& device, uint32_t now) const {
    if (_authorizationTimeout == NO_TIMEOUT) {
        return false;
    }
    return static_cast<uint32_t>(now - device.timestamp) >= _authorizationTimeout;
}

void AuthorizationService::removeExpiredAuthorizations_(uint32_t now) {
    size_t index = 0;
    while (index < _authorizedCount) {
        if (!isExpired_(_authorizedDevices[index], now)) {
            ++index;
            continue;
        }
        removeAuthorizedDeviceAt_(index);
    }
}

void AuthorizationService::removeAuthorizedDeviceAt_(size_t index) {
    for (size_t i = index; i + 1 < _authorizedCount; ++i) {
        _authorizedDevices[i] = _authorizedDevices[i + 1];
    }
    if (_authorizedCount > 0) {
        --_authorizedCount;
        _authorizedDevices[_authorizedCount] = AuthorizedDevice{};
    }
}

size_t AuthorizationService::oldestAuthorizationIndex_() const {
    size_t oldestIndex = 0;
    for (size_t i = 1; i < _authorizedCount; ++i) {
        if (_authorizedDevices[i].timestamp < _authorizedDevices[oldestIndex].timestamp) {
            oldestIndex = i;
        }
    }
    return oldestIndex;
}

} // namespace application
} // namespace knx
