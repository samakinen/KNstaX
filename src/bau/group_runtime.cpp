// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file group_runtime.cpp
 * @brief GroupRuntime: group objects, transmit shaping, inbound queue, and the
 *        TransmissionControl facet over it.
 */

#include "group_runtime.hpp"

#include "device_model.hpp"
#include "knx/config.hpp"
#include "knx/util/log.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace knx {
namespace bau {

namespace {

constexpr const char* TAG = "KNX.BAU";

/// Inbound group events drained per loop() call, so a burst cannot starve the
/// rest of the owner context.
constexpr size_t kMaxInboundGroupEventsPerLoop = 32u;

/**
 * @brief One inbound group telegram, marshalled out of the lower-layer callback
 *        context onto the BAU owner context.
 *
 * Trivially copyable on purpose: it travels through a `platform::Queue`, which
 * moves raw bytes.
 *
 * It carries the group address only. The owner context resolves the
 * associated group objects from the tables when it processes the event, so
 * there is no cap on how many objects one address reaches (a fixed array of
 * 16 here used to drop the whole telegram for the 17th).
 */
struct PendingInboundGroupEvent {
    uint8_t kind{static_cast<uint8_t>(BusAccessUnit::MessageKind::Unknown)};
    uint16_t destinationRaw{0u};
    uint16_t dataLength{0u};
    std::array<uint8_t, config::MAX_APDU_LENGTH> data{};
};

static_assert(std::is_trivially_copyable_v<PendingInboundGroupEvent>,
              "PendingInboundGroupEvent travels through a byte queue");

} // namespace

GroupRuntime::GroupRuntime(platform::Platform& platform, BusAccessStackPort* stackPort, DeviceModel& model)
    : _stackPort(stackPort)
    , _deviceObject(model.deviceObject())
    , _addressTable(model.addressTable())
    , _associationTable(model.associationTable())
    , _groupObjectTable(model.groupObjectTable())
    , _inboundGroupEventQueue(std::make_unique<platform::Queue>(
          platform,
          sizeof(PendingInboundGroupEvent),
          INBOUND_GROUP_EVENT_QUEUE_CAPACITY))
{
}

// ─── Lifecycle ───────────────────────────────────────────────────────────────

bool GroupRuntime::inboundQueueReady() const
{
    return _inboundGroupEventQueue && _inboundGroupEventQueue->handle();
}

void GroupRuntime::resetQueues()
{
    clearInboundGroupEvents();
    _droppedInboundGroupEvents = 0u;
    _droppedAutomaticResponses = 0u;
    _autoResponseQueueHead = 0u;
    _autoResponseQueueCount = 0u;
    _autoResponseActive = false;
}

void GroupRuntime::attachToStackPort()
{
    if (_stackPort == nullptr) {
        return;
    }
    // Route group value services back into the group objects.
    _stackPort->setInboundCallback([this](const IndividualAddress& source,
                                          const GroupAddress& destination,
                                          MessageKind kind,
                                          std::span<const uint8_t> data,
                                          AddressType destinationType) {
        handleApplicationFrame(source, destination, kind, data, destinationType);
    });
    _stackPort->setWorkAvailableCallback([this]() {
        notifyWorkAvailableIfTransitioned();
    });
}

BusAccessUnit::OwnerWorkHint GroupRuntime::ownerWorkHint() const
{
    OwnerWorkHint hint;
    hint.pendingLoopWorkItems = queuedInboundGroupEventCount();
    hint.pendingDeferredWorkItems = _autoResponseQueueCount;
    return hint;
}

void GroupRuntime::setWorkAvailableCallback(BusAccessUnit::WorkAvailableCallback callback)
{
    _workAvailableCallback = std::move(callback);
    if (_stackPort != nullptr) {
        _stackPort->setWorkAvailableCallback([this]() {
            notifyWorkAvailableIfTransitioned();
        });
    }
    refreshWorkAvailabilityState();
}

void GroupRuntime::refreshWorkAvailabilityState()
{
    _hadImmediateWork = ownerWorkHint().hasImmediateWork();
}

void GroupRuntime::notifyWorkAvailableIfTransitioned()
{
    const bool hasImmediateWork = ownerWorkHint().hasImmediateWork();
    if (!_hadImmediateWork && hasImmediateWork && _workAvailableCallback) {
        _workAvailableCallback();
    }
    _hadImmediateWork = hasImmediateWork;
}

// ─── Group objects ───────────────────────────────────────────────────────────

application::GroupObject* GroupRuntime::groupObject(GroupObjectIndex index)
{
    return _groupObjectTable.getGroupObject(index);
}

const application::GroupObject* GroupRuntime::groupObject(GroupObjectIndex index) const
{
    return _groupObjectTable.getGroupObject(index);
}

GroupObjectIndex GroupRuntime::addGroupObject(const GroupAddress& address,
                                              application::DptId dpt,
                                              const application::GroupObjectFlags& flags)
{
    application::GroupObjectConfig config{};
    config.address = address;
    config.dpt = dpt;
    config.flags = flags;

    auto obj = std::make_unique<application::GroupObject>(config);
    obj->setReadRequestSender([this](const GroupAddress& addr) {
        return requestGroupValue(addr);
    });

    GroupObjectIndex index = _groupObjectTable.addGroupObject(std::move(obj));
    if (!index.isValid()) {
        return GroupObjectIndex::invalid();
    }

    // Subscribe to group address in data link layer
    if (_stackPort != nullptr) {
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

util::Result<void> GroupRuntime::bindGroupObjectToAddress(GroupObjectIndex index,
                                                          const GroupAddress& address)
{
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

    if (_stackPort != nullptr) {
        const auto addGroupAddress = _stackPort->addGroupAddress(address);
        if (addGroupAddress.isError()) {
            return addGroupAddress.error();
        }
    }

    return util::Result<void>::ok();
}

util::Result<void> GroupRuntime::setGroupObjectValue(GroupObjectIndex index, const application::DptValue& value)
{
    auto* obj = groupObject(index);
    if (!obj) {
        return util::ErrorCode::InvalidAddress;
    }

    return obj->setValue(value);
}

bool GroupRuntime::isGroupObjectValid(GroupObjectIndex index) const
{
    const auto* obj = groupObject(index);
    return obj && obj->isValid();
}

size_t GroupRuntime::groupObjectAssociationCount(GroupObjectIndex index) const
{
    // An empty out-span asks findAddressIndices for the required count only.
    const auto countRes = _associationTable.findAddressIndices(index, std::span<AddressTableIndex>{});
    return countRes.isOk() ? countRes.value() : 0u;
}

bool GroupRuntime::isGroupObjectLinked(GroupObjectIndex index) const
{
    return groupObjectAssociationCount(index) > 0u;
}

util::Result<application::DptValue> GroupRuntime::groupObjectValue(GroupObjectIndex index) const
{
    const auto* obj = groupObject(index);
    if (!obj) {
        return util::ErrorCode::InvalidAddress;
    }

    return obj->value();
}

util::Result<void> GroupRuntime::requestGroupObjectValue(GroupObjectIndex index)
{
    auto* obj = groupObject(index);
    if (!obj) {
        return util::ErrorCode::InvalidAddress;
    }

    return obj->requestValue();
}

void GroupRuntime::setGroupObjectWriteCallback(GroupObjectWriteCallback callback)
{
    _groupObjectWriteCallback = std::move(callback);
}

void GroupRuntime::setGroupObjectReadCallback(GroupObjectReadCallback callback)
{
    _groupObjectReadCallback = std::move(callback);
}

// ─── Outbound ────────────────────────────────────────────────────────────────

util::Result<void> GroupRuntime::sendGroupValue(const GroupAddress& address, std::span<const uint8_t> data)
{
    if (!_running || _stackPort == nullptr) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->sendGroupValueWrite(address, data);
}

util::Result<void> GroupRuntime::beginSendGroupValue(const GroupAddress& address, std::span<const uint8_t> data)
{
    if (!_running || _stackPort == nullptr) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->beginSendGroupValueWrite(address, data);
}

util::Result<void> GroupRuntime::sendGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data)
{
    if (!_running || _stackPort == nullptr) {
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

util::Result<void> GroupRuntime::emitGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data)
{
    if (!_running || _stackPort == nullptr) {
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

util::Result<void> GroupRuntime::setGroupObjectTransmitPolicy(
    GroupObjectIndex objectIndex,
    const application::GroupObjectTransmitPolicy& policy)
{
    auto* object = _groupObjectTable.getGroupObject(objectIndex);
    if (object == nullptr) {
        return util::ErrorCode::InvalidParameter;
    }
    object->setTransmitPolicy(policy);
    return util::Result<void>::ok();
}

util::Result<application::GroupObjectTransmitPolicy>
GroupRuntime::groupObjectTransmitPolicy(GroupObjectIndex objectIndex) const
{
    const auto* object = _groupObjectTable.getGroupObject(objectIndex);
    if (object == nullptr) {
        return util::ErrorCode::InvalidParameter;
    }
    return object->transmitPolicy();
}

void GroupRuntime::pumpGroupObjectTransmissions(uint32_t now)
{
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

util::Result<void> GroupRuntime::respondGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data)
{
    if (!_running || _stackPort == nullptr) {
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

util::Result<void> GroupRuntime::requestGroupValue(const GroupAddress& address)
{
    if (!_running || _stackPort == nullptr) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->sendGroupValueRead(address);
}

util::Result<void> GroupRuntime::beginRequestGroupValue(const GroupAddress& address)
{
    if (!_running || _stackPort == nullptr) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->beginSendGroupValueRead(address);
}

util::Result<GroupRuntime::TransmissionProgressState> GroupRuntime::pollOutboundTransmission()
{
    if (!_running || _stackPort == nullptr) {
        return util::ErrorCode::NotInitialized;
    }
    return _stackPort->pollTransmissionProgress();
}

void GroupRuntime::pumpReadOnInit()
{
    if (_readOnInitState == ReadOnInitState::Done) {
        return;
    }

    // Value-Read-on-Initialisation is only meaningful once the device has an
    // individual address and can actually be answered.  Before that the reads
    // would be emitted from the unprogrammed address and thrown away.
    if (!_running || !_deviceObject.getIndividualAddress().isValid()) {
        return;
    }

    if (_readOnInitState == ReadOnInitState::Pending) {
        _readOnInitState = ReadOnInitState::Running;
        _readOnInitCursor = 0u;
    }

    // 03/05/01 §4.12.5.2.4.1.3 warns that this feature multiplies bus load
    // after a whole-installation restart, so the reads are spread over
    // successive loop() calls and pushed through the same rate limiter as any
    // other transmission instead of being burst out in one pass.
    const uint16_t count = _groupObjectTable.objectCount();
    size_t issuedThisPass = 0u;
    while (_readOnInitCursor < count && issuedThisPass < kMaxReadOnInitPerLoop) {
        const GroupObjectIndex index(_readOnInitCursor);
        auto* obj = _groupObjectTable.getGroupObject(index);
        ++_readOnInitCursor;

        if (obj == nullptr || !obj->readOnInit()) {
            continue;
        }
        if (!obj->getAddress().isValid()) {
            continue;  // Not yet bound by ETS — nothing to read from.
        }

        const auto res = requestGroupValue(obj->getAddress());
        if (res.isError()) {
            if (res.error() == util::ErrorCode::Busy || res.error() == util::ErrorCode::QueueFull) {
                // Bus or rate limiter is saturated; retry this object next loop.
                --_readOnInitCursor;
                return;
            }
            KNX_LOGW(TAG, "Read-on-init failed for obj %u: %d",
                     static_cast<unsigned>(index.value()), static_cast<int>(res.error()));
        }
        ++issuedThisPass;
    }

    if (_readOnInitCursor >= count) {
        _readOnInitState = ReadOnInitState::Done;
    }
}

// ─── TransmissionControl surface ─────────────────────────────────────────────

void GroupRuntime::setDefaultTransmissionOptions(const TransmissionOptions& options)
{
    if (_stackPort != nullptr) {
        _stackPort->setDefaultTransmissionOptions(options);
    }
}

const GroupRuntime::TransmissionOptions& GroupRuntime::defaultTransmissionOptions() const
{
    static const TransmissionOptions fallback{};
    return _stackPort != nullptr ? _stackPort->defaultTransmissionOptions() : fallback;
}

bool GroupRuntime::popOutboundTransmissionOutcome(TransmissionOutcome& outcome)
{
    return _stackPort != nullptr ? _stackPort->popTransmissionOutcome(outcome) : false;
}

size_t GroupRuntime::queuedOutboundTransmissionOutcomeCount() const
{
    return _stackPort != nullptr ? _stackPort->queuedTransmissionOutcomeCount() : 0u;
}

util::Result<void> GroupRuntime::beginProcessAutomaticResponse()
{
    if (!_running || _stackPort == nullptr) {
        return util::ErrorCode::NotInitialized;
    }
    if (_autoResponseActive) {
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

    _autoResponseActive = true;
    return util::Result<void>::ok();
}

util::Result<GroupRuntime::TransmissionProgressState> GroupRuntime::pollProcessAutomaticResponse()
{
    if (!_running || _stackPort == nullptr) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_autoResponseActive) {
        return util::ErrorCode::OperationNotReady;
    }

    auto progress = _stackPort->pollTransmissionProgress();
    if (progress.isError()) {
        _autoResponseActive = false;
        return progress.error();
    }

    if (progress.value() != TransmissionProgressState::Pending) {
        _autoResponseActive = false;
    }
    return progress.value();
}

util::Result<void> GroupRuntime::sendAutomaticResponseImmediate(const GroupAddress& destination,
                                                                std::span<const uint8_t> data)
{
    if (!_running || _stackPort == nullptr) {
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

void GroupRuntime::enqueueAutomaticResponse(const GroupAddress& destination, std::span<const uint8_t> data)
{
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

bool GroupRuntime::popAutomaticResponse(PendingAutoResponse& response)
{
    if (_autoResponseQueueCount == 0u) {
        return false;
    }

    response = std::move(_autoResponseQueue[_autoResponseQueueHead]);
    _autoResponseQueueHead = (_autoResponseQueueHead + 1u) % AUTO_RESPONSE_QUEUE_CAPACITY;
    --_autoResponseQueueCount;
    refreshWorkAvailabilityState();
    return true;
}

// ─── Inbound ─────────────────────────────────────────────────────────────────

bool GroupRuntime::enqueueInboundGroupEvent(MessageKind kind,
                                            const GroupAddress& destination,
                                            std::span<const uint8_t> data)
{
    if (!_inboundGroupEventQueue) {
        return false;
    }
    if (data.size() > config::MAX_APDU_LENGTH) {
        ++_droppedInboundGroupEvents;
        KNX_LOGW(TAG, "Dropping inbound group event for %d/%d/%d: invalid event shape",
                 destination.main(), destination.middle(), destination.sub());
        return false;
    }

    PendingInboundGroupEvent event{};
    event.kind = static_cast<uint8_t>(kind);
    event.destinationRaw = destination.raw;
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

size_t GroupRuntime::queuedInboundGroupEventCount() const
{
    return _inboundGroupEventQueue ? _inboundGroupEventQueue->count() : 0u;
}

void GroupRuntime::processInboundGroupEvents()
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

        // Resolve GA → ASAP → group objects against the tables as they are
        // now, iterating the association table in place: any number of group
        // objects may share an address.
        const AddressTableIndex addressIdx = _addressTable.findIndex(destination);
        if (!addressIdx.isValid()) {
            continue;
        }

        if (kind == MessageKind::GroupValueWrite ||
            kind == MessageKind::GroupValueResponse) {
            const auto source = (kind == MessageKind::GroupValueWrite)
                                    ? application::GroupObject::ValueSource::Write
                                    : application::GroupObject::ValueSource::Response;
            _associationTable.forEachGroupObject(addressIdx, [&](GroupObjectIndex objectIndex) {
                auto* obj = _groupObjectTable.getGroupObject(objectIndex);
                if (obj == nullptr) {
                    return;
                }
                // The object's communication flags decide whether this service
                // may touch it.  A rejected telegram must not reach the
                // firmware callback either, otherwise disabling an object in
                // ETS would still fire application logic.
                if (obj->receiveValue(data, source).isError()) {
                    return;
                }
                KNX_LOGD(TAG, "Group value %s: %d/%d/%d -> obj %u",
                         kind == MessageKind::GroupValueWrite ? "write" : "response",
                         destination.main(), destination.middle(), destination.sub(),
                         static_cast<unsigned>(objectIndex.value()));
                if (kind == MessageKind::GroupValueWrite && _groupObjectWriteCallback) {
                    _groupObjectWriteCallback(objectIndex, data);
                }
            });
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
        _associationTable.forEachGroupObject(addressIdx, [&](GroupObjectIndex objectIndex) {
            const auto* obj = _groupObjectTable.getGroupObject(objectIndex);
            if (obj && obj->readable() && _groupObjectReadCallback) {
                _groupObjectReadCallback(objectIndex);
            }
        });

        // Only the first readable, valid object answers: one response per read.
        bool responded = false;
        _associationTable.forEachGroupObject(addressIdx, [&](GroupObjectIndex objectIndex) {
            if (responded) {
                return;
            }
            auto* obj = _groupObjectTable.getGroupObject(objectIndex);
            if (obj && obj->readable() && obj->isValid() && _stackPort != nullptr) {
                if (_autoResponseMode == AutoResponseMode::Deferred) {
                    enqueueAutomaticResponse(destination, obj->getRawValue());
                } else {
                    if (const auto sent = sendAutomaticResponseImmediate(destination, obj->getRawValue());
                        sent.isError()) {
                        KNX_LOGW(TAG, "Automatic response for %d/%d/%d not sent: %s",
                                 destination.main(), destination.middle(), destination.sub(),
                                 util::errorCodeToString(sent.error()));
                    }
                }
                responded = true;
            }
        });
    }
}

void GroupRuntime::clearInboundGroupEvents()
{
    if (!_inboundGroupEventQueue) {
        return;
    }

    PendingInboundGroupEvent event{};
    while (_inboundGroupEventQueue->receive(&event, 0u).isOk()) {
    }
}

void GroupRuntime::handleApplicationFrame(const IndividualAddress& /*source*/,
                                          const GroupAddress& destination,
                                          MessageKind kind,
                                          std::span<const uint8_t> data,
                                          AddressType destinationType)
{
    if (destinationType != AddressType::Group) {
        // Individual-addressed services are handled inside ApplicationLayer.
        return;
    }

    // Route via the KNX three-table chain so ETS-programmed associations are
    // honoured.  Matching a group object by its own address would bypass the
    // tables entirely and must not be used for ETS-commissioned devices.
    //
    //   GroupAddress  →  AddressTableIndex (ASAP)
    //   AddressTableIndex  →  GroupObjectIndex[]  (via AssociationTable)
    //   GroupObjectIndex  →  GroupObject
    //
    // Here we only decide whether the telegram concerns this device at all;
    // the owner context resolves the group objects when it processes the
    // event (processInboundGroupEvents). Copying them into a 16-entry buffer
    // here made findGroupObjects() fail with BufferTooSmall for the 17th
    // object on an address, and the whole telegram was dropped.
    const AddressTableIndex addressIdx = _addressTable.findIndex(destination);
    if (!addressIdx.isValid() || !_associationTable.hasGroupObjects(addressIdx)) {
        return;
    }

    if (kind == MessageKind::GroupValueWrite ||
        kind == MessageKind::GroupValueResponse) {
        (void)enqueueInboundGroupEvent(kind, destination, data);
        return;
    }

    if (kind == MessageKind::GroupValueRead) {
        (void)enqueueInboundGroupEvent(kind, destination, {});
        return;
    }
}

// ─── TransmissionControl facet ───────────────────────────────────────────────

void TransmissionControl::setDefaultOptions(const Options& options) { _runtime.setDefaultTransmissionOptions(options); }
const TransmissionControl::Options& TransmissionControl::defaultOptions() const { return _runtime.defaultTransmissionOptions(); }
util::Result<TransmissionControl::ProgressState> TransmissionControl::poll() { return _runtime.pollOutboundTransmission(); }
bool TransmissionControl::popOutcome(Outcome& outcome) { return _runtime.popOutboundTransmissionOutcome(outcome); }
size_t TransmissionControl::queuedOutcomeCount() const { return _runtime.queuedOutboundTransmissionOutcomeCount(); }
void TransmissionControl::setAutoResponseMode(AutoResponseMode mode) { _runtime.setAutoResponseMode(mode); }
TransmissionControl::AutoResponseMode TransmissionControl::autoResponseMode() const { return _runtime.autoResponseMode(); }
util::Result<void> TransmissionControl::beginAutomaticResponse() { return _runtime.beginProcessAutomaticResponse(); }
util::Result<TransmissionControl::ProgressState> TransmissionControl::pollAutomaticResponse() { return _runtime.pollProcessAutomaticResponse(); }
size_t TransmissionControl::queuedAutomaticResponseCount() const { return _runtime.queuedAutomaticResponseCount(); }
size_t TransmissionControl::droppedAutomaticResponseCount() const { return _runtime.droppedAutomaticResponseCount(); }
size_t TransmissionControl::droppedInboundGroupEventCount() const { return _runtime.droppedInboundGroupEventCount(); }

const ConstTransmissionControl::Options& ConstTransmissionControl::defaultOptions() const { return _runtime.defaultTransmissionOptions(); }
size_t ConstTransmissionControl::queuedOutcomeCount() const { return _runtime.queuedOutboundTransmissionOutcomeCount(); }
ConstTransmissionControl::AutoResponseMode ConstTransmissionControl::autoResponseMode() const { return _runtime.autoResponseMode(); }
size_t ConstTransmissionControl::queuedAutomaticResponseCount() const { return _runtime.queuedAutomaticResponseCount(); }
size_t ConstTransmissionControl::droppedAutomaticResponseCount() const { return _runtime.droppedAutomaticResponseCount(); }
size_t ConstTransmissionControl::droppedInboundGroupEventCount() const { return _runtime.droppedInboundGroupEventCount(); }

} // namespace bau
} // namespace knx
