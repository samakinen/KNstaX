// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file security_interface_object.cpp
 * @brief Security Interface Object implementation
 */

#include "knx/objects/security_interface_object.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include <algorithm>
#include <cstring>
#include <optional>

namespace knx {
namespace objects {

SecurityInterfaceObject::SecurityInterfaceObject()
    : _securityMode(SecurityMode::Disabled)
    , _toolKey{}
    , _sendingSequence(0)
    , _receivingSequence(0)
    , _lastFailure(SecurityFailure::None)
    , _failureCount(0)
    , _lastFailureSource(0)
{
}

namespace {

// Entry sizes of 03/05/01 §6.3.6 Figure 72, §6.3.7 Figure 74 and §6.3.8
// Figure 75. They match the PDT_GENERIC_nn the properties are declared as.
constexpr size_t kP2PKeyEntryBytes = 20;     // IA_Index(2) + Key(16) + Roles(2)
constexpr size_t kGroupKeyEntryBytes = 18;   // GA_Index(2) + Key(16)
constexpr size_t kZoneKeyEntryBytes = 19;
constexpr size_t kKeyOffset = 2;
constexpr size_t kKeyBytes = 16;

uint16_t readIndex(std::span<const uint8_t> table, size_t offset)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(table[offset]) << 8) | table[offset + 1]);
}

/// The key of the entry in @p table whose index field is @p index. The last
/// such entry wins.
bool findKeyByIndex(std::span<const uint8_t> table, size_t entryBytes, uint16_t index,
                    std::array<uint8_t, 16>& key)
{
    bool found = false;
    for (size_t offset = 0; offset + entryBytes <= table.size(); offset += entryBytes) {
        if (readIndex(table, offset) == index) {
            std::copy_n(table.begin() + static_cast<ptrdiff_t>(offset + kKeyOffset), kKeyBytes, key.begin());
            found = true;
        }
    }
    return found;
}

}  // namespace

// === Security Mode ===

void SecurityInterfaceObject::setSecurityMode(SecurityMode mode) {
    _securityMode = mode;
    KNX_LOGI("KNX.Security", "Security mode set to %s", 
             mode == SecurityMode::Enabled ? "ENABLED" : "DISABLED");
}

void SecurityInterfaceObject::setSecurityModeSilent(SecurityMode mode) {
    _securityMode = mode;
}

// === Tool Key Management ===

void SecurityInterfaceObject::setToolKey(const std::array<uint8_t, 16>& key) {
    _toolKey = key;
    KNX_LOGD("KNX.Security", "Tool key set");
}

void SecurityInterfaceObject::setToolKeySilent(const std::array<uint8_t, 16>& key) {
    _toolKey = key;
}

bool SecurityInterfaceObject::hasToolKey() const {
    return isKeyNonZero(_toolKey);
}

// === Group and device keys ===

const SecurityInterfaceObject::FirmwareKey*
SecurityInterfaceObject::findFirmwareKey(uint16_t address, bool group) const {
    for (size_t i = 0; i < _firmwareKeyCount; ++i) {
        if (_firmwareKeys[i].address == address && _firmwareKeys[i].group == group) {
            return &_firmwareKeys[i];
        }
    }
    return nullptr;
}

bool SecurityInterfaceObject::setFirmwareKey(uint16_t address, bool group,
                                             const std::array<uint8_t, 16>& key) {
    for (size_t i = 0; i < _firmwareKeyCount; ++i) {
        if (_firmwareKeys[i].address == address && _firmwareKeys[i].group == group) {
            _firmwareKeys[i].key = key;
            return true;
        }
    }
    if (_firmwareKeyCount >= _firmwareKeys.size()) {
        return false;
    }
    _firmwareKeys[_firmwareKeyCount] = FirmwareKey{key, address, group};
    ++_firmwareKeyCount;
    return true;
}

void SecurityInterfaceObject::removeFirmwareKey(uint16_t address, bool group) {
    for (size_t i = 0; i < _firmwareKeyCount; ++i) {
        if (_firmwareKeys[i].address == address && _firmwareKeys[i].group == group) {
            --_firmwareKeyCount;
            _firmwareKeys[i] = _firmwareKeys[_firmwareKeyCount];
            _firmwareKeys[_firmwareKeyCount] = FirmwareKey{};
            return;
        }
    }
}

uint16_t SecurityInterfaceObject::firmwareKeyCount(bool group) const {
    uint16_t count = 0;
    for (size_t i = 0; i < _firmwareKeyCount; ++i) {
        if (_firmwareKeys[i].group == group) {
            ++count;
        }
    }
    return count;
}

void SecurityInterfaceObject::setDeviceKey(const IndividualAddress& address,
                                           const std::array<uint8_t, 16>& key) {
    if (!setFirmwareKey(address.raw, false, key)) {
        KNX_LOGE("KNX.Security", "Firmware device key for %d.%d.%d refused: %u keys set "
                 "(KNX_SECURITY_FIRMWARE_KEYS)",
                 address.area(), address.line(), address.device(),
                 static_cast<unsigned>(_firmwareKeys.size()));
        return;
    }
    if (downloadedDeviceKeysInForce()) {
        KNX_LOGW("KNX.Security",
                 "Firmware device key for %d.%d.%d recorded, not in force: the P2P key table "
                 "ETS downloaded is",
                 address.area(), address.line(), address.device());
        return;
    }
    KNX_LOGD("KNX.Security", "Device key set for %d.%d.%d",
             address.area(), address.line(), address.device());
}

util::Result<void> SecurityInterfaceObject::getDeviceKey(const IndividualAddress& address,
                                           std::array<uint8_t, 16>& key) const {
    if (downloadedDeviceKeysInForce()) {
        // §6.3.6: the IA_Index is the partner's 1-based index in the Security
        // Individual Address Table.
        std::optional<size_t> index;
        {
            platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
            index = security_ia_table::indexOf(_iaTable.used(), address);
        }
        if (index.has_value()
            && findKeyByIndex(_p2pKeyTable.used(), kP2PKeyEntryBytes,
                              static_cast<uint16_t>(*index + 1u), key)) {
            return util::Result<void>::ok();
        }
        return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
    }
    if (const auto* firmware = findFirmwareKey(address.raw, false)) {
        key = firmware->key;
        return util::Result<void>::ok();
    }
    return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
}

void SecurityInterfaceObject::removeDeviceKey(const IndividualAddress& address) {
    removeFirmwareKey(address.raw, false);
    KNX_LOGD("KNX.Security", "Device key removed for %d.%d.%d",
             address.area(), address.line(), address.device());
}

uint16_t SecurityInterfaceObject::deviceKeyCount() const {
    if (downloadedDeviceKeysInForce()) {
        return keyTableSummary().deviceKeys;
    }
    return firmwareKeyCount(false);
}

void SecurityInterfaceObject::setGroupKey(const GroupAddress& address,
                                          const std::array<uint8_t, 16>& key) {
    if (!setFirmwareKey(address.raw, true, key)) {
        KNX_LOGE("KNX.Security", "Firmware group key for %d/%d/%d refused: %u keys set "
                 "(KNX_SECURITY_FIRMWARE_KEYS)",
                 address.main(), address.middle(), address.sub(),
                 static_cast<unsigned>(_firmwareKeys.size()));
        return;
    }
    if (downloadedGroupKeysInForce()) {
        KNX_LOGW("KNX.Security",
                 "Firmware group key for %d/%d/%d recorded, not in force: the group key table "
                 "ETS downloaded is",
                 address.main(), address.middle(), address.sub());
        return;
    }
    KNX_LOGD("KNX.Security", "Group key set for %d/%d/%d",
             address.main(), address.middle(), address.sub());
}

GroupAddress SecurityInterfaceObject::groupAddressAt(uint16_t gaIndex) const {
    if (_groupAddressResolver == nullptr || gaIndex == 0u) {
        return GroupAddress(GroupAddress::invalidValue());
    }
    return _groupAddressResolver(_groupAddressResolverContext, gaIndex);
}

util::Result<void> SecurityInterfaceObject::getGroupKey(const GroupAddress& address,
                                          std::array<uint8_t, 16>& key) const {
    if (downloadedGroupKeysInForce()) {
        // §6.3.7: each entry names its group address by GA_Index. Resolving
        // the entries here, rather than keeping a resolved copy, holds every
        // key once and follows a new address table at once. The last entry
        // for an address wins.
        const auto table = _groupKeyTable.used();
        bool found = false;
        for (size_t offset = 0; offset + kGroupKeyEntryBytes <= table.size();
             offset += kGroupKeyEntryBytes) {
            const GroupAddress entryAddress = groupAddressAt(readIndex(table, offset));
            if (entryAddress.raw != GroupAddress::invalidValue() && entryAddress.raw == address.raw) {
                std::copy_n(table.begin() + static_cast<ptrdiff_t>(offset + kKeyOffset), kKeyBytes,
                            key.begin());
                found = true;
            }
        }
        return found ? util::Result<void>::ok()
                     : util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
    }
    if (const auto* firmware = findFirmwareKey(address.raw, true)) {
        key = firmware->key;
        return util::Result<void>::ok();
    }
    return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
}

void SecurityInterfaceObject::removeGroupKey(const GroupAddress& address) {
    removeFirmwareKey(address.raw, true);
    KNX_LOGD("KNX.Security", "Group key removed for %d/%d/%d",
             address.main(), address.middle(), address.sub());
}

uint16_t SecurityInterfaceObject::groupKeyCount() const {
    if (downloadedGroupKeysInForce()) {
        return keyTableSummary().groupKeys;
    }
    return firmwareKeyCount(true);
}

void SecurityInterfaceObject::setGroupAddressResolver(GroupAddressResolver resolver,
                                                      const void* context) {
    _groupAddressResolver = resolver;
    _groupAddressResolverContext = context;
}

SecurityInterfaceObject::KeyTableSummary SecurityInterfaceObject::keyTableSummary() const {
    KeyTableSummary summary;
    const auto groupTable = _groupKeyTable.used();
    for (size_t offset = 0; offset + kGroupKeyEntryBytes <= groupTable.size();
         offset += kGroupKeyEntryBytes) {
        if (groupAddressAt(readIndex(groupTable, offset)).raw != GroupAddress::invalidValue()) {
            ++summary.groupKeys;
        } else {
            // The group address table is downloaded as its own load procedure,
            // so this is expected mid-download; a leftover after it means the
            // two tables disagree.
            ++summary.unresolvedGroupKeys;
        }
    }

    {
        platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
        summary.securityAddresses = _iaTable.count;
    }
    const auto p2pTable = _p2pKeyTable.used();
    for (size_t offset = 0; offset + kP2PKeyEntryBytes <= p2pTable.size();
         offset += kP2PKeyEntryBytes) {
        // IA_Index is 1-based, like every other KNX table index. The Roles
        // field is not evaluated: this stack knows only Tool and Unlisted.
        const uint16_t iaIndex = readIndex(p2pTable, offset);
        if (iaIndex != 0u && iaIndex <= summary.securityAddresses) {
            ++summary.deviceKeys;
        } else {
            ++summary.unresolvedDeviceKeys;
        }
    }
    return summary;
}

// === Sequence Number Management ===

util::Result<uint64_t> SecurityInterfaceObject::incrementSendingSequence() {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    const uint64_t next = _sendingSequence + 1u;
    if (_sendingReserveWriter != nullptr) {
        if (next > _sendingCeiling) {
            // Nothing reserved for this number (the first send after a start-up,
            // or a background top-up that has not succeeded): store a ceiling
            // first. The writer touches only persistence, never this object, so
            // calling it under the lock cannot deadlock, and it keeps two
            // senders from taking numbers past a ceiling that is not stored yet.
            if (!storeSendingCeilingLocked(_sendingSequence + _sendingReserveWindow)) {
                KNX_LOGE("KNX.Security", "Sending sequence %llu not reserved: refusing the secured send",
                         (unsigned long long)next);
                return util::ErrorCode::ResourceUnavailable;
            }
        }
        if (_sendingCeiling - next < _sendingReserveWindow / 2u) {
            _sendingReserveLow = true;
        }
    }
    _sendingSequence = next;
    return next;
}

bool SecurityInterfaceObject::storeSendingCeilingLocked(uint64_t ceiling) {
    if (!_sendingReserveWriter(_sendingReserveContext, ceiling)) {
        return false;
    }
    _sendingCeiling = ceiling;
    _sendingReserveLow = false;
    KNX_LOGD("KNX.Security", "Sending sequence reserved up to %llu", (unsigned long long)ceiling);
    return true;
}

void SecurityInterfaceObject::setSendingReserveWriter(SendingReserveWriter writer, void* context,
                                                      uint64_t window) {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    _sendingReserveWriter = writer;
    _sendingReserveContext = context;
    _sendingReserveWindow = window < 2u ? 2u : window;
}

uint64_t SecurityInterfaceObject::sendingReserveCeiling() const {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    return _sendingCeiling;
}

void SecurityInterfaceObject::restoreSendingReserveCeiling(uint64_t ceiling) {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    if (ceiling > _sendingSequence) {
        _sendingSequence = ceiling;
    }
    // Everything up to the counter may have been used before the reset, so
    // nothing above it is reserved yet.
    _sendingCeiling = _sendingSequence;
    _sendingReserveLow = false;
}

bool SecurityInterfaceObject::sendingReserveLow() const {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    return _sendingReserveLow;
}

bool SecurityInterfaceObject::topUpSendingReserve() {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    if (_sendingReserveWriter == nullptr) {
        _sendingReserveLow = false;
        return true;
    }
    const uint64_t ceiling = _sendingSequence + _sendingReserveWindow;
    if (ceiling <= _sendingCeiling) {
        _sendingReserveLow = false;
        return true;
    }
    return storeSendingCeilingLocked(ceiling);
}

uint64_t SecurityInterfaceObject::getSendingSequenceThreadSafe() const {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    return _sendingSequence;
}

util::Result<void> SecurityInterfaceObject::updateReceivingSequence(uint64_t seq) {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    
    // Sequence number must be strictly increasing
    if (seq <= _receivingSequence) {
        KNX_LOGW("KNX.Security", "Invalid sequence: got %llu, expected > %llu", 
                 (unsigned long long)seq, (unsigned long long)_receivingSequence);
        logSecurityFailure(SecurityFailure::SequenceNumberInvalid);
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    _receivingSequence = seq;
    return util::Result<void>::ok();
}

void SecurityInterfaceObject::setSendingSequence(uint64_t seq) {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    _sendingSequence = seq;
    KNX_LOGD("KNX.Security", "Sending sequence set to %llu", (unsigned long long)seq);
}

void SecurityInterfaceObject::setSendingSequenceSilent(uint64_t seq) {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    _sendingSequence = seq;
}

void SecurityInterfaceObject::setReceivingSequence(uint64_t seq) {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    _receivingSequence = seq;
    KNX_LOGD("KNX.Security", "Receiving sequence set to %llu", (unsigned long long)seq);
}

uint64_t SecurityInterfaceObject::getToolAccessSequence() const {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    return _toolAccessSequence;
}

void SecurityInterfaceObject::setToolAccessSequence(uint64_t seq) {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    if (seq == _toolAccessSequence) {
        return;
    }
    _toolAccessSequence = seq;
    _toolAccessSequenceDirty = true;
}

uint64_t SecurityInterfaceObject::getPeerSequence(const IndividualAddress& address) const {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    if (const auto index = security_ia_table::indexOf(_iaTable.used(), address)) {
        return security_ia_table::sequenceAt(_iaTable.used(), *index);
    }
    if (const auto index = security_ia_table::indexOf(_unlisted.used(), address)) {
        return security_ia_table::sequenceAt(_unlisted.used(), *index);
    }
    return 0u;
}

util::Result<void> SecurityInterfaceObject::setPeerSequence(const IndividualAddress& address,
                                                            uint64_t seq) {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);

    // §6.3.8.4: after decrypting an S-A_Data-PDU the device "shall update the
    // field Last Valid SeqNr for that IA in the Security Individual Address
    // Table". The table is a persisted property, so this is also what makes
    // the value durable.
    const auto table = _iaTable.used();
    if (const auto index = security_ia_table::indexOf(table, address)) {
        if (security_ia_table::sequenceAt(table, *index) != seq) {
            security_ia_table::setEntryAt(table, *index, address, seq);
            _peerSequencesDirty = true;
        }
        return util::Result<void>::ok();
    }

    if (!parkSequenceLocked(address, seq)) {
        KNX_LOGW("KNX.Security",
                 "Sequence number of %d.%d.%d not tracked: it is not in the Security Individual "
                 "Address Table and all %u unlisted slots are taken (KNX_SECURITY_UNLISTED_SENDERS)",
                 address.area(), address.line(), address.device(),
                 static_cast<unsigned>(config::SECURITY_UNLISTED_SENDERS));
        return util::ErrorCode::ResourceUnavailable;
    }
    return util::Result<void>::ok();
}

bool SecurityInterfaceObject::canTrackPeer(const IndividualAddress& address) const {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    return _unlisted.count < config::SECURITY_UNLISTED_SENDERS
        || security_ia_table::indexOf(_iaTable.used(), address).has_value()
        || security_ia_table::indexOf(_unlisted.used(), address).has_value();
}

bool SecurityInterfaceObject::parkSequenceLocked(IndividualAddress address, uint64_t seq) {
    const auto slots = _unlisted.used();
    if (const auto index = security_ia_table::indexOf(slots, address)) {
        security_ia_table::setEntryAt(slots, *index, address, seq);
        return true;
    }
    if (_unlisted.count >= config::SECURITY_UNLISTED_SENDERS) {
        return false;
    }
    ++_unlisted.count;
    security_ia_table::setEntryAt(_unlisted.used(), _unlisted.count - 1u, address, seq);
    return true;
}

void SecurityInterfaceObject::removeUnlistedLocked(size_t index) {
    constexpr size_t kEntry = security_ia_table::kEntryBytes;
    const size_t last = _unlisted.count - 1u;
    if (index != last) {
        std::copy_n(_unlisted.data.begin() + static_cast<ptrdiff_t>(last * kEntry), kEntry,
                    _unlisted.data.begin() + static_cast<ptrdiff_t>(index * kEntry));
    }
    std::fill_n(_unlisted.data.begin() + static_cast<ptrdiff_t>(last * kEntry), kEntry, uint8_t{0});
    --_unlisted.count;
}

void SecurityInterfaceObject::parkIaEntriesLocked(size_t first, size_t count) {
    // Entries leaving the table keep their Last Valid SeqNr in an unlisted
    // slot: when ETS downloads the same partner again, adoptParkedLocked()
    // puts the higher number back, so a download cannot reopen a replay
    // window. Only numbers above 0 carry anything.
    const auto table = _iaTable.used();
    for (size_t index = first; index < first + count && index < _iaTable.count; ++index) {
        const uint64_t seq = security_ia_table::sequenceAt(table, index);
        if (seq == 0u) {
            continue;
        }
        const IndividualAddress address = security_ia_table::addressAt(table, index);
        if (!parkSequenceLocked(address, seq)) {
            KNX_LOGW("KNX.Security",
                     "Last Valid SeqNr of %d.%d.%d dropped: all %u unlisted slots are taken",
                     address.area(), address.line(), address.device(),
                     static_cast<unsigned>(config::SECURITY_UNLISTED_SENDERS));
        }
    }
}

void SecurityInterfaceObject::adoptParkedLocked(size_t first, size_t count) {
    // A table entry and an unlisted slot for the same sender: the higher
    // number wins, so neither a restored table nor a stale ETS download rolls
    // the replay window back, and the slot is free again.
    const auto table = _iaTable.used();
    for (size_t index = first; index < first + count && index < _iaTable.count; ++index) {
        const IndividualAddress address = security_ia_table::addressAt(table, index);
        const auto slot = security_ia_table::indexOf(_unlisted.used(), address);
        if (!slot.has_value()) {
            continue;
        }
        const uint64_t parked = security_ia_table::sequenceAt(_unlisted.used(), *slot);
        if (parked > security_ia_table::sequenceAt(table, index)) {
            security_ia_table::setEntryAt(table, index, address, parked);
            _peerSequencesDirty = true;
        }
        removeUnlistedLocked(*slot);
    }
}

bool SecurityInterfaceObject::sequenceStateDirty() const {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    return _peerSequencesDirty || _toolAccessSequenceDirty;
}

void SecurityInterfaceObject::clearSequenceStateDirty() {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    _peerSequencesDirty = false;
    _toolAccessSequenceDirty = false;
}

bool SecurityInterfaceObject::peerSequencesDirty() const {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    return _peerSequencesDirty;
}

bool SecurityInterfaceObject::toolAccessSequenceDirty() const {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    return _toolAccessSequenceDirty;
}

bool SecurityInterfaceObject::takePeerSequencesDirty() {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    const bool dirty = _peerSequencesDirty;
    _peerSequencesDirty = false;
    return dirty;
}

bool SecurityInterfaceObject::takeToolAccessSequenceDirty() {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    const bool dirty = _toolAccessSequenceDirty;
    _toolAccessSequenceDirty = false;
    return dirty;
}

void SecurityInterfaceObject::markPeerSequencesDirty() {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    _peerSequencesDirty = true;
}

void SecurityInterfaceObject::markToolAccessSequenceDirty() {
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    _toolAccessSequenceDirty = true;
}

// === Security Failures ===

void SecurityInterfaceObject::logSecurityFailure(SecurityFailure failure, uint16_t source) {
    _lastFailure = failure;
    _failureCount++;
    _lastFailureSource = source;
    
    const char* failureStr = "Unknown";
    switch (failure) {
        case SecurityFailure::None: failureStr = "None"; break;
        case SecurityFailure::SequenceNumberInvalid: failureStr = "Invalid Sequence"; break;
        case SecurityFailure::AuthenticationFailed: failureStr = "Auth Failed"; break;
        case SecurityFailure::DecryptionFailed: failureStr = "Decryption Failed"; break;
        case SecurityFailure::UnknownKey: failureStr = "Unknown Key"; break;
        case SecurityFailure::InvalidSender: failureStr = "Invalid Sender"; break;
    }
    
    KNX_LOGW("KNX.Security", "Security failure #%lu: %s (source: 0x%04X)",
             static_cast<unsigned long>(_failureCount), failureStr, source);
}

void SecurityInterfaceObject::clearFailureLog() {
    _lastFailure = SecurityFailure::None;
    _failureCount = 0;
    _lastFailureSource = 0;
    KNX_LOGI("KNX.Security", "Security failure log cleared");
}

namespace {
util::Result<uint16_t> getObjectType(const SecurityInterfaceObject& /*domain*/) {
    return InterfaceObjectType::security().value();
}

util::Result<uint8_t> getSecurityMode(const SecurityInterfaceObject& domain) {
    return static_cast<uint8_t>(domain.getSecurityMode());
}

util::Result<void> setSecurityMode(SecurityInterfaceObject& domain, const uint8_t& value) {
    domain.setSecurityModeSilent(static_cast<SecurityMode>(value));
    return util::Result<void>::ok();
}

util::Result<void> readToolKey(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* /*userData*/)
{
    if (startIndex.value != 0 || elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto* domain = static_cast<const SecurityInterfaceObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto& key = domain->getToolKey();
    return out.writeBytes(key);
}

util::Result<void> writeToolKey(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* /*userData*/)
{
    if (startIndex.value != 0 || elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    auto* domain = static_cast<SecurityInterfaceObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    std::array<uint8_t, 16> key{};
    auto res = in.readBytes(key);
    if (res.isError()) {
        return res;
    }
    domain->setToolKeySilent(key);
    return util::Result<void>::ok();
}

util::Result<void> readSequenceNumberSending(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* /*userData*/)
{
    if (startIndex.value != 0 || elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto* domain = static_cast<const SecurityInterfaceObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const uint64_t seq = domain->getSendingSequenceThreadSafe();
    for (int i = 5; i >= 0; --i) {
        auto res = out.u8(static_cast<uint8_t>((seq >> (i * 8)) & 0xFFu));
        if (res.isError()) {
            return res;
        }
    }
    return util::Result<void>::ok();
}

util::Result<void> writeSequenceNumberSending(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* /*userData*/)
{
    if (startIndex.value != 0 || elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    auto* domain = static_cast<SecurityInterfaceObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    uint8_t buffer[6]{};
    auto res = in.readBytes(buffer);
    if (res.isError()) {
        return res;
    }
    uint64_t seq = 0;
    for (uint8_t b : buffer) {
        seq = (seq << 8) | b;
    }
    domain->setSendingSequenceSilent(seq);
    return util::Result<void>::ok();
}

util::Result<void> readSecurityFailuresLog(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* /*userData*/)
{
    if (startIndex.value != 0 || elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto* domain = static_cast<const SecurityInterfaceObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    auto res = out.u8(static_cast<uint8_t>(domain->getLastFailure()));
    if (res.isError()) return res;
    const uint32_t count = domain->getFailureCount();
    res = out.u8(static_cast<uint8_t>((count >> 24) & 0xFFu)); if (res.isError()) return res;
    res = out.u8(static_cast<uint8_t>((count >> 16) & 0xFFu)); if (res.isError()) return res;
    res = out.u8(static_cast<uint8_t>((count >> 8) & 0xFFu)); if (res.isError()) return res;
    res = out.u8(static_cast<uint8_t>(count & 0xFFu)); if (res.isError()) return res;
    res = out.u8(static_cast<uint8_t>((domain->getLastFailureSource() >> 8) & 0xFFu)); if (res.isError()) return res;
    return out.u8(static_cast<uint8_t>(domain->getLastFailureSource() & 0xFFu));
}

util::Result<void> writeSecurityFailuresLog(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* /*userData*/)
{
    if (startIndex.value != 0 || elementCount != 1) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (context.domain == nullptr) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    // Accepted and not stored: a read answers from the failure counters.
    std::array<uint8_t, 7> data{};
    return in.readBytes(data);
}

/// userData of the array properties: which table the handler serves.
struct ArrayPropertyInfo {
    SecurityProperty prop;
};

util::Result<void> readArrayProperty(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* userData)
{
    const auto* domain = static_cast<const SecurityInterfaceObject*>(context.domain);
    const auto* info = static_cast<const ArrayPropertyInfo*>(userData);
    if (!domain || !info) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const auto data = domain->tableData(info->prop);
    const size_t elementSize = domain->tableElementSize(info->prop);
    const size_t offset = static_cast<size_t>(startIndex.value) * elementSize;
    const size_t count = static_cast<size_t>(elementCount) * elementSize;

    if (data.empty()) {
        // Never written: zeros, as for an element that exists but is unset.
        for (size_t i = 0; i < count; ++i) {
            if (auto res = out.u8(0); res.isError()) {
                return res;
            }
        }
        return util::Result<void>::ok();
    }
    if (offset + count > data.size()) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }
    return out.writeBytes(data.subspan(offset, count));
}

util::Result<void> writeArrayProperty(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* userData)
{
    auto* domain = static_cast<SecurityInterfaceObject*>(context.domain);
    const auto* info = static_cast<const ArrayPropertyInfo*>(userData);
    if (!domain || !info) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const size_t count = static_cast<size_t>(elementCount) * domain->tableElementSize(info->prop);
    if (count == 0 || in.remaining() < count) {
        return util::Result<void>::err(util::ErrorCode::BufferTooSmall);
    }
    if (auto res = domain->writeTable(info->prop, startIndex.value, in.peek(count)); res.isError()) {
        return res;
    }
    (void)in.skip(count);
    return util::Result<void>::ok();
}

/// Current number of elements of an array property: how much of the table
/// ETS has downloaded, not how much it could download.
util::Result<uint16_t> countArrayProperty(const PropertyContext& context, const void* userData)
{
    const auto* domain = static_cast<const SecurityInterfaceObject*>(context.domain);
    const auto* info = static_cast<const ArrayPropertyInfo*>(userData);
    if (!domain || !info) {
        return util::Result<uint16_t>(util::ErrorCode::InvalidParameter);
    }
    const size_t elementSize = domain->tableElementSize(info->prop);
    return util::Result<uint16_t>(
        static_cast<uint16_t>(elementSize == 0 ? 0 : domain->tableData(info->prop).size() / elementSize));
}

/// Set the current number of elements. ETS shortens a table this way before
/// re-downloading it — "OT=17 PID=54 nr_of_elem=1 start_index=0 data=0000" is
/// literally "clear the Security Individual Address Table". Growing is allowed
/// too and zero-fills, so a subsequent element write lands in a defined array.
util::Result<void> resizeArrayProperty(const PropertyContext& context,
                                       uint16_t elementCount,
                                       const void* userData)
{
    auto* domain = static_cast<SecurityInterfaceObject*>(context.domain);
    const auto* info = static_cast<const ArrayPropertyInfo*>(userData);
    if (!domain || !info) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    return domain->resizeTable(info->prop, elementCount);
}

/// maxElements of an array property: its capacity, which ETS reads from the
/// property description. The kernel needs at least 1.
constexpr uint16_t declaredCapacity(size_t capacity)
{
    return static_cast<uint16_t>(capacity == 0 ? 1 : capacity);
}

static constexpr ScalarPropertyData<SecurityInterfaceObject, uint16_t> kObjectTypeData{ &getObjectType, nullptr };
static constexpr ScalarPropertyData<SecurityInterfaceObject, uint8_t> kSecurityModeData{ &getSecurityMode, &setSecurityMode };

static constexpr ArrayPropertyInfo kP2PKeyInfo{ SecurityProperty::P2PKeyTable };
static constexpr ArrayPropertyInfo kGroupKeyInfo{ SecurityProperty::GroupKeyTable };
static constexpr ArrayPropertyInfo kIndividualAddrInfo{ SecurityProperty::SecurityIndividualAddressTable };
static constexpr ArrayPropertyInfo kSecurityReportInfo{ SecurityProperty::SecurityReport };
static constexpr ArrayPropertyInfo kSecurityReportControlInfo{ SecurityProperty::SecurityReportControl };
static constexpr ArrayPropertyInfo kZoneKeyInfo{ SecurityProperty::ZoneKeyTable };
static constexpr ArrayPropertyInfo kGoSecurityFlagsInfo{ SecurityProperty::GoSecurityFlags };

static constexpr PropertyHandler kSecurityHandlers[] = {
    ScalarProperty<SecurityInterfaceObject, uint16_t>::make(
        application::PropertyID::ObjectType,
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kObjectTypeData),

    // PID_LOAD_STATE_CONTROL: ETS runs the standard load procedure
    // (StartLoading -> write key tables -> LoadCompleted) before it will
    // download Data Secure key material. Without this property the download
    // aborts at the first step.
    LoadControlProperty<SecurityInterfaceObject>::make(
        static_cast<application::PropertyID>(SecurityProperty::LoadStateControl)),

    ScalarProperty<SecurityInterfaceObject, uint8_t>::make(
        static_cast<application::PropertyID>(SecurityProperty::SecurityMode),
        application::PropertyDataType::UnsignedChar,
        PropertyCapability::ReadWrite,
        &kSecurityModeData),

    {
        static_cast<application::PropertyID>(SecurityProperty::ToolKey),
        application::PropertyDataType::Generic16,
        PropertyCapability::ReadWrite,
        1,
        0,
        &readToolKey,
        &writeToolKey,
        nullptr
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::SequenceNumberSending),
        application::PropertyDataType::Generic06,
        PropertyCapability::ReadWrite,
        1,
        0,
        &readSequenceNumberSending,
        &writeSequenceNumberSending,
        nullptr
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::SecurityFailuresLog),
        application::PropertyDataType::Generic07,
        PropertyCapability::ReadWrite,
        1,
        0,
        &readSecurityFailuresLog,
        &writeSecurityFailuresLog,
        nullptr
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::P2PKeyTable),
        application::PropertyDataType::Generic20,
        PropertyCapability::ReadWrite,
        declaredCapacity(config::SECURITY_P2P_KEYS),
        0,
        &readArrayProperty,
        &writeArrayProperty,
        &kP2PKeyInfo,
        &countArrayProperty,
        &resizeArrayProperty
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::GroupKeyTable),
        application::PropertyDataType::Generic18,
        PropertyCapability::ReadWrite,
        declaredCapacity(config::SECURITY_GROUP_KEYS),
        0,
        &readArrayProperty,
        &writeArrayProperty,
        &kGroupKeyInfo,
        &countArrayProperty,
        &resizeArrayProperty
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::SecurityIndividualAddressTable),
        application::PropertyDataType::Generic08,
        PropertyCapability::ReadWrite,
        declaredCapacity(config::SECURITY_IA_ENTRIES),
        0,
        &readArrayProperty,
        &writeArrayProperty,
        &kIndividualAddrInfo,
        &countArrayProperty,
        &resizeArrayProperty
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::SecurityReport),
        application::PropertyDataType::Bitset8,
        PropertyCapability::ReadWrite,
        1,
        0,
        &readArrayProperty,
        &writeArrayProperty,
        &kSecurityReportInfo
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::SecurityReportControl),
        application::PropertyDataType::Bitset8,
        PropertyCapability::ReadWrite,
        1,
        0,
        &readArrayProperty,
        &writeArrayProperty,
        &kSecurityReportControlInfo
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::ZoneKeyTable),
        application::PropertyDataType::Generic19,
        PropertyCapability::ReadWrite,
        declaredCapacity(config::SECURITY_ZONE_KEYS),
        0,
        &readArrayProperty,
        &writeArrayProperty,
        &kZoneKeyInfo,
        &countArrayProperty,
        &resizeArrayProperty
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::GoSecurityFlags),
        application::PropertyDataType::GenericData,
        PropertyCapability::ReadWrite,
        0xFFFF,
        1,
        &readArrayProperty,
        &writeArrayProperty,
        &kGoSecurityFlagsInfo,
        &countArrayProperty,
        &resizeArrayProperty
    }
};

constexpr size_t kSecurityHandlerCount = sizeof(kSecurityHandlers) / sizeof(kSecurityHandlers[0]);
static_assert(validatePropertyTableLayout(kSecurityHandlers, kSecurityHandlerCount).isOk(),
              "kSecurityHandlers breaks the property kernel's rules");
} // namespace

KernelBinding SecurityInterfaceObject::kernelBinding() const {
    KernelBinding binding;
    binding.handlers = kSecurityHandlers;
    binding.handlerCount = kSecurityHandlerCount;
    binding.context = PropertyContext{const_cast<SecurityInterfaceObject*>(this), _validationPolicy};
    return binding;
}

// === Bulk Operations ===

void SecurityInterfaceObject::clearAllKeys() {
    _toolKey.fill(0);
    _p2pKeyTable.clear();
    _groupKeyTable.clear();
    _firmwareKeys.fill(FirmwareKey{});
    _firmwareKeyCount = 0;
    KNX_LOGI("KNX.Security", "All security keys cleared");
}

void SecurityInterfaceObject::loadControlReset() {
    // Tool key intentionally preserved — see the declaration for why. With
    // the tables gone, the firmware-provided keys are in force again. The
    // tables are zeroed, not only emptied: keys do not linger in RAM.
    {
        platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
        parkIaEntriesLocked(0, _iaTable.count);
        _iaTable.clear();
    }
    _p2pKeyTable.clear();
    _groupKeyTable.clear();
    _zoneKeyTable.clear();
    if (_goSecurityFlags) {
        std::fill_n(_goSecurityFlags.get(), _goSecurityFlagsCapacity, uint8_t{0});
    }
    _goSecurityFlagsCount = 0;
    KNX_LOGI("KNX.Security", "Security key tables cleared for load");
}

bool SecurityInterfaceObject::isValid() const {
    // Security object is valid if:
    // - Security is disabled, OR
    // - Security is enabled AND tool key is set
    if (_securityMode == SecurityMode::Disabled) {
        return true;
    }

    return hasToolKey();
}

// === Array properties ===

bool SecurityInterfaceObject::isArrayProperty(SecurityProperty prop) {
    switch (prop) {
        case SecurityProperty::P2PKeyTable:
        case SecurityProperty::GroupKeyTable:
        case SecurityProperty::SecurityIndividualAddressTable:
        case SecurityProperty::ZoneKeyTable:
        case SecurityProperty::GoSecurityFlags:
        case SecurityProperty::SecurityReport:
        case SecurityProperty::SecurityReportControl:
            return true;
        default:
            return false;
    }
}

SecurityInterfaceObject::TableRef SecurityInterfaceObject::tableRef(SecurityProperty prop) {
    const auto ref = [](auto& table, size_t elementSize) {
        return TableRef{table.data.data(), &table.count,
                        static_cast<uint16_t>(table.data.size() / elementSize),
                        static_cast<uint8_t>(elementSize)};
    };
    switch (prop) {
        case SecurityProperty::P2PKeyTable:
            return ref(_p2pKeyTable, kP2PKeyEntryBytes);
        case SecurityProperty::GroupKeyTable:
            return ref(_groupKeyTable, kGroupKeyEntryBytes);
        case SecurityProperty::SecurityIndividualAddressTable:
            return ref(_iaTable, security_ia_table::kEntryBytes);
        case SecurityProperty::ZoneKeyTable:
            return ref(_zoneKeyTable, kZoneKeyEntryBytes);
        case SecurityProperty::SecurityReport:
            return ref(_securityReport, 1);
        case SecurityProperty::SecurityReportControl:
            return ref(_securityReportControl, 1);
        case SecurityProperty::GoSecurityFlags:
            return TableRef{_goSecurityFlags.get(), &_goSecurityFlagsCount, _goSecurityFlagsCapacity, 1};
        default:
            return TableRef{};
    }
}

std::span<const uint8_t> SecurityInterfaceObject::tableData(SecurityProperty prop) const {
    const TableRef table = tableRef(prop);
    if (table.data == nullptr || table.count == nullptr) {
        return {};
    }
    return std::span<const uint8_t>(table.data, static_cast<size_t>(*table.count) * table.elementSize);
}

uint8_t SecurityInterfaceObject::tableElementSize(SecurityProperty prop) const {
    return tableRef(prop).elementSize;
}

uint16_t SecurityInterfaceObject::tableCapacity(SecurityProperty prop) const {
    return tableRef(prop).capacity;
}

util::Result<void> SecurityInterfaceObject::writeTable(SecurityProperty prop, size_t firstElement,
                                                       std::span<const uint8_t> data) {
    const TableRef table = tableRef(prop);
    if (table.count == nullptr || table.elementSize == 0 || data.size() % table.elementSize != 0) {
        return util::ErrorCode::InvalidParameter;
    }
    const size_t elements = data.size() / table.elementSize;
    if (firstElement + elements > table.capacity) {
        // The capacity is what the product file promised ETS; a table that
        // does not fit gets a negative response, as the address table does.
        KNX_LOGW("KNX.Security", "Write of PID %u elements %u..%u refused: capacity %u",
                 static_cast<unsigned>(prop), static_cast<unsigned>(firstElement + 1u),
                 static_cast<unsigned>(firstElement + elements), static_cast<unsigned>(table.capacity));
        return util::ErrorCode::OutOfRange;
    }

    const auto write = [&] {
        if (firstElement > *table.count) {
            // A gap is zero-filled, so every element below the count is defined.
            std::fill(table.data + static_cast<size_t>(*table.count) * table.elementSize,
                      table.data + firstElement * table.elementSize, uint8_t{0});
        }
        std::copy(data.begin(), data.end(), table.data + firstElement * table.elementSize);
        if (firstElement + elements > *table.count) {
            *table.count = static_cast<uint16_t>(firstElement + elements);
        }
    };

    if (prop != SecurityProperty::SecurityIndividualAddressTable) {
        write();
        return util::Result<void>::ok();
    }

    // The partners' Last Valid SeqNr values live in this table. Every entry
    // the write replaces keeps its number in an unlisted slot first, and each
    // written entry then takes the higher of its own number and its slot's:
    // a download, even of the same partner, never moves a number back.
    platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
    parkIaEntriesLocked(firstElement, elements);
    write();
    adoptParkedLocked(firstElement, elements);
    return util::Result<void>::ok();
}

util::Result<void> SecurityInterfaceObject::resizeTable(SecurityProperty prop, uint16_t count) {
    const TableRef table = tableRef(prop);
    if (table.count == nullptr) {
        return util::ErrorCode::InvalidParameter;
    }
    if (count > table.capacity) {
        KNX_LOGW("KNX.Security", "PID %u resized to %u elements refused: capacity %u",
                 static_cast<unsigned>(prop), static_cast<unsigned>(count),
                 static_cast<unsigned>(table.capacity));
        return util::ErrorCode::OutOfRange;
    }

    // An empty table is a real state, not an absent one: the persistence
    // layer turns it into an erase, so the previous download's entries cannot
    // come back at the next boot.
    const auto resize = [&] {
        std::fill(table.data, table.data + static_cast<size_t>(table.capacity) * table.elementSize,
                  uint8_t{0});
        *table.count = count;
    };
    if (prop == SecurityProperty::SecurityIndividualAddressTable) {
        platform::LocalLock<platform::LocalMutex> lock(_sequenceMutex);
        parkIaEntriesLocked(0, _iaTable.count);
        resize();
    } else {
        resize();
    }
    return util::Result<void>::ok();
}

void SecurityInterfaceObject::setGroupObjectCount(uint16_t count) {
    if (_goSecurityFlags || count == 0u) {
        return;
    }
    _goSecurityFlags = std::make_unique<uint8_t[]>(count);
    _goSecurityFlagsCapacity = count;
}

uint8_t SecurityInterfaceObject::goSecurityFlags(uint16_t number) const {
    return number < _goSecurityFlagsCount ? _goSecurityFlags[number] : uint8_t{0};
}

// === Helper Methods ===

bool SecurityInterfaceObject::isKeyNonZero(const std::array<uint8_t, 16>& key) {
    return std::any_of(key.begin(), key.end(), [](uint8_t b) { return b != 0; });
}

// === InterfaceObject implementation ===

} // namespace objects
} // namespace knx
