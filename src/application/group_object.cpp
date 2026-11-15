// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file group_object.cpp
 * @brief Group object and group object store implementation
 */

#include "knx/application/group_object.hpp"
#include "knx/config.hpp"
#include "knx/util/log.hpp"

#include <algorithm>
#include <array>
#include <cstring>

static const char* TAG [[maybe_unused]] = "KNX.GroupObject";

namespace knx {
namespace application {

namespace {

constexpr uint16_t kNoIndex = 0xFFFFu;

bool isInertPolicy(const GroupObjectTransmitPolicy& p)
{
    return !p.onChangeEnabled && p.changeThreshold == 0 && p.cyclicIntervalMs == 0u && p.minIntervalMs == 0u;
}

} // namespace

// ─── GroupObjectStore ────────────────────────────────────────────────────────

GroupObjectStore::~GroupObjectStore() = default;

void GroupObjectStore::reserve(uint16_t objects, size_t valueBytes)
{
    if (objects > _capacity) {
        std::unique_ptr<GroupObject[]> grown(new GroupObject[objects]);
        for (uint16_t i = 0; i < _count; ++i) {
            grown[i].copyFrom(_entries[i]);
        }
        _entries = std::move(grown);
        _capacity = objects;
    }
    if (valueBytes > _pool.capacity()) {
        _pool.reserve(valueBytes);
    }
}

uint16_t GroupObjectStore::allocateValue(size_t bytes)
{
    const size_t offset = _pool.size();
    if (_pool.capacity() < offset + bytes) {
        _pool.reserve(offset + bytes);  // exact: the pool is sized from the DPTs, not doubled
    }
    _pool.resize(offset + bytes, 0u);
    return static_cast<uint16_t>(offset);
}

uint16_t GroupObjectStore::indexOf(const GroupObject* object) const noexcept
{
    if (_entries && object >= _entries.get() && object < _entries.get() + _count) {
        return static_cast<uint16_t>(object - _entries.get());
    }
    return kNoIndex;
}

uint8_t GroupObjectStore::policySlot(GroupObject& object)
{
    if (object._policy != GroupObject::kNoPolicy) {
        return object._policy;
    }
    if (_policies.size() >= GroupObject::kNoPolicy) {
        return GroupObject::kNoPolicy;
    }
    PolicyState state{};
    state.lastSentOffset = allocateValue(groupObjectValueBytes(object._dpt));
    if (_policies.capacity() == _policies.size()) {
        _policies.reserve(_policies.size() + 1u);
    }
    _policies.push_back(state);
    object._policy = static_cast<uint8_t>(_policies.size() - 1u);
    return object._policy;
}

GroupObject* GroupObjectStore::append(const GroupObjectConfig& config)
{
    if (_count >= kMaxObjects) {
        return nullptr;
    }
    if (_count == _capacity) {
        reserve(static_cast<uint16_t>(std::min<size_t>(kMaxObjects, _capacity + 8u)), 0u);
    }
    GroupObject* object = &_entries[_count];
    object->init(this, config);
    ++_count;
    return object;
}

GroupObject* GroupObjectStore::appendCopy(const GroupObject& source)
{
    GroupObjectConfig config{};
    config.address = source.getAddress();
    config.dpt = source.dptId();
    config.flags = source.flags();
    config.transmitPolicy = source.transmitPolicy();
    GroupObject* object = append(config);
    if (object == nullptr) {
        return nullptr;
    }

    const auto value = source.getRawValue();
    std::copy(value.begin(), value.end(), _pool.begin() + object->_valueOffset);
    object->_valueLength = static_cast<uint8_t>(value.size());
    object->setBit(GroupObject::kBitValid, source.isValid());
    object->setBit(GroupObject::kBitPending, source.pendingSend());

    if (const PolicyState* from = source.policyState(); from != nullptr && from->lastSentValid) {
        if (policySlot(*object) != GroupObject::kNoPolicy) {
            PolicyState& to = _policies[object->_policy];
            std::copy(source._store->_pool.begin() + from->lastSentOffset,
                      source._store->_pool.begin() + from->lastSentOffset + from->lastSentLength,
                      _pool.begin() + to.lastSentOffset);
            to.lastSentLength = from->lastSentLength;
            to.lastSentMs = from->lastSentMs;
            to.lastSentValid = true;
        }
    }
    return object;
}

bool GroupObjectStore::erase(uint16_t index)
{
    if (index >= _count) {
        return false;
    }
    for (uint16_t i = index; i + 1u < _count; ++i) {
        _entries[i].copyFrom(_entries[i + 1u]);
    }
    --_count;
    return true;
}

void GroupObjectStore::clear()
{
    _count = 0;
    _pool.clear();
    _policies.clear();
}

// ─── GroupObject ─────────────────────────────────────────────────────────────

void GroupObject::init(GroupObjectStore* store, const GroupObjectConfig& config)
{
    _store = store;
    _dpt = config.dpt;
    _address = config.address;
    _bits = 0;
    _policy = kNoPolicy;
    _valueLength = 0;
    setFlags(config.flags);
    _valueOffset = store->allocateValue(groupObjectValueBytes(config.dpt));
    if (!isInertPolicy(config.transmitPolicy)) {
        (void)setTransmitPolicy(config.transmitPolicy);
    }
}

void GroupObject::copyFrom(const GroupObject& other) noexcept
{
    _store = other._store;
    _dpt = other._dpt;
    _address = other._address;
    _bits = static_cast<uint16_t>(other._bits & ~(1u << kBitOwnsStore));
    _valueOffset = other._valueOffset;
    _valueLength = other._valueLength;
    _policy = other._policy;
}

GroupObject::GroupObject(const GroupObjectConfig& config)
{
    auto* store = new GroupObjectStore();
    init(store, config);
    setBit(kBitOwnsStore, true);
}

GroupObject::~GroupObject()
{
    if (bit(kBitOwnsStore)) {
        delete _store;
    }
}

void GroupObject::setFlags(const GroupObjectFlags& flags) noexcept
{
    const uint16_t keep = static_cast<uint16_t>(_bits & ~((1u << kBitComm) | (1u << kBitRead) | (1u << kBitWrite)
                                                           | (1u << kBitTransmit) | (1u << kBitUpdate)
                                                           | (1u << kBitReadOnInit) | (3u << kPrioShift)));
    _bits = static_cast<uint16_t>(keep
        | (flags.communication ? (1u << kBitComm) : 0u)
        | (flags.read ? (1u << kBitRead) : 0u)
        | (flags.write ? (1u << kBitWrite) : 0u)
        | (flags.transmit ? (1u << kBitTransmit) : 0u)
        | (flags.update ? (1u << kBitUpdate) : 0u)
        | (flags.readOnInit ? (1u << kBitReadOnInit) : 0u)
        | ((static_cast<unsigned>(flags.priority) & 3u) << kPrioShift));
}

GroupObjectIndex GroupObject::index() const noexcept
{
    const uint16_t i = _store->indexOf(this);
    return i == kNoIndex ? GroupObjectIndex::invalid() : GroupObjectIndex(i);
}

std::span<const uint8_t> GroupObject::getRawValue() const noexcept
{
    return std::span<const uint8_t>(_store->_pool.data() + _valueOffset, _valueLength);
}

PolicyState* GroupObject::policyState() noexcept
{
    return _policy == kNoPolicy ? nullptr : &_store->_policies[_policy];
}

const PolicyState* GroupObject::policyState() const noexcept
{
    return _policy == kNoPolicy ? nullptr : &_store->_policies[_policy];
}

bool GroupObject::asBool() const {
    const auto raw = getRawValue();
    if (!isValid() || raw.empty()) return false;
    return (raw[0] & 0x01) != 0;
}

uint8_t GroupObject::asUint8() const {
    const auto raw = getRawValue();
    if (!isValid() || raw.empty()) return 0;
    return raw[0];
}

#if KNX_FEATURE_DPT_FLOAT
float GroupObject::asFloat() const {
    const auto raw = getRawValue();
    if (!isValid() || raw.size() < 2) return 0.0f;
    float result = 0.0f;
    if (Dpt9::decode(raw, result).isError()) {
        return 0.0f;
    }
    return result;
}
#endif

util::Result<void> GroupObject::setValue(bool value) {
    return setValue(DptValue(value));
}

util::Result<void> GroupObject::setValue(uint8_t value) {
    return setValue(DptValue(value));
}

util::Result<void> GroupObject::setValue(float value) {
    return setValue(DptValue(value));
}

util::Result<void> GroupObject::setValue(const DptValue& value) {
    // Same bound as the object's own storage: encoding into a 254-byte stack
    // buffer only to copy at most 16 bytes out of it wastes half a kilobyte of
    // stack on every setValue() call.
    std::array<uint8_t, config::MAX_GROUP_OBJECT_PAYLOAD_BYTES> buffer{};
    auto encodeResult = encodeDptValue(_dpt, value, std::span<uint8_t>(buffer));
    if (encodeResult.isError()) {
        return encodeResult.error();
    }
    return storeValue(std::span<const uint8_t>(buffer.data(), encodeResult.value()));
}

util::Result<void> GroupObject::storeValue(std::span<const uint8_t> data)
{
    // The object owns exactly the bytes its datapoint type needs.
    if (data.size() > groupObjectValueBytes(_dpt)) {
        return util::ErrorCode::BufferTooSmall;
    }
    std::copy(data.begin(), data.end(), _store->_pool.begin() + _valueOffset);
    _valueLength = static_cast<uint8_t>(data.size());
    setBit(kBitValid, true);

    if (_store->_onValue) {
        _store->_onValue(index(), data);
    }
    return util::Result<void>::ok();
}

util::Result<DptValue> GroupObject::value() const {
    if (!isValid()) {
        return util::ErrorCode::NotInitialized;
    }

    return decodeDptValue(_dpt, getRawValue());
}

uint8_t GroupObject::valueFieldType() const noexcept {
    const uint16_t bits = dptPayloadBits(_dpt);
    if (bits == 0) {
        // Unknown DPT: fall back to the width actually being carried, so a
        // raw-payload object still produces a descriptor ETS can parse.
        const size_t octets = _valueLength;
        return octets == 0 ? valueFieldTypeForOctets(1) : valueFieldTypeForOctets(octets);
    }
    if (bits < 8) {
        return valueFieldTypeForBits(bits);
    }
    return valueFieldTypeForOctets(static_cast<size_t>(bits) / 8u);
}

util::Result<void> GroupObject::receiveValue(std::span<const uint8_t> data, ValueSource source) {
    // Write enable (W) governs A_GroupValue_Write; Response-Update enable (U)
    // governs A_GroupValue_Response.  Communication enable gates both, and is
    // already folded into writable()/updatable().
    const bool accepted = (source == ValueSource::Write) ? writable() : updatable();
    if (!accepted) {
        KNX_LOGW(TAG, "Group object 0x%04X rejects %s (flags C=%d W=%d U=%d)",
                 _address.raw,
                 source == ValueSource::Write ? "write" : "response",
                 bit(kBitComm) ? 1 : 0,
                 bit(kBitWrite) ? 1 : 0,
                 bit(kBitUpdate) ? 1 : 0);
        return util::ErrorCode::AccessDenied;
    }

    auto storeResult = storeValue(data);
    if (storeResult.isError()) {
        return storeResult;
    }

    KNX_LOGD(TAG, "Group object 0x%04X updated", _address.raw);

    return util::Result<void>::ok();
}

util::Result<void> GroupObject::requestValue() {
    // Sending A_GroupValue_Read asks *another* device for its value, so it is
    // not gated by our own Read enable (R governs answering reads, not issuing
    // them).  Only Communication enable applies — otherwise a write-only object
    // could never use Value-Read-on-Initialisation.
    if (!bit(kBitComm)) {
        KNX_LOGW(TAG, "Group object 0x%04X has communication disabled", _address.raw);
        return util::ErrorCode::AccessDenied;
    }

    // Delegate to BAU/facade if available.
    if (_store->_readRequestSender) {
        return _store->_readRequestSender(_address);
    }

    KNX_LOGW(TAG, "No read request sender wired; request not sent");
    return util::ErrorCode::OperationFailed;
}

util::Result<void> GroupObject::setTransmitPolicy(const GroupObjectTransmitPolicy& policy)
{
    if (_policy == kNoPolicy && isInertPolicy(policy)) {
        return util::Result<void>::ok();  // the inert default needs no state
    }
    if (_store->policySlot(*this) == kNoPolicy) {
        return util::ErrorCode::ResourceUnavailable;
    }
    _store->_policies[_policy].policy = policy;
    return util::Result<void>::ok();
}

GroupObjectTransmitPolicy GroupObject::transmitPolicy() const noexcept
{
    const PolicyState* state = policyState();
    return state != nullptr ? state->policy : GroupObjectTransmitPolicy{};
}

bool GroupObject::changedSignificantly(const PolicyState& state) const {
    if (!state.lastSentValid) {
        return true;
    }
    const std::span<const uint8_t> current = getRawValue();
    const std::span<const uint8_t> last(_store->_pool.data() + state.lastSentOffset, state.lastSentLength);
    // Byte-identical values never count as a change (also the cheapest test).
    if (current.size() == last.size() && std::equal(current.begin(), current.end(), last.begin())) {
        return false;
    }

#if KNX_FEATURE_DPT_FLOAT
    const double threshold = state.policy.changeThreshold;
    if (threshold <= 0.0) {
        return true;  // any change counts
    }

    // Numeric DPTs: compare decoded engineering magnitudes against the threshold.
    // Non-numeric types (or a decode failure) fall back to "changed" since the
    // raw bytes already differ.
    auto cur = decodeDptValue(_dpt, current);
    auto prev = decodeDptValue(_dpt, last);
    if (cur.isError() || prev.isError()) {
        return true;
    }
    double curScalar = 0.0;
    double prevScalar = 0.0;
    if (!dptValueAsScalar(cur.value(), curScalar) || !dptValueAsScalar(prev.value(), prevScalar)) {
        return true;
    }
    const double delta = (curScalar >= prevScalar) ? (curScalar - prevScalar) : (prevScalar - curScalar);
    return delta >= threshold;
#else
    const uint32_t threshold = state.policy.changeThreshold;
    if (threshold == 0u) {
        return true;  // any change counts
    }

    // Integer DPTs: compare the decoded values against the integer threshold.
    // Anything else (or a decode failure) counts as changed, since the raw
    // bytes already differ.
    auto cur = decodeDptValue(_dpt, current);
    auto prev = decodeDptValue(_dpt, last);
    if (cur.isError() || prev.isError()) {
        return true;
    }
    int64_t curInt = 0;
    int64_t prevInt = 0;
    if (!dptValueAsInteger(cur.value(), curInt) || !dptValueAsInteger(prev.value(), prevInt)) {
        return true;
    }
    const uint64_t delta = static_cast<uint64_t>(curInt >= prevInt ? curInt - prevInt : prevInt - curInt);
    return delta >= threshold;
#endif
}

PublishDecision GroupObject::decidePublish(uint32_t nowMs) const {
    // An object without a policy state has the inert default policy and no
    // transmit history: every publish goes out.
    const PolicyState* state = policyState();
    if (state == nullptr) {
        return PublishDecision::Send;
    }
    const auto& policy = state->policy;

    // First transmission always goes out (establishes the baseline value/time).
    if (!state->lastSentValid) {
        return PublishDecision::Send;
    }
    if (policy.onChangeEnabled && !changedSignificantly(*state)) {
        return PublishDecision::Suppress;
    }
    if (policy.minIntervalMs != 0u && (nowMs - state->lastSentMs) < policy.minIntervalMs) {
        return PublishDecision::Defer;
    }
    return PublishDecision::Send;
}

bool GroupObject::dueForCyclic(uint32_t nowMs) const noexcept {
    const PolicyState* state = policyState();
    return state != nullptr
        && state->policy.cyclicIntervalMs != 0u
        && state->lastSentValid
        && (nowMs - state->lastSentMs) >= state->policy.cyclicIntervalMs;
}

bool GroupObject::readyToSendDeferred(uint32_t nowMs) const noexcept {
    if (!pendingSend()) {
        return false;
    }
    const PolicyState* state = policyState();
    return state == nullptr
        || state->policy.minIntervalMs == 0u
        || !state->lastSentValid
        || (nowMs - state->lastSentMs) >= state->policy.minIntervalMs;
}

void GroupObject::noteTransmitted(uint32_t nowMs) {
    setPendingSend(false);
    PolicyState* state = policyState();
    if (state == nullptr) {
        return;
    }
    const auto value = getRawValue();
    std::copy(value.begin(), value.end(), _store->_pool.begin() + state->lastSentOffset);
    state->lastSentLength = static_cast<uint8_t>(value.size());
    state->lastSentValid = true;
    state->lastSentMs = nowMs;
}

} // namespace application
} // namespace knx
