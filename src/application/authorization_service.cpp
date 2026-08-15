// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file authorization_service.cpp
 * @brief KNX Authorization Service implementation
 */

#include "knx/application/authorization_service.hpp"
#include "knx/util/log.hpp"
#include "knx/protocol/tpdu_codec.hpp"
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

util::Result<void> AuthorizationService::encodeRequest(const AuthorizationKey& key,
                                                       std::span<uint8_t, kEncodedRequestLength> out) {
    auto result = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::AuthorizeRequest),
        key,
        out
    );
    if (result.isError()) return result.error();
    return util::Result<void>::ok();
}

util::Result<void> AuthorizationService::encodeResponse(AuthorizationLevel level,
                                                        std::span<uint8_t, kEncodedResponseLength> out) {
    auto result = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::AuthorizeResponse),
        {static_cast<uint8_t>(level)},
        out
    );
    if (result.isError()) return result.error();
    return util::Result<void>::ok();
}

util::Result<void> AuthorizationService::decodeRequest(std::span<const uint8_t> data, 
                                         AuthorizationKey& key) {
    if (data.size() < 6) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    const auto hdr = knx::protocol::unpackTpduHeader(data[0], data[1]);
        if (hdr.apci.service() != APCIService::AuthorizeRequest) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    // Extract key (4 bytes starting at offset 2)
    std::copy(data.begin() + 2, data.begin() + 6, key.begin());
    
    return util::Result<void>::ok();
}

util::Result<void> AuthorizationService::decodeResponse(std::span<const uint8_t> data, 
                                          AuthorizationLevel& level) {
    if (data.size() < 3) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    const auto hdr = knx::protocol::unpackTpduHeader(data[0], data[1]);
        if (hdr.apci.service() != APCIService::AuthorizeResponse) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }
    
    // Extract level (1 byte at offset 2)
    level = static_cast<AuthorizationLevel>(data[2]);
    
    // Validate level
    if (level > AuthorizationLevel::Maximum) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    return util::Result<void>::ok();
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
