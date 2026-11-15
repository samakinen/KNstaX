// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file authorization_service.hpp
 * @brief A_Authorize and A_Key_Write: access levels and the keys that grant them
 *
 * 03/03/07 §3.5.7 and §3.5.8. Access levels are numbered as on the wire: 0 is
 * maximum access, 15 minimum. Each of levels 0…14 can hold one four-octet key
 * (§3.5.8 Figure 88); FFFFFFFFh means "no key" for that level. The table is
 * device configuration: the BusAccessUnit persists it and restores it at
 * start-up, and Master Reset 02h/07h resets part of it.
 */

#pragma once

#include "knx/config.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include "knx/util/inplace_function.hpp"
#include <span>

namespace knx {
namespace application {

/// A four-octet authorisation key, most significant octet first.
using AuthorizationKey = std::array<uint8_t, 4>;

/// An access level in wire numbering: 0 = maximum access, 15 = minimum.
using AccessLevel = uint8_t;

/**
 * @brief Access levels and their keys, and each partner's current level.
 *
 * @thread_safety Owner-context only (the application layer's task).
 */
class AuthorizationService {
public:
    /// 03/03/07 §3.5.7: "0 (maximum level, i.e. maximum access rights) and 15
    /// (minimum level, i.e. minimum access rights)".
    static constexpr AccessLevel kMaximumAccessLevel = 0u;
    static constexpr AccessLevel kMinimumAccessLevel = 15u;

    /// §3.5.8 Figure 88: a key for each of levels 0…N-1, and level N, here the
    /// minimum, reached without one ("None — N (free access)").
    static constexpr size_t kKeyedLevelCount = kMinimumAccessLevel;

    /// §3.5.8: a key of FFFFFFFFh means "there shall be no key associated to
    /// the corresponding level any more". Also the ex-factory value (03/05/02
    /// §3.7.1.2.3.2.3 NOTE 15).
    static constexpr AuthorizationKey kNoKey = {0xFFu, 0xFFu, 0xFFu, 0xFFu};

    /// §3.5.8: the level returned in A_Key_Response when the requester's
    /// current access level does not allow the write.
    static constexpr AccessLevel kKeyWriteRefused = 0xFFu;

    using KeyTable = std::array<AuthorizationKey, kKeyedLevelCount>;

    /// Size of a key table in persistent storage: the keys in level order.
    static constexpr size_t kSerializedKeyTableSize = kKeyedLevelCount * 4u;

    /// Sends A_Authorize_Response(level) to @p destination.
    using ResponseCallback = util::InplaceFunction<void(const IndividualAddress& destination, AccessLevel level), ::knx::config::callbackBytes(32)>;

    /// The key table changed through a management service; persist it.
    using KeyTableChangedCallback = util::InplaceFunction<void(const KeyTable& keys), ::knx::config::callbackBytes(32)>;

    /// Monotonic ticks for the optional authorisation expiry.
    using TimeSource = util::InplaceFunction<uint32_t(), ::knx::config::callbackBytes(32)>;
    static constexpr uint32_t NO_TIMEOUT = 0u;

    AuthorizationService();

    void setResponseCallback(ResponseCallback callback) { _responseCallback = std::move(callback); }
    void setKeyTableChangedCallback(KeyTableChangedCallback callback) {
        _keyTableChangedCallback = std::move(callback);
    }
    void setTimeSource(TimeSource timeSource) { _timeSource = std::move(timeSource); }
    /// `NO_TIMEOUT` (the default) keeps an authorisation until the connection
    /// is released, which is what §3.5.7 says.
    void setAuthorizationTimeout(uint32_t timeoutTicks) { _authorizationTimeout = timeoutTicks; }

    // --- The key table ------------------------------------------------------

    /// Set the key of @p level (0…14); kNoKey removes it. A local
    /// configuration path, so the change callback is not called.
    util::Result<void> setKey(AccessLevel level, const AuthorizationKey& key);

    [[nodiscard]] const KeyTable& keyTable() const { return _keys; }

    /// Adopt a table restored from storage. Does not call the change callback.
    void restoreKeyTable(const KeyTable& keys) { _keys = keys; }

    /// True once at least one level has a key.
    [[nodiscard]] bool hasKeys() const;

    /**
     * @brief Master Reset 02h/07h: levels @p fromLevel and higher (numerically)
     *        back to "no key".
     *
     * 03/05/02 §3.7.1.2.3.2.3: "the MaS shall set the Authorisation Keys of
     * the access level to which the requesting MaC is authorised and higher
     * level (higher numerical value) to their default settings". Calls the
     * change callback when anything changed.
     */
    void resetKeysFrom(AccessLevel fromLevel);

    static void serializeKeyTable(const KeyTable& keys, std::span<uint8_t, kSerializedKeyTableSize> out);
    static KeyTable deserializeKeyTable(std::span<const uint8_t, kSerializedKeyTableSize> in);

    // --- The services -------------------------------------------------------

    /// A_Authorize_Request.ind (§3.5.7): store and answer the level @p key grants.
    void handleRequest(const IndividualAddress& source, const AuthorizationKey& key);

    /**
     * @brief A_Key_Write.ind (§3.5.8); returns the level for A_Key_Response.
     *
     * "The current access level shall be less or equal to the access level
     * indicated in the A_Key_Write.ind primitive, otherwise the remote
     * application process shall return FFh". A level this device has no key
     * for (15 and above) is answered the same way: nothing was stored.
     */
    AccessLevel handleKeyWrite(const IndividualAddress& source, AccessLevel level, const AuthorizationKey& key);

    /**
     * @brief The level @p key authorises for (§3.5.7).
     *
     * The level whose key it is, the most privileged one when several share
     * it; for FFFFFFFFh that is the free access level. A key that matches no
     * level is invalid and gets the minimum level ("If ... the communication
     * partner authorizes itself with an invalid key, the Remote Management
     * shall select the minimal access level").
     *
     * A device with no key at all does not support authorisation in the sense
     * of that clause: every key, like none, gives level 0.
     */
    [[nodiscard]] AccessLevel levelForKey(const AuthorizationKey& key) const;

    /// "the maximum access level protected with FFFFFFFFh" (§3.5.7): the most
    /// privileged level without a key, or the minimum level when all have one.
    [[nodiscard]] AccessLevel freeAccessLevel() const;

    /**
     * @brief The level @p address holds now, for the access checks.
     *
     * The level it was granted by its last A_Authorize on the current
     * connection; otherwise the free access level (§3.5.7, "if the
     * communication partner does not authorize itself"). On a device nobody
     * has keyed that is level 0 for everyone, so nothing is gated.
     */
    [[nodiscard]] AccessLevel currentAccessLevel(const IndividualAddress& address) const;

    /// Forget @p address's level; it is back to the free access level.
    void clearAuthorization(const IndividualAddress& address);
    void clearAllAuthorizations();

private:
    struct AuthorizedDevice {
        IndividualAddress address;
        AccessLevel level{kMinimumAccessLevel};
        uint32_t timestamp{0};
    };

    static constexpr size_t MAX_AUTHORIZED_DEVICES = 16;

    KeyTable _keys;

    AuthorizedDevice _authorizedDevices[MAX_AUTHORIZED_DEVICES];
    size_t _authorizedCount{0};
    uint32_t _authorizationTimeout{NO_TIMEOUT};
    mutable uint32_t _logicalClock{0};

    ResponseCallback _responseCallback;
    KeyTableChangedCallback _keyTableChangedCallback;
    TimeSource _timeSource;

    void storeAuthorization(const IndividualAddress& address, AccessLevel level);
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
