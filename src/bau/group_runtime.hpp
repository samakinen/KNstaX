// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file group_runtime.hpp
 * @brief The group-object runtime behind BusAccessUnit and TransmissionControl.
 *
 * Private to the BAU. Owns group communication: the object table surface,
 * outbound publish/respond with transmit shaping (send-on-change, cyclic,
 * min-interval, rate limit), Value-Read-on-Initialisation, the automatic
 * read-response queue, and the inbound event queue that marshals telegrams
 * from the lower-layer callback context onto the owner context.
 *
 * @thread_safety Owner context only, except enqueueInboundGroupEvent() and
 * handleApplicationFrame(), which are the two entry points called from the
 * lower-layer context and do nothing but copy into the queue.
 */

#pragma once

#include "knx/bau/bau.hpp"
#include "knx/platform/raii_resources.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace knx {
namespace bau {

class DeviceModel;

class GroupRuntime final {
public:
    using MessageKind = BusAccessUnit::MessageKind;
    using AutoResponseMode = BusAccessUnit::AutoResponseMode;
    using TransmissionOptions = BusAccessUnit::TransmissionOptions;
    using TransmissionOutcome = BusAccessUnit::TransmissionOutcome;
    using TransmissionProgressState = BusAccessUnit::TransmissionProgressState;
    using OwnerWorkHint = BusAccessUnit::OwnerWorkHint;

    GroupRuntime(platform::Platform& platform, BusAccessStackPort* stackPort, DeviceModel& model);

    // Installs callbacks capturing `this` into the stack port and the group objects.
    GroupRuntime(const GroupRuntime&) = delete;
    GroupRuntime& operator=(const GroupRuntime&) = delete;
    GroupRuntime(GroupRuntime&&) = delete;
    GroupRuntime& operator=(GroupRuntime&&) = delete;

    // --- Lifecycle (driven by BusAccessUnit) ---------------------------------

    bool inboundQueueReady() const;
    /// Drop queued events and responses and zero the drop counters.
    void resetQueues();
    void resetWorkAvailability() { _hadImmediateWork = false; }
    /// Route group services from the stack port into this runtime.
    void attachToStackPort();
    /// Sends are refused until the BAU has finished init().
    void setRunning(bool running) { _running = running; }
    /// Monotonic time, 0 without a time source.
    uint32_t nowMs() const { return _timeSource ? _timeSource() : 0u; }

    /// The owner-loop share of BusAccessUnit::loop().
    void processInboundGroupEvents();
    void pumpReadOnInit();
    void pumpGroupObjectTransmissions(uint32_t nowMs);
    void refreshWorkAvailabilityState();

    OwnerWorkHint ownerWorkHint() const;
    void setWorkAvailableCallback(BusAccessUnit::WorkAvailableCallback callback);

    // --- Group objects (BusAccessUnit's public surface) ---------------------

    application::GroupObject* groupObject(GroupObjectIndex index);
    const application::GroupObject* groupObject(GroupObjectIndex index) const;
    GroupObjectIndex addGroupObject(const GroupAddress& address,
                                    application::DptId dpt,
                                    const application::GroupObjectFlags& flags);
    util::Result<void> bindGroupObjectToAddress(GroupObjectIndex index, const GroupAddress& address);
    bool isGroupObjectValid(GroupObjectIndex index) const;
    size_t groupObjectAssociationCount(GroupObjectIndex index) const;
    bool isGroupObjectLinked(GroupObjectIndex index) const;
    util::Result<application::DptValue> groupObjectValue(GroupObjectIndex index) const;
    util::Result<void> setGroupObjectValue(GroupObjectIndex index, const application::DptValue& value);
    util::Result<void> requestGroupObjectValue(GroupObjectIndex index);
    void setGroupObjectWriteCallback(GroupObjectWriteCallback callback);
    void setGroupObjectReadCallback(GroupObjectReadCallback callback);

    util::Result<void> sendGroupValue(const GroupAddress& address, std::span<const uint8_t> data);
    util::Result<void> beginSendGroupValue(const GroupAddress& address, std::span<const uint8_t> data);
    util::Result<void> sendGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data);
    util::Result<void> respondGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data);
    /// Unconditional group-value write on all associated addresses (the raw send
    /// primitive behind sendGroupValueForObject, bypassing policy/rate gates).
    /// No associated address means no telegram and an ok result.
    util::Result<void> emitGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data);
    util::Result<void> requestGroupValue(const GroupAddress& address);
    util::Result<void> beginRequestGroupValue(const GroupAddress& address);
    util::Result<TransmissionProgressState> pollOutboundTransmission();

    void setTimeSource(BusAccessUnit::TimeSourceFn timeSource) { _timeSource = std::move(timeSource); }
    void setTelegramRateLimit(const application::TelegramRateLimitConfig& config) {
        _txRateLimiter.configure(config);
    }
    util::Result<void> setGroupObjectTransmitPolicy(GroupObjectIndex objectIndex,
                                                    const application::GroupObjectTransmitPolicy& policy);
    util::Result<application::GroupObjectTransmitPolicy>
    groupObjectTransmitPolicy(GroupObjectIndex objectIndex) const;

    // --- TransmissionControl ---------------------------------------------------

    void setDefaultTransmissionOptions(const TransmissionOptions& options);
    const TransmissionOptions& defaultTransmissionOptions() const;
    bool popOutboundTransmissionOutcome(TransmissionOutcome& outcome);
    size_t queuedOutboundTransmissionOutcomeCount() const;
    void setAutoResponseMode(AutoResponseMode mode) { _autoResponseMode = mode; }
    AutoResponseMode autoResponseMode() const { return _autoResponseMode; }
    size_t queuedAutomaticResponseCount() const { return _autoResponseQueueCount; }
    size_t droppedAutomaticResponseCount() const { return _droppedAutomaticResponses; }
    size_t droppedInboundGroupEventCount() const { return _droppedInboundGroupEvents; }
    util::Result<void> beginProcessAutomaticResponse();
    util::Result<TransmissionProgressState> pollProcessAutomaticResponse();

    /// Group service entry from the stack port, and from PID_GO_DIAGNOSTICS,
    /// which hands its own telegram to the local objects.
    void handleApplicationFrame(const IndividualAddress& source,
                                const GroupAddress& destination,
                                MessageKind kind,
                                std::span<const uint8_t> data,
                                AddressType destinationType);

private:
    struct PendingAutoResponse {
        GroupAddress destination;
        std::vector<uint8_t> data;
    };

    static constexpr size_t AUTO_RESPONSE_QUEUE_CAPACITY = 4u;
    static constexpr size_t INBOUND_GROUP_EVENT_QUEUE_CAPACITY = 8u;

    bool enqueueInboundGroupEvent(MessageKind kind,
                                  const GroupAddress& destination,
                                  std::span<const uint8_t> data);
    void clearInboundGroupEvents();
    util::Result<void> sendAutomaticResponseImmediate(const GroupAddress& destination,
                                                      std::span<const uint8_t> data);
    void enqueueAutomaticResponse(const GroupAddress& destination, std::span<const uint8_t> data);
    bool popAutomaticResponse(PendingAutoResponse& response);
    size_t queuedInboundGroupEventCount() const;
    void notifyWorkAvailableIfTransitioned();

    BusAccessStackPort* const _stackPort;
    objects::DeviceObject& _deviceObject;
    objects::AddressTableObject& _addressTable;
    objects::AssociationTableObject& _associationTable;
    objects::GroupObjectTableObject& _groupObjectTable;
    bool _running{false};

    GroupObjectWriteCallback _groupObjectWriteCallback;
    GroupObjectReadCallback _groupObjectReadCallback;

    std::unique_ptr<platform::Queue> _inboundGroupEventQueue;
    std::array<PendingAutoResponse, AUTO_RESPONSE_QUEUE_CAPACITY> _autoResponseQueue{};
    size_t _autoResponseQueueHead{0};
    size_t _autoResponseQueueCount{0};
    AutoResponseMode _autoResponseMode{AutoResponseMode::Immediate};
    bool _autoResponseActive{false};
    size_t _droppedAutomaticResponses{0};
    size_t _droppedInboundGroupEvents{0};
    BusAccessUnit::WorkAvailableCallback _workAvailableCallback{};
    bool _hadImmediateWork{false};

    // Outbound transmit shaping.
    BusAccessUnit::TimeSourceFn _timeSource{};
    application::TelegramRateLimiter _txRateLimiter{};

    // Value-Read-on-Initialisation (group object flag I).  Runs once after the
    // device reaches a valid individual address, spread across loop() calls so
    // a whole-installation restart does not turn into a read storm.
    enum class ReadOnInitState : uint8_t { Pending, Running, Done };
    static constexpr size_t kMaxReadOnInitPerLoop = 4u;
    ReadOnInitState _readOnInitState{ReadOnInitState::Pending};
    uint16_t _readOnInitCursor{0};
};

} // namespace bau
} // namespace knx
