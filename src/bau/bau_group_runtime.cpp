// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bau_group_runtime.cpp
 * @brief BusAccessUnit group-object runtime.
 *
 * Owns everything about group communication: the object table surface,
 * outbound publish/respond with transmit shaping, and the inbound event queue
 * that marshals telegrams from the lower-layer callback context onto the owner
 * context. Split out of bau.cpp, where it was interleaved with lower-stack
 * assembly and device lifecycle.
 *
 * @thread_safety Owner context only, except enqueueInboundGroupEvent() and
 * handleApplicationFrame(), which are the two entry points called from the
 * lower-layer context and do nothing but copy into the queue.
 */

#include "knx/bau/bau.hpp"
#include "bau_internal.hpp"
#include "knx/objects/interface_object.hpp"
#include "knx/objects/interface_object_manager.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/application/property_store.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/platform/raii_resources.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/testing/mock_tp1_physical.hpp"
#include "knx/security/data_secure.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace knx {
namespace bau {

using StackPort = BusAccessStackPort;

using detail::kMaxInboundAssociatedGroupObjects;
using detail::kMaxInboundGroupEventsPerLoop;
using detail::PendingInboundGroupEvent;

namespace {
constexpr const char* TAG = "KNX.BAU";
}

application::GroupObject* BusAccessUnit::groupObject(GroupObjectIndex index) {
    return _groupObjectTable.getGroupObject(index);
}

const application::GroupObject* BusAccessUnit::groupObject(GroupObjectIndex index) const {
    return _groupObjectTable.getGroupObject(index);
}

GroupObjectIndex BusAccessUnit::addGroupObject(const GroupAddress& address,
                                               application::DptId dpt,
                                               bool readable,
                                               bool writable,
                                               bool transmit,
                                               bool receivable) {
    application::GroupObjectFlags flags{};
    flags.communication = true;
    flags.read = readable;
    flags.write = writable;
    flags.transmit = transmit;
    flags.update = receivable;
    flags.readOnInit = false;
    flags.priority = Priority::Low;
    return addGroupObject(address, dpt, flags);
}

GroupObjectIndex BusAccessUnit::addGroupObject(const GroupAddress& address,
                                               application::DptId dpt,
                                               const application::GroupObjectFlags& flags) {
    application::GroupObjectConfig config{};
    config.address = address;
    config.dpt = dpt;
    config.flags = flags;

    auto obj = std::make_unique<application::GroupObject>(config);
    // Wire read request sender to BAU
    obj->setReadRequestSender([this](const GroupAddress& addr) {
        return this->requestGroupValue(addr);
    });

    GroupObjectIndex index = _groupObjectTable.addGroupObject(std::move(obj));
    if (!index.isValid()) {
        return GroupObjectIndex::invalid();
    }

    // Subscribe to group address in data link layer
    if (_stackPort) {
        (void)_stackPort->addGroupAddress(config.address);
    }

    // Add to address table
    _addressTable.addEntry(config.address);

    // Add to association table so the three-table routing chain resolves this
    // object for code-driven GA assignments as well.
    {
        const AddressTableIndex addrIdx = _addressTable.findIndex(config.address);
        if (addrIdx.isValid() && !_associationTable.hasAssociation(addrIdx, index)) {
            (void)_associationTable.addEntry({addrIdx, index});
        }
    }

    return index;
}

util::Result<void> BusAccessUnit::bindGroupObjectToAddress(GroupObjectIndex index,
                                                           const GroupAddress& address) {
    if (!address.isValid()) {
        return util::ErrorCode::InvalidAddress;
    }

    if (!groupObject(index)) {
        return util::ErrorCode::InvalidAddress;
    }

    AddressTableIndex addressIndex = _addressTable.findIndex(address);
    if (!addressIndex.isValid()) {
        addressIndex = _addressTable.addEntry(address);
        if (!addressIndex.isValid()) {
            return util::ErrorCode::ResourceUnavailable;
        }
    }

    if (!_associationTable.hasAssociation(addressIndex, index)) {
        const auto addAssociation = _associationTable.addEntry({addressIndex, index});
        if (addAssociation.isError()) {
            return addAssociation.error();
        }
    }

    if (_stackPort) {
        const auto addGroupAddress = _stackPort->addGroupAddress(address);
        if (addGroupAddress.isError()) {
            return addGroupAddress.error();
        }
    }

    return util::Result<void>::ok();
}

util::Result<void> BusAccessUnit::setGroupObjectValue(GroupObjectIndex index, const application::DptValue& value) {
    auto* obj = groupObject(index);
    if (!obj) {
        return util::ErrorCode::InvalidAddress;
    }

    return obj->setValue(value);
}

bool BusAccessUnit::isGroupObjectValid(GroupObjectIndex index) const {
    const auto* obj = groupObject(index);
    return obj && obj->isValid();
}

size_t BusAccessUnit::groupObjectAssociationCount(GroupObjectIndex index) const {
    // An empty out-span asks findAddressIndices for the required count only.
    const auto countRes = _associationTable.findAddressIndices(index, std::span<AddressTableIndex>{});
    return countRes.isOk() ? countRes.value() : 0u;
}

bool BusAccessUnit::isGroupObjectLinked(GroupObjectIndex index) const {
    return groupObjectAssociationCount(index) > 0u;
}

util::Result<application::DptValue> BusAccessUnit::groupObjectValue(GroupObjectIndex index) const {
    const auto* obj = groupObject(index);
    if (!obj) {
        return util::ErrorCode::InvalidAddress;
    }

    return obj->value();
}

util::Result<void> BusAccessUnit::requestGroupObjectValue(GroupObjectIndex index) {
    auto* obj = groupObject(index);
    if (!obj) {
        return util::ErrorCode::InvalidAddress;
    }

    return obj->requestValue();
}

util::Result<void> BusAccessUnit::beginRequestGroupObjectValue(GroupObjectIndex index) {
    return requestGroupObjectValue(index);
}

void BusAccessUnit::setGroupObjectWriteCallback(GroupObjectWriteCallback callback) {
    _groupObjectWriteCallback = std::move(callback);
}

void BusAccessUnit::setGroupObjectReadCallback(GroupObjectReadCallback callback) {
    _groupObjectReadCallback = std::move(callback);
}

util::Result<void> BusAccessUnit::sendGroupValue(const GroupAddress& address, std::span<const uint8_t> data) {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->sendGroupValueWrite(address, data);
}

util::Result<void> BusAccessUnit::beginSendGroupValue(const GroupAddress& address, std::span<const uint8_t> data) {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->beginSendGroupValueWrite(address, data);
}

util::Result<void> BusAccessUnit::sendGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data) {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }

    // An object the project left unlinked has no group address to send on. Bail
    // out before the shaping below so it neither latches a deferred send nor
    // spends a rate-limiter token that a linked object could have used. The
    // caller's value is already stored on the object, so a later link (or a read
    // response once one exists) still sees the current reading.
    if (!isGroupObjectLinked(objectIndex)) {
        return util::Result<void>::ok();
    }

    // Apply the object's transmit policy (send-on-change / cyclic / min-interval)
    // and the global rate limiter before the value hits the wire. All shaping is
    // opt-in: with the default (inert) policy and an unconfigured limiter this is
    // a straight pass-through to emitGroupValueForObject.
    //
    // The send-on-change filter is purely value-based and always applies. The
    // time-based parts (min-interval deferral, rate limiting, and the cyclic
    // pump in loop()) are meaningful only with a monotonic clock, so they are
    // skipped entirely when no time source is configured — otherwise a pinned
    // nowMs would defer forever and never refill the token bucket.
    const bool haveClock = static_cast<bool>(_timeSource);
    const uint32_t now = haveClock ? _timeSource() : 0u;
    auto* object = _groupObjectTable.getGroupObject(objectIndex);
    if (object != nullptr) {
        switch (object->decidePublish(now)) {
            case application::PublishDecision::Suppress:
                return util::Result<void>::ok();      // value unchanged — drop
            case application::PublishDecision::Defer:
                if (haveClock) {
                    object->setPendingSend(true);     // too soon — loop() will retry
                    return util::Result<void>::ok();
                }
                break;                                // no clock: send now
            case application::PublishDecision::Send:
                break;
        }
        if (haveClock && !_txRateLimiter.tryConsume(now)) {
            object->setPendingSend(true);             // bus busy — coalesce + retry
            return util::Result<void>::ok();
        }
        const auto result = emitGroupValueForObject(objectIndex, data);
        if (result.isOk()) {
            object->noteTransmitted(now);
        }
        return result;
    }

    return emitGroupValueForObject(objectIndex, data);
}

util::Result<void> BusAccessUnit::emitGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data) {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }

    // Resolve all group addresses associated with this communication object.
    std::array<AddressTableIndex, 32> addrIdxBuf{};
    const auto findRes = _associationTable.findAddressIndices(objectIndex,
                                                              std::span<AddressTableIndex>(addrIdxBuf));
    if (findRes.isError()) {
        return findRes.error();
    }

    // No association entry means ETS linked no group address to this object.
    // That is a legitimate, project-controlled configuration - the integrator
    // simply did not use this datapoint - so sending is a no-op, not a failure.
    // Reporting an error here made every cyclic refresh of an unlinked object
    // look like a stack fault.
    const size_t count = findRes.value();
    if (count == 0) {
        return util::Result<void>::ok();
    }

    util::Result<void> lastResult = util::Result<void>::ok();
    for (size_t i = 0; i < count; ++i) {
        const GroupAddress ga = _addressTable.getAddress(addrIdxBuf[i]);
        if (ga.raw == 0) {
            continue;
        }
        const auto sendRes = _stackPort->sendGroupValueWrite(ga, data);
        if (sendRes.isError()) {
            lastResult = sendRes;
        }
    }

    return lastResult;
}

util::Result<void> BusAccessUnit::setGroupObjectTransmitPolicy(
    GroupObjectIndex objectIndex,
    const application::GroupObjectTransmitPolicy& policy) {
    auto* object = _groupObjectTable.getGroupObject(objectIndex);
    if (object == nullptr) {
        return util::ErrorCode::InvalidParameter;
    }
    object->setTransmitPolicy(policy);
    return util::Result<void>::ok();
}

util::Result<application::GroupObjectTransmitPolicy>
BusAccessUnit::groupObjectTransmitPolicy(GroupObjectIndex objectIndex) const {
    const auto* object = _groupObjectTable.getGroupObject(objectIndex);
    if (object == nullptr) {
        return util::ErrorCode::InvalidParameter;
    }
    return object->transmitPolicy();
}

void BusAccessUnit::pumpGroupObjectTransmissions(uint32_t now) {
    // Cyclic heartbeats and deferred releases are time-based, so they only run
    // with a monotonic clock (see sendGroupValueForObject).
    if (!_timeSource) {
        return;
    }

    // Release cyclic-due heartbeats and deferred (min-interval or rate-limited)
    // sends. Each object always holds its latest value, so a deferred send
    // naturally coalesces to the newest reading. Under rate-limit saturation we
    // stop for this tick; still-pending/still-due objects retry on the next loop.
    const uint16_t count = _groupObjectTable.objectCount();
    for (uint16_t i = 0; i < count; ++i) {
        const GroupObjectIndex index(i);
        auto* object = _groupObjectTable.getGroupObject(index);
        if (object == nullptr || !object->transmit() || !object->isValid()) {
            continue;
        }
        // Unlinked objects have nowhere to send; skip them rather than burn a
        // rate-limiter token on a send that resolves to zero group addresses.
        if (!isGroupObjectLinked(index)) {
            continue;
        }
        const bool cyclic = object->dueForCyclic(now);
        const bool deferred = object->readyToSendDeferred(now);
        if (!cyclic && !deferred) {
            continue;
        }
        if (!_txRateLimiter.tryConsume(now)) {
            break;
        }
        const auto result = emitGroupValueForObject(index, object->getRawValue());
        if (result.isOk()) {
            object->noteTransmitted(now);
        }
    }
}

util::Result<void> BusAccessUnit::respondGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data) {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }

    std::array<AddressTableIndex, 32> addrIdxBuf{};
    const auto findRes = _associationTable.findAddressIndices(objectIndex,
                                                              std::span<AddressTableIndex>(addrIdxBuf));
    if (findRes.isError()) {
        return findRes.error();
    }

    // Unlinked object: nothing to respond on (see emitGroupValueForObject).
    const size_t count = findRes.value();
    if (count == 0) {
        return util::Result<void>::ok();
    }

    util::Result<void> lastResult = util::Result<void>::ok();
    for (size_t i = 0; i < count; ++i) {
        const GroupAddress ga = _addressTable.getAddress(addrIdxBuf[i]);
        if (ga.raw == 0) {
            continue;
        }
        const auto sendRes = _stackPort->sendGroupValueResponse(ga, data);
        if (sendRes.isError()) {
            lastResult = sendRes;
        }
    }

    return lastResult;
}

util::Result<void> BusAccessUnit::requestGroupValue(const GroupAddress& address) {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->sendGroupValueRead(address);
}

util::Result<void> BusAccessUnit::beginRequestGroupValue(const GroupAddress& address) {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->beginSendGroupValueRead(address);
}

util::Result<BusAccessUnit::TransmissionProgressState> BusAccessUnit::pollOutboundTransmission() {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->pollTransmissionProgress();
}


bool BusAccessUnit::popOutboundTransmissionOutcome(TransmissionOutcome& outcome) {
    return _stackPort ? _stackPort->popTransmissionOutcome(outcome) : false;
}

size_t BusAccessUnit::queuedOutboundTransmissionOutcomeCount() const {
    return _stackPort ? _stackPort->queuedTransmissionOutcomeCount() : 0u;
}

util::Result<void> BusAccessUnit::beginProcessAutomaticResponse() {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }
    if (_autoResponseOperation.active) {
        return util::ErrorCode::Busy;
    }

    PendingAutoResponse response;
    if (!popAutomaticResponse(response)) {
        return util::ErrorCode::OperationNotReady;
    }

    auto beginResult = _stackPort->beginSendGroupValueResponse(response.destination, response.data);
    if (beginResult.isError()) {
        enqueueAutomaticResponse(response.destination, response.data);
        return beginResult.error();
    }

    _autoResponseOperation.active = true;
    return util::Result<void>::ok();
}

util::Result<BusAccessUnit::TransmissionProgressState> BusAccessUnit::pollProcessAutomaticResponse() {
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_autoResponseOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    auto progress = _stackPort->pollTransmissionProgress();
    if (progress.isError()) {
        _autoResponseOperation = AutoResponseOperationState{};
        return progress.error();
    }

    if (progress.value() != TransmissionProgressState::Pending) {
        _autoResponseOperation = AutoResponseOperationState{};
    }
    return progress.value();
}

util::Result<void> BusAccessUnit::sendAutomaticResponseImmediate(const GroupAddress& destination,
                                                                 std::span<const uint8_t> data)
{
    if (!_initialized || !_stackPort) {
        return util::ErrorCode::NotInitialized;
    }

    auto beginResult = _stackPort->beginSendGroupValueResponse(destination, data);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    for (;;) {
        auto progress = _stackPort->pollTransmissionProgress();
        if (progress.isError()) {
            return progress.error();
        }

        switch (progress.value()) {
            case TransmissionProgressState::Pending:
                continue;
            case TransmissionProgressState::Success:
                return util::Result<void>::ok();
            case TransmissionProgressState::Busy:
                return util::ErrorCode::Busy;
            case TransmissionProgressState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
            case TransmissionProgressState::Timeout:
                return util::ErrorCode::Timeout;
        }
    }
}

void BusAccessUnit::enqueueAutomaticResponse(const GroupAddress& destination, std::span<const uint8_t> data) {
    PendingAutoResponse response;
    response.destination = destination;
    response.data.assign(data.begin(), data.end());

    if (_autoResponseQueueCount >= AUTO_RESPONSE_QUEUE_CAPACITY) {
        ++_droppedAutomaticResponses;
        KNX_LOGW(TAG, "Dropping automatic response for %d/%d/%d: queue full (%zu dropped)",
                 destination.main(), destination.middle(), destination.sub(),
                 _droppedAutomaticResponses);
        return;
    }

    const size_t index = (_autoResponseQueueHead + _autoResponseQueueCount) % AUTO_RESPONSE_QUEUE_CAPACITY;
    _autoResponseQueue[index] = std::move(response);
    ++_autoResponseQueueCount;

    notifyWorkAvailableIfTransitioned();
}

bool BusAccessUnit::popAutomaticResponse(PendingAutoResponse& response) {
    if (_autoResponseQueueCount == 0u) {
        return false;
    }

    response = std::move(_autoResponseQueue[_autoResponseQueueHead]);
    _autoResponseQueueHead = (_autoResponseQueueHead + 1u) % AUTO_RESPONSE_QUEUE_CAPACITY;
    --_autoResponseQueueCount;
    refreshWorkAvailabilityState();
    return true;
}

bool BusAccessUnit::enqueueInboundGroupEvent(MessageKind kind,
                                             const GroupAddress& destination,
                                             std::span<const uint8_t> data,
                                             std::span<const GroupObjectIndex> associatedObjects)
{
    if (!_inboundGroupEventQueue) {
        return false;
    }
    if (associatedObjects.size() > kMaxInboundAssociatedGroupObjects ||
        data.size() > config::MAX_APDU_LENGTH) {
        ++_droppedInboundGroupEvents;
        KNX_LOGW(TAG, "Dropping inbound group event for %d/%d/%d: invalid event shape",
                 destination.main(), destination.middle(), destination.sub());
        return false;
    }

    PendingInboundGroupEvent event{};
    event.kind = static_cast<uint8_t>(kind);
    event.destinationRaw = destination.raw;
    event.associatedObjectCount = static_cast<uint8_t>(associatedObjects.size());
    for (size_t index = 0; index < associatedObjects.size(); ++index) {
        event.associatedObjectIndices[index] = associatedObjects[index].value();
    }
    event.dataLength = static_cast<uint16_t>(data.size());
    std::copy(data.begin(), data.end(), event.data.begin());

    const auto enqueueResult = _inboundGroupEventQueue->send(&event, 0u);
    if (enqueueResult.isError()) {
        ++_droppedInboundGroupEvents;
        KNX_LOGW(TAG, "Dropping inbound group event for %d/%d/%d: %s",
                 destination.main(), destination.middle(), destination.sub(),
                 util::errorCodeToString(enqueueResult.error()));
        return false;
    }

    notifyWorkAvailableIfTransitioned();

    return true;
}

size_t BusAccessUnit::queuedInboundGroupEventCount() const {
    return _inboundGroupEventQueue ? _inboundGroupEventQueue->count() : 0u;
}

void BusAccessUnit::refreshWorkAvailabilityState() {
    _hadImmediateWork = ownerWorkHint().hasImmediateWork();
}

void BusAccessUnit::notifyWorkAvailableIfTransitioned() {
    const bool hasImmediateWork = ownerWorkHint().hasImmediateWork();
    if (!_hadImmediateWork && hasImmediateWork && _workAvailableCallback) {
        _workAvailableCallback();
    }
    _hadImmediateWork = hasImmediateWork;
}

void BusAccessUnit::processInboundGroupEvents()
{
    if (!_inboundGroupEventQueue) {
        return;
    }

    PendingInboundGroupEvent event{};
    size_t processedCount = 0u;
    while (processedCount < kMaxInboundGroupEventsPerLoop
           && _inboundGroupEventQueue->receive(&event, 0u).isOk()) {
        ++processedCount;
        const auto kind = static_cast<MessageKind>(event.kind);
        const GroupAddress destination(event.destinationRaw);
        const auto data = std::span<const uint8_t>(event.data.data(), event.dataLength);

        if (kind == MessageKind::GroupValueWrite ||
            kind == MessageKind::GroupValueResponse) {
            const auto source = (kind == MessageKind::GroupValueWrite)
                                    ? application::GroupObject::ValueSource::Write
                                    : application::GroupObject::ValueSource::Response;
            for (size_t index = 0; index < event.associatedObjectCount; ++index) {
                const GroupObjectIndex objectIndex(event.associatedObjectIndices[index]);
                auto* obj = _groupObjectTable.getGroupObject(objectIndex);
                if (obj == nullptr) {
                    continue;
                }
                // The object's communication flags decide whether this service
                // may touch it.  A rejected telegram must not reach the
                // firmware callback either, otherwise disabling an object in
                // ETS would still fire application logic.
                if (obj->receiveValue(data, source).isError()) {
                    continue;
                }
                KNX_LOGD(TAG, "Group value %s: %d/%d/%d -> obj %u",
                         kind == MessageKind::GroupValueWrite ? "write" : "response",
                         destination.main(), destination.middle(), destination.sub(),
                         static_cast<unsigned>(objectIndex.value()));
                if (kind == MessageKind::GroupValueWrite && _groupObjectWriteCallback) {
                    _groupObjectWriteCallback(objectIndex, data);
                }
            }
            continue;
        }

        if (kind != MessageKind::GroupValueRead) {
            continue;
        }

        KNX_LOGD(TAG, "Group value read: %d/%d/%d",
                 destination.main(), destination.middle(), destination.sub());

        // Read enable (R) decides whether this device answers at all.  Objects
        // without it stay silent, which is what lets several devices share a
        // group address with exactly one of them designated as the responder.
        for (size_t index = 0; index < event.associatedObjectCount; ++index) {
            const GroupObjectIndex objectIndex(event.associatedObjectIndices[index]);
            const auto* obj = _groupObjectTable.getGroupObject(objectIndex);
            if (obj && obj->readable() && _groupObjectReadCallback) {
                _groupObjectReadCallback(objectIndex);
            }
        }

        for (size_t index = 0; index < event.associatedObjectCount; ++index) {
            auto* obj = _groupObjectTable.getGroupObject(GroupObjectIndex(event.associatedObjectIndices[index]));
            if (obj && obj->readable() && obj->isValid() && _stackPort) {
                if (_autoResponseMode == AutoResponseMode::Deferred) {
                    enqueueAutomaticResponse(destination, obj->getRawValue());
                } else {
                    (void)sendAutomaticResponseImmediate(destination, obj->getRawValue());
                }
                break;
            }
        }
    }
}

void BusAccessUnit::clearInboundGroupEvents()
{
    if (!_inboundGroupEventQueue) {
        return;
    }

    PendingInboundGroupEvent event{};
    while (_inboundGroupEventQueue->receive(&event, 0u).isOk()) {
    }
}

void BusAccessUnit::handleApplicationFrame(const IndividualAddress& /*source*/,
                                           const GroupAddress& destination,
                                           MessageKind kind,
                                           std::span<const uint8_t> data,
                                           AddressType destinationType) {
    if (destinationType != AddressType::Group) {
        // Individual-addressed services are handled inside ApplicationLayer.
        return;
    }

    // Route via the KNX three-table chain so ETS-programmed associations are
    // honoured.  The legacy findByAddress() path bypasses the tables entirely
    // and must not be used for ETS-commissioned devices.
    //
    //   GroupAddress  →  AddressTableIndex (ASAP)
    //   AddressTableIndex  →  GroupObjectIndex[]  (via AssociationTable)
    //   GroupObjectIndex  →  GroupObject
    const AddressTableIndex addressIdx = _addressTable.findIndex(destination);
    const bool hasTableEntry = addressIdx.isValid();

    // Gather associated group object indices (up to 16 per GA is generous).
    std::array<GroupObjectIndex, 16> assocBuf{};
    std::span<GroupObjectIndex> assocSpan = assocBuf;
    size_t assocCount = 0;

    if (hasTableEntry) {
        const auto findRes = _associationTable.findGroupObjects(addressIdx, assocSpan);
        if (findRes.isOk()) {
            assocCount = findRes.value();
        }
    }

    const auto associatedObjects = std::span<const GroupObjectIndex>(assocBuf.data(), assocCount);

    if (kind == MessageKind::GroupValueWrite ||
        kind == MessageKind::GroupValueResponse) {
        if (assocCount > 0u) {
            (void)enqueueInboundGroupEvent(kind, destination, data, associatedObjects);
        }
        return;
    }

    if (kind == MessageKind::GroupValueRead) {
        if (assocCount > 0u) {
            (void)enqueueInboundGroupEvent(kind, destination, {}, associatedObjects);
        }
        return;
    }
}

} // namespace bau
} // namespace knx
