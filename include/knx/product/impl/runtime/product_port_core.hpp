// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file product_port_core.hpp
 * @brief The non-template half of the endpoint runtime: everything that works on ports by slot.
 *
 * EndpointRuntime<Definition, Capacity> is a thin shell around one of these. The
 * shell owns the storage whose size depends on the product (the configured
 * group addresses, the pending-action queue, the binding entries) and hands
 * the core pointers to it; the core, compiled once into the library, does the
 * rest: registers the group objects, binds addresses, applies transmit
 * policies, answers reads, dispatches bus writes to the bindings, publishes and
 * drains the pending-action queue. Nothing in it is instantiated per port, so
 * its code size does not grow with the product.
 *
 * Group-object access is deliberately narrow. The core touches the BAU only
 * through registerGroupObjects() and the four calls `setGroupObjectValue`,
 * `sendGroupValueForObject` / `respondGroupValueForObject`, `groupObject()` and
 * `isGroupObjectLinked` (and the group-object value read on a bus write), so a
 * more compact group-object representation (small-target-profile.md 6.2) only
 * has to replace those. PortDescriptor carries no GroupObject types.
 *
 * Concurrency: as the runtime it serves. Everything here runs in the owner
 * context (the thread that calls loop() and the typed API), except that
 * nothing is locked: the queue and the bindings are not safe to share.
 */

#pragma once

#include "knx/application/dpt.hpp"
#include "knx/bau/bau.hpp"
#include "knx/product/impl/bindings/endpoint_bindings.hpp"
#include "knx/product/impl/model/port_table.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>

namespace knx::product {

enum class PendingBusActionKind : uint8_t {
    Publish = 0,
    ReadResponse = 1,
};

/// A publish or read response that was requested before the stack started (or
/// could not be sent yet). Kept until the BAU exists, then sent in order.
struct PendingPortAction {
    PendingBusActionKind kind{PendingBusActionKind::Publish};
    uint16_t logicalId{0};
    uint16_t slot{0};
    application::DptValue value{};
};

/// Where the core finds the product's bindings: the entry arrays of its
/// EndpointBindings, indexed by the ranks in the port table.
struct PortBindingTables {
    detail::ReceiveBinding* commands{nullptr};
    detail::ReceiveBinding* stateWrites{nullptr};
    detail::StateBinding* providers{nullptr};
};

class ProductPortCore {
public:
    ProductPortCore(const PortDescriptor* ports,
                    uint16_t portCount,
                    GroupAddress* configuredAddresses,
                    PendingPortAction* pendingActions,
                    uint16_t pendingCapacity,
                    PortBindingTables bindings,
                    IndividualAddress defaultIndividualAddress);

    ProductPortCore(const ProductPortCore&) = delete;
    ProductPortCore& operator=(const ProductPortCore&) = delete;

    // ── Lookup ───────────────────────────────────────────────────────────────

    /// Slot of the port with this logical id, or kNoSlot.
    uint16_t slotFor(uint16_t logicalId) const;
    uint16_t portCount() const { return _portCount; }

    /// The started device the core works on, or nullptr (before start() and after stop()).
    void attach(bau::BusAccessUnit* bau) { _bau = bau; }
    bool started() const { return _bau != nullptr; }

    /// The device's address: the BAU's once started, the configured default before.
    IndividualAddress individualAddress() const;

    // ── Configuration (valid before and after start) ─────────────────────────

    /// InvalidAddress for an invalid address, InvalidParameter for a slot that is no port.
    util::Result<void> bindGroupAddress(uint16_t slot, GroupAddress address);
    util::Result<void> setTransmitPolicy(uint16_t slot, const application::GroupObjectTransmitPolicy& policy);
    util::Result<application::GroupObjectTransmitPolicy> transmitPolicy(uint16_t slot) const;
    bool isPortLinked(uint16_t slot) const;

    // ── Start-up (the shell calls these in this order once the BAU exists) ───

    /// One group object per port, at the port's slot. ResourceUnavailable if the
    /// group object table hands out a different index.
    util::Result<void> registerGroupObjects();
    /// Routes bus writes and reads on the group objects to dispatchWrite / prepareRead.
    void wireCallbacks();
    util::Result<void> applyConfiguredGroupAddresses();
    /// Seeds every readable port's group object from its state provider.
    void seedReadableState();

    // ── Outbound ─────────────────────────────────────────────────────────────

    /// Send (or, before start, queue) a group value write. Errors: InvalidParameter
    /// (no such port), AccessDenied (port does not transmit), OperationNotReady
    /// (device has no individual address yet), QueueFull (not started, queue full).
    util::Result<void> publish(uint16_t slot, const application::DptValue& value);

    /// Answer a read of the port from its state provider; same queueing rules.
    /// AccessDenied for a port that is not readable, OperationNotReady without a provider.
    util::Result<void> respond(uint16_t slot);

    /// Send what was queued before start. Results are dropped, as for any send.
    void drainPendingActions();

    // ── Inbound ──────────────────────────────────────────────────────────────

    /// A bus write reached the group object at @p slot: read its value and dispatch it.
    util::Result<void> dispatchWrite(GroupObjectIndex slot);

    /// Hand a value to the port's handlers: the command handler first, then the
    /// state-write handler. AccessDenied if the port takes no writes, DecodeFailed
    /// if a handler exists but the value is not of its type, OperationNotReady if
    /// nothing handled it.
    util::Result<void> dispatchValue(uint16_t slot, const application::DptValue& value);

    /// A bus read reached the object: refresh its value from the state provider.
    util::Result<void> prepareRead(GroupObjectIndex slot);

    // ── Pending actions ──────────────────────────────────────────────────────

    size_t pendingActionCount() const { return _pendingCount; }
    const PendingPortAction* pendingActionAt(size_t index) const;
    void clearPendingActions() { _pendingCount = 0u; }

private:
    util::Result<void> publishStarted(uint16_t slot, const application::DptValue& value);
    util::Result<void> respondStarted(uint16_t slot, const application::DptValue& value);
    util::Result<void> enqueue(PendingBusActionKind kind, uint16_t slot, const application::DptValue& value);
    /// The port's current state from its provider; false when it has none.
    bool readState(const PortDescriptor& port, application::DptValue& out) const;

    const PortDescriptor* _ports;
    GroupAddress* _configuredAddresses;
    PendingPortAction* _pending;
    PortBindingTables _bindings;
    bau::BusAccessUnit* _bau{nullptr};
    uint16_t _portCount;
    uint16_t _pendingCapacity;
    uint16_t _pendingCount{0u};
    IndividualAddress _defaultAddress;
};

} // namespace knx::product
