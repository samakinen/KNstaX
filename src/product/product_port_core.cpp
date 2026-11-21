// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/product/impl/runtime/product_port_core.hpp"

#include <memory>
#include <utility>

namespace knx::product {

using application::DptValue;
using util::ErrorCode;
using util::Result;

ProductPortCore::ProductPortCore(const PortDescriptor* ports,
                                 uint16_t portCount,
                                 GroupAddress* configuredAddresses,
                                 PendingPortAction* pendingActions,
                                 uint16_t pendingCapacity,
                                 PortBindingTables bindings,
                                 IndividualAddress defaultIndividualAddress)
    : _ports(ports)
    , _configuredAddresses(configuredAddresses)
    , _pending(pendingActions)
    , _bindings(bindings)
    , _portCount(portCount)
    , _pendingCapacity(pendingCapacity)
    , _defaultAddress(defaultIndividualAddress)
{
}

uint16_t ProductPortCore::slotFor(uint16_t logicalId) const
{
    return findPortSlot(_ports, _portCount, logicalId);
}

IndividualAddress ProductPortCore::individualAddress() const
{
    return _bau != nullptr ? _bau->deviceObject().getIndividualAddress() : _defaultAddress;
}

// ── Configuration ────────────────────────────────────────────────────────────

Result<void> ProductPortCore::bindGroupAddress(uint16_t slot, GroupAddress address)
{
    if (!address.isValid()) {
        return ErrorCode::InvalidAddress;
    }
    if (slot >= _portCount) {
        return ErrorCode::InvalidParameter;
    }

    _configuredAddresses[slot] = address;

    if (_bau != nullptr) {
        return _bau->bindGroupObjectToAddress(GroupObjectIndex(slot), address);
    }
    return Result<void>::ok();
}

Result<void> ProductPortCore::setTransmitPolicy(uint16_t slot, const application::GroupObjectTransmitPolicy& policy)
{
    if (slot >= _portCount) {
        return ErrorCode::InvalidParameter;
    }
    if (_bau == nullptr) {
        return ErrorCode::OperationNotReady;
    }
    return _bau->setGroupObjectTransmitPolicy(GroupObjectIndex(slot), policy);
}

Result<application::GroupObjectTransmitPolicy> ProductPortCore::transmitPolicy(uint16_t slot) const
{
    if (slot >= _portCount) {
        return ErrorCode::InvalidParameter;
    }
    if (_bau == nullptr) {
        return ErrorCode::OperationNotReady;
    }
    return _bau->groupObjectTransmitPolicy(GroupObjectIndex(slot));
}

bool ProductPortCore::isPortLinked(uint16_t slot) const
{
    if (slot >= _portCount || _bau == nullptr) {
        return false;
    }
    return _bau->isGroupObjectLinked(GroupObjectIndex(slot));
}

// ── Start-up ─────────────────────────────────────────────────────────────────

Result<void> ProductPortCore::registerGroupObjects()
{
    // One allocation for all objects and one for all values, sized from the ports.
    size_t valueBytes = 0;
    for (uint16_t slot = 0; slot < _portCount; ++slot) {
        valueBytes += application::groupObjectValueBytes(_ports[slot].dpt);
    }
    _bau->groupObjectTable().reserve(_portCount, valueBytes);

    for (uint16_t slot = 0; slot < _portCount; ++slot) {
        const PortDescriptor& port = _ports[slot];

        application::GroupObjectConfig config{};
        config.address = _configuredAddresses[slot];
        config.dpt = port.dpt;
        config.flags.communication = true;
        config.flags.read = port.readable();
        config.flags.write = port.writable();
        config.flags.transmit = port.transmit();
        config.flags.update = port.receivable();
        config.flags.readOnInit = port.readOnInit();
        config.flags.priority = port.priority;

        const GroupObjectIndex index = _bau->groupObjectTable().addGroupObject(config);
        if (!index.isValid() || index.value() != slot) {
            return ErrorCode::ResourceUnavailable;
        }
    }

    return Result<void>::ok();
}

void ProductPortCore::wireCallbacks()
{
    _bau->setGroupObjectWriteCallback([this](GroupObjectIndex slot, std::span<const uint8_t> /*data*/) {
        (void)dispatchWrite(slot);
    });

    _bau->setGroupObjectReadCallback([this](GroupObjectIndex slot) {
        (void)prepareRead(slot);
    });
}

Result<void> ProductPortCore::applyConfiguredGroupAddresses()
{
    for (uint16_t slot = 0; slot < _portCount; ++slot) {
        const GroupAddress address = _configuredAddresses[slot];
        if (!address.isValid()) {
            continue;
        }

        const auto bind = _bau->bindGroupObjectToAddress(GroupObjectIndex(slot), address);
        if (bind.isError()) {
            return bind.error();
        }
    }

    return Result<void>::ok();
}

void ProductPortCore::seedReadableState()
{
    for (uint16_t slot = 0; slot < _portCount; ++slot) {
        (void)prepareRead(GroupObjectIndex(slot));
    }
}

// ── Outbound ─────────────────────────────────────────────────────────────────

Result<void> ProductPortCore::publish(uint16_t slot, const DptValue& value)
{
    if (slot >= _portCount) {
        return ErrorCode::InvalidParameter;
    }
    if (!_ports[slot].transmit()) {
        return ErrorCode::AccessDenied;
    }
    if (isInitialIndividualAddress(individualAddress())) {
        return ErrorCode::OperationNotReady;
    }

    if (_bau != nullptr) {
        return publishStarted(slot, value);
    }
    return enqueue(PendingBusActionKind::Publish, slot, value);
}

Result<void> ProductPortCore::respond(uint16_t slot)
{
    if (slot >= _portCount) {
        return ErrorCode::InvalidParameter;
    }
    const PortDescriptor& port = _ports[slot];
    if (!port.readable()) {
        return ErrorCode::AccessDenied;
    }
    if (isInitialIndividualAddress(individualAddress())) {
        return ErrorCode::OperationNotReady;
    }

    DptValue state;
    if (!readState(port, state)) {
        return ErrorCode::OperationNotReady;
    }

    if (_bau != nullptr) {
        return respondStarted(slot, state);
    }
    return enqueue(PendingBusActionKind::ReadResponse, slot, state);
}

Result<void> ProductPortCore::publishStarted(uint16_t slot, const DptValue& value)
{
    if (isInitialIndividualAddress(individualAddress())) {
        return ErrorCode::OperationNotReady;
    }

    const GroupObjectIndex index(slot);
    const auto setValue = _bau->setGroupObjectValue(index, value);
    if (setValue.isError()) {
        return setValue.error();
    }

    const auto* object = _bau->groupObject(index);
    if (object == nullptr) {
        return ErrorCode::InvalidAddress;
    }

    return _bau->sendGroupValueForObject(index, object->getRawValue());
}

Result<void> ProductPortCore::respondStarted(uint16_t slot, const DptValue& value)
{
    if (isInitialIndividualAddress(individualAddress())) {
        return ErrorCode::OperationNotReady;
    }

    const GroupObjectIndex index(slot);
    const auto setValue = _bau->setGroupObjectValue(index, value);
    if (setValue.isError()) {
        return setValue.error();
    }

    const auto* object = _bau->groupObject(index);
    if (object == nullptr) {
        return ErrorCode::InvalidAddress;
    }

    return _bau->respondGroupValueForObject(index, object->getRawValue());
}

void ProductPortCore::drainPendingActions()
{
    if (_bau == nullptr) {
        return;
    }

    while (_pendingCount > 0u) {
        const PendingPortAction action = _pending[0];
        for (uint16_t index = 1u; index < _pendingCount; ++index) {
            _pending[index - 1u] = _pending[index];
        }
        --_pendingCount;

        if (action.kind == PendingBusActionKind::Publish) {
            (void)publishStarted(action.slot, action.value);
        } else {
            (void)respondStarted(action.slot, action.value);
        }
    }
}

Result<void> ProductPortCore::enqueue(PendingBusActionKind kind, uint16_t slot, const DptValue& value)
{
    // A capacity is a promise: a full queue refuses the request, it never drops an older one.
    if (_pendingCount >= _pendingCapacity) {
        return ErrorCode::QueueFull;
    }

    PendingPortAction& action = _pending[_pendingCount];
    action.kind = kind;
    action.logicalId = _ports[slot].logicalId;
    action.slot = slot;
    action.value = value;
    ++_pendingCount;
    return Result<void>::ok();
}

const PendingPortAction* ProductPortCore::pendingActionAt(size_t index) const
{
    return index < _pendingCount ? &_pending[index] : nullptr;
}

// ── Inbound ──────────────────────────────────────────────────────────────────

Result<void> ProductPortCore::dispatchWrite(GroupObjectIndex slot)
{
    if (_bau == nullptr || slot.value() >= _portCount) {
        return ErrorCode::InvalidAddress;
    }

    const auto value = _bau->groupObjectValue(slot);
    if (value.isError()) {
        return value.error();
    }

    return dispatchValue(slot.value(), value.value());
}

Result<void> ProductPortCore::dispatchValue(uint16_t slot, const DptValue& value)
{
    if (slot >= _portCount) {
        return ErrorCode::InvalidParameter;
    }
    const PortDescriptor& port = _ports[slot];
    if (!port.acceptsWrites()) {
        return ErrorCode::AccessDenied;
    }

    // The command handler first, then the state-write handler; only ports with
    // a state (the first stateWriteCount receive ranks) have the second.
    bool handled = false;
    const uint16_t rank = port.receiveRank;
    detail::ReceiveBinding* const entries[2] = {
        &_bindings.commands[rank],
        rank < _bindings.stateWriteCount ? &_bindings.stateWrites[rank] : nullptr,
    };
    for (detail::ReceiveBinding* entry : entries) {
        if (entry == nullptr || !*entry) {
            continue;
        }
        if (!entry->invoke(entry->self(), &value)) {
            return ErrorCode::DecodeFailed;
        }
        handled = true;
    }

    if (!handled) {
        return ErrorCode::OperationNotReady;
    }
    return Result<void>::ok();
}

bool ProductPortCore::readState(const PortDescriptor& port, DptValue& out) const
{
    const detail::StateBinding& entry = _bindings.providers[port.stateRank];
    return entry && entry.invoke(entry.self(), &out);
}

Result<void> ProductPortCore::prepareRead(GroupObjectIndex slot)
{
    if (_bau == nullptr || slot.value() >= _portCount) {
        return ErrorCode::InvalidAddress;
    }

    const PortDescriptor& port = _ports[slot.value()];
    if (!port.readable()) {
        return Result<void>::ok();
    }

    DptValue state;
    if (!readState(port, state)) {
        return ErrorCode::OperationNotReady;
    }

    return _bau->setGroupObjectValue(slot, state);
}

} // namespace knx::product
