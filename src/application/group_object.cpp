// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file group_object.cpp
 * @brief Group object implementation
 */

#include "knx/application/group_object.hpp"
#include "knx/config.hpp"
#include "knx/util/log.hpp"

#include <algorithm>
#include <array>

static const char* TAG [[maybe_unused]] = "KNX.GroupObject";

namespace knx {
namespace application {

namespace {

template <typename EncodeFn>
util::Result<void> encodeGroupObjectValue(EncodeFn&& encode, RawValueBuffer& out)
{
    // Same bound as the object's own storage: encoding into a 254-byte stack
    // buffer only to copy at most 16 bytes out of it wastes half a kilobyte of
    // stack on every setValue() call.
    std::array<uint8_t, config::MAX_GROUP_OBJECT_PAYLOAD_BYTES> buffer{};
    auto encodeResult = encode(std::span<uint8_t>(buffer));
    if (encodeResult.isError()) {
        return encodeResult.error();
    }

    if (!out.assign(std::span<const uint8_t>(buffer.data(), encodeResult.value()))) {
        return util::ErrorCode::BufferTooSmall;
    }
    return util::Result<void>::ok();
}

util::Result<void> storeGroupObjectValue(RawValueBuffer& currentValue,
                                         bool& valid,
                                         const GroupAddress& address,
                                         const GroupObjectCallback& callback,
                                         std::span<const uint8_t> data)
{
    if (!currentValue.assign(data)) {
        return util::ErrorCode::BufferTooSmall;
    }
    valid = true;

    if (callback) {
        callback(address, data);
    }

    return util::Result<void>::ok();
}

} // namespace

GroupObject::GroupObject(const GroupObjectConfig& config)
    : _config(config)
    , _valid(false)
    , _callback(nullptr)
{
}

GroupObject::~GroupObject() = default;

bool GroupObject::asBool() const {
    if (!_valid || _rawValue.empty()) return false;
    return (_rawValue[0] & 0x01) != 0;
}

uint8_t GroupObject::asUint8() const {
    if (!_valid || _rawValue.empty()) return 0;
    return _rawValue[0];
}

float GroupObject::asFloat() const {
    if (!_valid || _rawValue.size() < 2) return 0.0f;
    float result = 0.0f;
    if (Dpt9::decode(_rawValue, result).isError()) {
        return 0.0f;
    }
    return result;
}

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
    RawValueBuffer data;
    auto encodeResult = encodeGroupObjectValue(
        [this, &value](std::span<uint8_t> out) { return encodeDptValue(_config.dpt, value, out); },
        data);
    if (encodeResult.isError()) {
        return encodeResult.error();
    }

    return storeGroupObjectValue(_rawValue, _valid, _config.address, _callback, data);
}

util::Result<DptValue> GroupObject::value() const {
    if (!_valid) {
        return util::ErrorCode::NotInitialized;
    }

    return decodeDptValue(_config.dpt, _rawValue.span());
}

uint8_t GroupObject::valueFieldType() const noexcept {
    const uint16_t bits = dptPayloadBits(_config.dpt);
    if (bits == 0) {
        // Unknown DPT: fall back to the width actually being carried, so a
        // raw-payload object still produces a descriptor ETS can parse.
        const size_t octets = _rawValue.size();
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
                 _config.address.raw,
                 source == ValueSource::Write ? "write" : "response",
                 _config.flags.communication ? 1 : 0,
                 _config.flags.write ? 1 : 0,
                 _config.flags.update ? 1 : 0);
        return util::ErrorCode::AccessDenied;
    }

    auto storeResult = storeGroupObjectValue(_rawValue, _valid, _config.address, _callback, data);
    if (storeResult.isError()) {
        return storeResult;
    }
    
    KNX_LOGD(TAG, "Group object 0x%04X updated", _config.address.raw);
    
    return util::Result<void>::ok();
}

void GroupObject::setCallback(GroupObjectCallback callback) {
    _callback = callback;
}

void GroupObject::setReadRequestSender(GroupReadRequestCallback callback) {
    _readRequestSender = callback;
}

util::Result<void> GroupObject::requestValue() {
    // Sending A_GroupValue_Read asks *another* device for its value, so it is
    // not gated by our own Read enable (R governs answering reads, not issuing
    // them).  Only Communication enable applies — otherwise a write-only object
    // could never use Value-Read-on-Initialisation.
    if (!_config.flags.communication) {
        KNX_LOGW(TAG, "Group object 0x%04X has communication disabled", _config.address.raw);
        return util::ErrorCode::AccessDenied;
    }

    // Delegate to BAU/facade if available.
    if (_readRequestSender) {
        return _readRequestSender(_config.address);
    }

    KNX_LOGW(TAG, "No read request sender wired; request not sent");
    return util::ErrorCode::OperationFailed;
}

bool GroupObject::changedSignificantly() const {
    if (!_lastSentValid) {
        return true;
    }
    // Byte-identical values never count as a change (also the cheapest test).
    if (_rawValue.size() == _lastSentRaw.size()
        && std::equal(_rawValue.begin(), _rawValue.end(), _lastSentRaw.begin())) {
        return false;
    }

    const double threshold = _config.transmitPolicy.changeThreshold;
    if (threshold <= 0.0) {
        return true;  // any change counts
    }

    // Numeric DPTs: compare decoded engineering magnitudes against the threshold.
    // Non-numeric types (or a decode failure) fall back to "changed" since the
    // raw bytes already differ.
    auto current = decodeDptValue(_config.dpt, _rawValue.span());
    auto previous = decodeDptValue(_config.dpt, _lastSentRaw.span());
    if (current.isError() || previous.isError()) {
        return true;
    }
    double cur = 0.0;
    double prev = 0.0;
    if (!dptValueAsScalar(current.value(), cur) || !dptValueAsScalar(previous.value(), prev)) {
        return true;
    }
    const double delta = (cur >= prev) ? (cur - prev) : (prev - cur);
    return delta >= threshold;
}

PublishDecision GroupObject::decidePublish(uint32_t nowMs) const {
    const auto& policy = _config.transmitPolicy;

    // First transmission always goes out (establishes the baseline value/time).
    if (!_lastSentValid) {
        return PublishDecision::Send;
    }
    if (policy.onChangeEnabled && !changedSignificantly()) {
        return PublishDecision::Suppress;
    }
    if (policy.minIntervalMs != 0u && (nowMs - _lastSentMs) < policy.minIntervalMs) {
        return PublishDecision::Defer;
    }
    return PublishDecision::Send;
}

bool GroupObject::dueForCyclic(uint32_t nowMs) const noexcept {
    const auto& policy = _config.transmitPolicy;
    return policy.cyclicIntervalMs != 0u
        && _lastSentValid
        && (nowMs - _lastSentMs) >= policy.cyclicIntervalMs;
}

bool GroupObject::readyToSendDeferred(uint32_t nowMs) const noexcept {
    if (!_pendingSend) {
        return false;
    }
    const auto& policy = _config.transmitPolicy;
    return policy.minIntervalMs == 0u
        || !_lastSentValid
        || (nowMs - _lastSentMs) >= policy.minIntervalMs;
}

void GroupObject::noteTransmitted(uint32_t nowMs) {
    (void)_lastSentRaw.assign(_rawValue.span());
    _lastSentValid = true;
    _lastSentMs = nowMs;
    _pendingSend = false;
}

} // namespace application
} // namespace knx
