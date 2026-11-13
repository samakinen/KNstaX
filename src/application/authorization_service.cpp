// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file authorization_service.cpp
 * @brief A_Authorize and A_Key_Write (03/03/07 §3.5.7, §3.5.8)
 */

#include "knx/application/authorization_service.hpp"
#include "knx/util/log.hpp"

#include <algorithm>

namespace knx {
namespace application {

static const char* TAG = "KNX.App.Auth";

AuthorizationService::AuthorizationService()
{
    _keys.fill(kNoKey);
    std::fill_n(_authorizedDevices, MAX_AUTHORIZED_DEVICES, AuthorizedDevice{});
}

util::Result<void> AuthorizationService::setKey(AccessLevel level, const AuthorizationKey& key)
{
    if (level >= kKeyedLevelCount) {
        return util::ErrorCode::OutOfRange;
    }
    _keys[level] = key;
    return util::Result<void>::ok();
}

bool AuthorizationService::hasKeys() const
{
    return std::any_of(_keys.begin(), _keys.end(), [](const AuthorizationKey& key) { return key != kNoKey; });
}

void AuthorizationService::resetKeysFrom(AccessLevel fromLevel)
{
    bool changed = false;
    for (size_t level = fromLevel; level < _keys.size(); ++level) {
        if (_keys[level] != kNoKey) {
            _keys[level] = kNoKey;
            changed = true;
        }
    }
    if (changed) {
        KNX_LOGI(TAG, "Authorisation keys of level %u and higher reset", static_cast<unsigned>(fromLevel));
        if (_keyTableChangedCallback) {
            _keyTableChangedCallback(_keys);
        }
    }
}

void AuthorizationService::serializeKeyTable(const KeyTable& keys,
                                             std::span<uint8_t, kSerializedKeyTableSize> out)
{
    for (size_t level = 0; level < keys.size(); ++level) {
        std::copy(keys[level].begin(), keys[level].end(), out.begin() + level * 4u);
    }
}

AuthorizationService::KeyTable AuthorizationService::deserializeKeyTable(
    std::span<const uint8_t, kSerializedKeyTableSize> in)
{
    KeyTable keys{};
    for (size_t level = 0; level < keys.size(); ++level) {
        std::copy_n(in.begin() + level * 4u, 4u, keys[level].begin());
    }
    return keys;
}

AccessLevel AuthorizationService::freeAccessLevel() const
{
    for (size_t level = 0; level < _keys.size(); ++level) {
        if (_keys[level] == kNoKey) {
            return static_cast<AccessLevel>(level);
        }
    }
    return kMinimumAccessLevel;
}

AccessLevel AuthorizationService::levelForKey(const AuthorizationKey& key) const
{
    if (!hasKeys()) {
        return kMaximumAccessLevel;
    }
    // FFFFFFFFh is the key of every level that has none, so the first match
    // for it is the free access level, exactly as for a partner that does not
    // authorise; 03/05/02 DM_Authorize relies on that to find the free level.
    for (size_t level = 0; level < _keys.size(); ++level) {
        if (_keys[level] == key) {
            return static_cast<AccessLevel>(level);
        }
    }
    return kMinimumAccessLevel;
}

void AuthorizationService::handleRequest(const IndividualAddress& source, const AuthorizationKey& key)
{
    const AccessLevel level = levelForKey(key);
    storeAuthorization(source, level);
    KNX_LOGI(TAG, "Authorised %d.%d.%d at level %u", source.area(), source.line(), source.device(),
             static_cast<unsigned>(level));
    if (_responseCallback) {
        _responseCallback(source, level);
    }
}

AccessLevel AuthorizationService::handleKeyWrite(const IndividualAddress& source,
                                                 AccessLevel level,
                                                 const AuthorizationKey& key)
{
    if (level >= kKeyedLevelCount) {
        KNX_LOGW(TAG, "A_Key_Write for level %u from %d.%d.%d refused: no key at that level",
                 static_cast<unsigned>(level), source.area(), source.line(), source.device());
        return kKeyWriteRefused;
    }
    const AccessLevel current = currentAccessLevel(source);
    if (current > level) {
        KNX_LOGW(TAG, "A_Key_Write for level %u from %d.%d.%d refused: it holds level %u",
                 static_cast<unsigned>(level), source.area(), source.line(), source.device(),
                 static_cast<unsigned>(current));
        return kKeyWriteRefused;
    }

    if (_keys[level] != key) {
        _keys[level] = key;
        if (_keyTableChangedCallback) {
            _keyTableChangedCallback(_keys);
        }
    }
    KNX_LOGI(TAG, "Key of level %u %s by %d.%d.%d", static_cast<unsigned>(level),
             key == kNoKey ? "removed" : "written", source.area(), source.line(), source.device());
    return level;
}

AccessLevel AuthorizationService::currentAccessLevel(const IndividualAddress& address) const
{
    if (!hasKeys()) {
        return kMaximumAccessLevel;
    }
    const auto* device = findAuthorizedDevice(address);
    if (device != nullptr && !isExpired_(*device, currentTime_())) {
        return device->level;
    }
    return freeAccessLevel();
}

void AuthorizationService::clearAuthorization(const IndividualAddress& address)
{
    auto* device = findAuthorizedDevice(address);
    if (device == nullptr) {
        return;
    }
    removeAuthorizedDeviceAt_(static_cast<size_t>(device - _authorizedDevices));
}

void AuthorizationService::clearAllAuthorizations()
{
    _authorizedCount = 0;
    std::fill_n(_authorizedDevices, MAX_AUTHORIZED_DEVICES, AuthorizedDevice{});
}

void AuthorizationService::storeAuthorization(const IndividualAddress& address, AccessLevel level)
{
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
    KNX_LOGW(TAG, "Authorization table full, evicting %d.%d.%d", evicted.address.area(),
             evicted.address.line(), evicted.address.device());
    _authorizedDevices[evictionIndex] = AuthorizedDevice{address, level, now};
}

AuthorizationService::AuthorizedDevice* AuthorizationService::findAuthorizedDevice(
    const IndividualAddress& address)
{
    for (size_t i = 0; i < _authorizedCount; ++i) {
        if (_authorizedDevices[i].address.raw == address.raw) {
            return &_authorizedDevices[i];
        }
    }
    return nullptr;
}

const AuthorizationService::AuthorizedDevice* AuthorizationService::findAuthorizedDevice(
    const IndividualAddress& address) const
{
    for (size_t i = 0; i < _authorizedCount; ++i) {
        if (_authorizedDevices[i].address.raw == address.raw) {
            return &_authorizedDevices[i];
        }
    }
    return nullptr;
}

uint32_t AuthorizationService::currentTime_() const
{
    if (_timeSource) {
        return _timeSource();
    }
    return ++_logicalClock;
}

bool AuthorizationService::isExpired_(const AuthorizedDevice& device, uint32_t now) const
{
    if (_authorizationTimeout == NO_TIMEOUT) {
        return false;
    }
    return static_cast<uint32_t>(now - device.timestamp) >= _authorizationTimeout;
}

void AuthorizationService::removeExpiredAuthorizations_(uint32_t now)
{
    size_t index = 0;
    while (index < _authorizedCount) {
        if (!isExpired_(_authorizedDevices[index], now)) {
            ++index;
            continue;
        }
        removeAuthorizedDeviceAt_(index);
    }
}

void AuthorizationService::removeAuthorizedDeviceAt_(size_t index)
{
    for (size_t i = index; i + 1 < _authorizedCount; ++i) {
        _authorizedDevices[i] = _authorizedDevices[i + 1];
    }
    if (_authorizedCount > 0) {
        --_authorizedCount;
        _authorizedDevices[_authorizedCount] = AuthorizedDevice{};
    }
}

size_t AuthorizationService::oldestAuthorizationIndex_() const
{
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
