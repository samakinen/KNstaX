// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file security_interface_object.cpp
 * @brief Security Interface Object implementation
 */

#include "knx/objects/security_interface_object.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include <cstring>
#include <algorithm>

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
    , _extraProperties()
{
}

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

// === Individual Device Keys ===

void SecurityInterfaceObject::setDeviceKey(const IndividualAddress& address, 
                                           const std::array<uint8_t, 16>& key) {
    _deviceKeys[address.raw] = key;
    KNX_LOGD("KNX.Security", "Device key set for %d.%d.%d", 
             address.area(), address.line(), address.device());
}

util::Result<void> SecurityInterfaceObject::getDeviceKey(const IndividualAddress& address, 
                                           std::array<uint8_t, 16>& key) const {
    auto it = _deviceKeys.find(address.raw);
    if (it != _deviceKeys.end()) {
        key = it->second;
        return util::Result<void>::ok();
    }
    return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
}

void SecurityInterfaceObject::removeDeviceKey(const IndividualAddress& address) {
    _deviceKeys.erase(address.raw);
    KNX_LOGD("KNX.Security", "Device key removed for %d.%d.%d", 
             address.area(), address.line(), address.device());
}

uint16_t SecurityInterfaceObject::deviceKeyCount() const {
    return static_cast<uint16_t>(_deviceKeys.size());
}

// === Group Address Keys ===

void SecurityInterfaceObject::setGroupKey(const GroupAddress& address, 
                                          const std::array<uint8_t, 16>& key) {
    _groupKeys[address.raw] = key;
    KNX_LOGD("KNX.Security", "Group key set for %d/%d/%d", 
             address.main(), address.middle(), address.sub());
}

util::Result<void> SecurityInterfaceObject::getGroupKey(const GroupAddress& address, 
                                          std::array<uint8_t, 16>& key) const {
    auto it = _groupKeys.find(address.raw);
    if (it != _groupKeys.end()) {
        key = it->second;
        return util::Result<void>::ok();
    }
    return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
}

void SecurityInterfaceObject::removeGroupKey(const GroupAddress& address) {
    _groupKeys.erase(address.raw);
    KNX_LOGD("KNX.Security", "Group key removed for %d/%d/%d", 
             address.main(), address.middle(), address.sub());
}

uint16_t SecurityInterfaceObject::groupKeyCount() const {
    return static_cast<uint16_t>(_groupKeys.size());
}

void SecurityInterfaceObject::clearGroupKeys() {
    _groupKeys.clear();
}

void SecurityInterfaceObject::clearDeviceKeys() {
    _deviceKeys.clear();
}

// === Sequence Number Management ===

uint64_t SecurityInterfaceObject::incrementSendingSequence() {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    return ++_sendingSequence;
}

uint64_t SecurityInterfaceObject::getSendingSequenceThreadSafe() const {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    return _sendingSequence;
}

util::Result<void> SecurityInterfaceObject::updateReceivingSequence(uint64_t seq) {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    
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
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    _sendingSequence = seq;
    KNX_LOGD("KNX.Security", "Sending sequence set to %llu", (unsigned long long)seq);
}

void SecurityInterfaceObject::setSendingSequenceSilent(uint64_t seq) {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    _sendingSequence = seq;
}

void SecurityInterfaceObject::setReceivingSequence(uint64_t seq) {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    _receivingSequence = seq;
    KNX_LOGD("KNX.Security", "Receiving sequence set to %llu", (unsigned long long)seq);
}

uint64_t SecurityInterfaceObject::getToolAccessSequence() const {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    return _toolAccessSequence;
}

void SecurityInterfaceObject::setToolAccessSequence(uint64_t seq) {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    if (seq == _toolAccessSequence) {
        return;
    }
    _toolAccessSequence = seq;
    _sequenceStateDirty = true;
}

uint64_t SecurityInterfaceObject::getPeerSequence(const IndividualAddress& address) const {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    auto it = _peerSequences.find(address.raw);
    return it == _peerSequences.end() ? 0u : it->second;
}

void SecurityInterfaceObject::setPeerSequence(const IndividualAddress& address, uint64_t seq) {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    auto& stored = _peerSequences[address.raw];
    if (stored == seq) {
        return;
    }
    stored = seq;
    _sequenceStateDirty = true;

    // Write through to the array property. §6.3.8.4: after decrypting an
    // S-A_Data-PDU the device "shall update the field Last Valid SeqNr for that
    // IA in the Security Individual Address Table". Keeping the table current
    // is also what makes the value durable — the table is a persisted property,
    // the live map is not.
    auto it = _extraProperties.find(SecurityProperty::SecurityIndividualAddressTable);
    if (it != _extraProperties.end()) {
        (void)security_ia_table::setSequenceFor(it->second, address, seq);
    }
}

void SecurityInterfaceObject::syncSequencesFromAddressTable() {
    std::lock_guard<std::mutex> lock(_sequenceMutex);

    const auto it = _extraProperties.find(SecurityProperty::SecurityIndividualAddressTable);
    if (it == _extraProperties.end()) {
        return;
    }

    const std::span<const uint8_t> blob(it->second);
    const size_t count = security_ia_table::entryCount(blob);
    for (size_t index = 0; index < count; ++index) {
        const IndividualAddress address = security_ia_table::addressAt(blob, index);
        const uint64_t stored = security_ia_table::sequenceAt(blob, index);
        auto& live = _peerSequences[address.raw];
        // A restored table and a running device can both be ahead: take the
        // higher value so a restore never reopens an already-closed replay
        // window, and a stale ETS download never rolls the window backwards.
        if (stored > live) {
            live = stored;
        } else if (live > stored) {
            (void)security_ia_table::setSequenceFor(it->second, address, live);
        }
    }
}

bool SecurityInterfaceObject::sequenceStateDirty() const {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    return _sequenceStateDirty;
}

void SecurityInterfaceObject::clearSequenceStateDirty() {
    std::lock_guard<std::mutex> lock(_sequenceMutex);
    _sequenceStateDirty = false;
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
struct BlobPropertyInfo {
    SecurityProperty prop;
    uint8_t elementSize;
};

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
    auto* domain = static_cast<SecurityInterfaceObject*>(context.domain);
    if (!domain) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    std::vector<uint8_t> data(7, 0);
    auto res = in.readBytes(data);
    if (res.isError()) {
        return res;
    }
    domain->upsertExtraProperty(SecurityProperty::SecurityFailuresLog) = std::move(data);
    return util::Result<void>::ok();
}

util::Result<void> readBlobProperty(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteWriter& out,
    const void* userData)
{
    const auto* domain = static_cast<const SecurityInterfaceObject*>(context.domain);
    const auto* info = static_cast<const BlobPropertyInfo*>(userData);
    if (!domain || !info) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const auto* dataPtr = domain->findExtraProperty(info->prop);
    const size_t offset = static_cast<size_t>(startIndex.value) * info->elementSize;
    const size_t count = static_cast<size_t>(elementCount) * info->elementSize;

    if (!dataPtr) {
        std::vector<uint8_t> zeros(count, 0);
        return out.writeBytes(zeros);
    }

    const auto& data = *dataPtr;
    if (offset + count > data.size()) {
        return util::Result<void>::err(util::ErrorCode::OutOfRange);
    }
    return out.writeBytes(std::span<const uint8_t>(data).subspan(offset, count));
}

util::Result<void> writeBlobProperty(
    const PropertyContext& context,
    DomainIndex startIndex,
    uint16_t elementCount,
    util::ByteReader& in,
    const void* userData)
{
    auto* domain = static_cast<SecurityInterfaceObject*>(context.domain);
    const auto* info = static_cast<const BlobPropertyInfo*>(userData);
    if (!domain || !info) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const size_t offset = static_cast<size_t>(startIndex.value) * info->elementSize;
    const size_t count = static_cast<size_t>(elementCount) * info->elementSize;
    std::vector<uint8_t> chunk(count, 0);
    auto res = in.readBytes(chunk);
    if (res.isError()) {
        return res;
    }

    auto& data = domain->upsertExtraProperty(info->prop);
    if (offset > data.size()) {
        data.resize(offset, 0);
    }
    if (offset + count > data.size()) {
        data.resize(offset + count, 0);
    }
    std::copy(chunk.begin(), chunk.end(), data.begin() + offset);
    return util::Result<void>::ok();
}

/// Current number of elements of a blob-backed array property: how much of the
/// table ETS has downloaded, not how much it could download.
util::Result<uint16_t> countBlobProperty(const PropertyContext& context, const void* userData)
{
    const auto* domain = static_cast<const SecurityInterfaceObject*>(context.domain);
    const auto* info = static_cast<const BlobPropertyInfo*>(userData);
    if (!domain || !info || info->elementSize == 0) {
        return util::Result<uint16_t>(util::ErrorCode::InvalidParameter);
    }
    const auto* data = domain->findExtraProperty(info->prop);
    if (!data) {
        return util::Result<uint16_t>(static_cast<uint16_t>(0));
    }
    return util::Result<uint16_t>(static_cast<uint16_t>(data->size() / info->elementSize));
}

/// Set the current number of elements. ETS shortens a table this way before
/// re-downloading it — "OT=17 PID=54 nr_of_elem=1 start_index=0 data=0000" is
/// literally "clear the Security Individual Address Table". Growing is allowed
/// too and zero-fills, so a subsequent element write lands in a defined array.
util::Result<void> resizeBlobProperty(const PropertyContext& context,
                                      uint16_t elementCount,
                                      const void* userData)
{
    auto* domain = static_cast<SecurityInterfaceObject*>(context.domain);
    const auto* info = static_cast<const BlobPropertyInfo*>(userData);
    if (!domain || !info || info->elementSize == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    // An empty table is a real state, not an absent one: the persistence layer
    // turns the empty vector into an erase, so the previous download's entries
    // cannot come back at the next boot.
    auto& data = domain->upsertExtraProperty(info->prop);
    data.assign(static_cast<size_t>(elementCount) * info->elementSize, 0);
    return util::Result<void>::ok();
}

static const ScalarPropertyData<SecurityInterfaceObject, uint16_t> kObjectTypeData{ &getObjectType, nullptr };
static const ScalarPropertyData<SecurityInterfaceObject, uint8_t> kSecurityModeData{ &getSecurityMode, &setSecurityMode };

static const BlobPropertyInfo kP2PKeyInfo{ SecurityProperty::P2PKeyTable, 20 };
static const BlobPropertyInfo kGroupKeyInfo{ SecurityProperty::GroupKeyTable, 18 };
static const BlobPropertyInfo kIndividualAddrInfo{ SecurityProperty::SecurityIndividualAddressTable, 8 };
static const BlobPropertyInfo kSecurityReportInfo{ SecurityProperty::SecurityReport, 1 };
static const BlobPropertyInfo kSecurityReportControlInfo{ SecurityProperty::SecurityReportControl, 1 };
static const BlobPropertyInfo kZoneKeyInfo{ SecurityProperty::ZoneKeyTable, 19 };
static const BlobPropertyInfo kGoSecurityFlagsInfo{ SecurityProperty::GoSecurityFlags, 1 };

static const PropertyHandler kSecurityHandlers[] = {
    ScalarProperty<SecurityInterfaceObject, uint16_t>::make(
        application::PropertyID::ObjectType,
        application::PropertyDataType::UnsignedInt,
        PropertyCapability::ReadOnly,
        &kObjectTypeData),

    // PID_LOAD_STATE_CONTROL: ETS runs the standard load procedure
    // (StartLoading -> write key tables -> LoadCompleted) before it will
    // download Data Secure key material. Without this property the download
    // aborts at the first step.
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
        0xFFFF,
        0,
        &readBlobProperty,
        &writeBlobProperty,
        &kP2PKeyInfo,
        &countBlobProperty,
        &resizeBlobProperty
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::GroupKeyTable),
        application::PropertyDataType::Generic18,
        PropertyCapability::ReadWrite,
        0xFFFF,
        0,
        &readBlobProperty,
        &writeBlobProperty,
        &kGroupKeyInfo,
        &countBlobProperty,
        &resizeBlobProperty
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::SecurityIndividualAddressTable),
        application::PropertyDataType::Generic08,
        PropertyCapability::ReadWrite,
        0xFFFF,
        0,
        &readBlobProperty,
        &writeBlobProperty,
        &kIndividualAddrInfo,
        &countBlobProperty,
        &resizeBlobProperty
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::SecurityReport),
        application::PropertyDataType::Bitset8,
        PropertyCapability::ReadWrite,
        1,
        0,
        &readBlobProperty,
        &writeBlobProperty,
        &kSecurityReportInfo
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::SecurityReportControl),
        application::PropertyDataType::Bitset8,
        PropertyCapability::ReadWrite,
        1,
        0,
        &readBlobProperty,
        &writeBlobProperty,
        &kSecurityReportControlInfo
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::ZoneKeyTable),
        application::PropertyDataType::Generic19,
        PropertyCapability::ReadWrite,
        0xFFFF,
        0,
        &readBlobProperty,
        &writeBlobProperty,
        &kZoneKeyInfo,
        &countBlobProperty,
        &resizeBlobProperty
    },

    {
        static_cast<application::PropertyID>(SecurityProperty::GoSecurityFlags),
        application::PropertyDataType::GenericData,
        PropertyCapability::ReadWrite,
        0xFFFF,
        1,
        &readBlobProperty,
        &writeBlobProperty,
        &kGoSecurityFlagsInfo,
        &countBlobProperty,
        &resizeBlobProperty
    }
};

constexpr size_t kSecurityHandlerCount = sizeof(kSecurityHandlers) / sizeof(kSecurityHandlers[0]);
} // namespace

KernelBinding SecurityInterfaceObject::kernelBinding() const {
    static bool validated = false;
    if (!validated) {
        const auto validation = validatePropertyTable(kSecurityHandlers, kSecurityHandlerCount);
        if (validation.isError()) {
            KNX_LOGE("KNX.Security", "Invalid property handler table (err=%d)", static_cast<int>(validation.error()));
        }
        validated = true;
    }
    KernelBinding binding;
    binding.handlers = kSecurityHandlers;
    binding.handlerCount = kSecurityHandlerCount;
    binding.context = PropertyContext{const_cast<SecurityInterfaceObject*>(this), _validationPolicy};
    return binding;
}

// === Bulk Operations ===

void SecurityInterfaceObject::clearAllKeys() {
    _toolKey.fill(0);
    _deviceKeys.clear();
    _groupKeys.clear();
    KNX_LOGI("KNX.Security", "All security keys cleared");
}

void SecurityInterfaceObject::loadControlReset() {
    // Tool key intentionally preserved — see the declaration for why.
    _deviceKeys.clear();
    _groupKeys.clear();
    _extraProperties.erase(SecurityProperty::P2PKeyTable);
    _extraProperties.erase(SecurityProperty::GroupKeyTable);
    _extraProperties.erase(SecurityProperty::SecurityIndividualAddressTable);
    _extraProperties.erase(SecurityProperty::ZoneKeyTable);
    _extraProperties.erase(SecurityProperty::GoSecurityFlags);
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

const std::vector<uint8_t>* SecurityInterfaceObject::findExtraProperty(SecurityProperty prop) const {
    auto it = _extraProperties.find(prop);
    if (it == _extraProperties.end()) {
        return nullptr;
    }
    return &it->second;
}

std::vector<uint8_t>& SecurityInterfaceObject::upsertExtraProperty(SecurityProperty prop) {
    return _extraProperties[prop];
}

// === Helper Methods ===

bool SecurityInterfaceObject::isKeyNonZero(const std::array<uint8_t, 16>& key) {
    return std::any_of(key.begin(), key.end(), [](uint8_t b) { return b != 0; });
}

// === InterfaceObject implementation ===

} // namespace objects
} // namespace knx
