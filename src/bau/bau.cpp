// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bau.cpp
 * @brief BusAccessUnit: the composition root and public façade.
 *
 * The BAU owns the six system interface objects and the stack port, and is
 * composed of three components, each a private header in src/bau/:
 *
 *   DeviceModel       (device_model.cpp)      interface-object inventory,
 *                     reference objects, property providers, coalesced
 *                     persistence, Data Secure key maps
 *   GroupRuntime      (group_runtime.cpp)     group objects, transmit shaping,
 *                     read-on-init, inbound queue, automatic responses;
 *                     backs TransmissionControl
 *   DeviceManagement  (device_management.cpp, go_diagnostics.cpp)
 *                     Function Properties, restart and Master Reset, router
 *                     role, KNXnet/IP Parameter Object; backs ManagementControl
 *
 * and the lower stack behind BusAccessStackPort (bau_stack_port.cpp), which
 * backs LinkAccess. This file holds the lifecycle — init() wires the
 * components in an order that matters, so it is written out phase by phase —
 * and the one-line forwards of the public surface.
 */

#include "knx/bau/bau.hpp"

#include "device_management.hpp"
#include "device_model.hpp"
#include "group_runtime.hpp"
#include "knx/util/log.hpp"

#include <utility>

namespace knx {
namespace bau {

namespace {
constexpr const char* TAG = "KNX.BAU";
}

#if KNX_FEATURE_TP1
BusAccessUnit::BusAccessUnit(platform::Platform& platform,
                             std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
    : BusAccessUnit(platform, createTp1StackPort(platform, std::move(mediumBackend)))
{
}
#endif

BusAccessUnit::BusAccessUnit(platform::Platform& platform,
                             std::unique_ptr<BusAccessStackPort> stackPort)
    : _platform(platform)
    , _stackPort(std::move(stackPort))
    , _model(std::make_unique<DeviceModel>(_deviceObject,
                                           _addressTable,
                                           _associationTable,
                                           _applicationProgram,
                                           _groupObjectTable,
                                           _securityObject,
                                           _stackPort.get()))
    , _groups(std::make_unique<GroupRuntime>(_platform, _stackPort.get(), *_model))
    , _management(std::make_unique<DeviceManagement>(_platform, _stackPort.get(), *_model, *_groups))
{
}

BusAccessUnit::~BusAccessUnit() {
    close();
}

util::Result<void> BusAccessUnit::init(const IndividualAddress& ownAddress,
                                      std::string_view persistenceNamespace,
                                      uint16_t persistenceSchemaVersion) {
    if (_initialized) {
        KNX_LOGW(TAG, "Already initialized");
        return util::Result<void>::ok();
    }

    if (!_stackPort) {
        KNX_LOGE(TAG, "Cannot initialize BAU without a stack port");
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    if (!_groups->inboundQueueReady()) {
        KNX_LOGE(TAG, "Cannot initialize BAU without an inbound group event queue");
        return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
    }

    // Phase 1 — clean slate for the group runtime.
    _groups->resetQueues();
    _groups->resetWorkAvailability();

    // Phase 2 — the lower stack, on the address the product starts with.
    _deviceObject.setIndividualAddress(ownAddress);
    _management->setDefaultIndividualAddress(ownAddress);

    auto stackInit = _stackPort->init(ownAddress);
    if (!stackInit) {
        KNX_LOGE(TAG, "Failed to initialize BAU lower stack: %s",
                 util::errorCodeToString(stackInit.error()));
        return stackInit;
    }
    _management->applyDeviceMedium();

    // Phase 3 — Device Object ↔ stack: programming mode, serial number and
    // individual address, in both directions. Before the restore in phase 6,
    // so the restored address reaches the stack (once, not half-restored).
    _management->wireDeviceObject();

#if KNX_SECURE_ENABLED
    _stackPort->configureDataSecure(_securityObject);
#endif

    // Phase 4 — management services. This also records which reference
    // objects the medium needs; they are created in phase 6, so the first
    // publication only declares them, and phase 7 seeds them.
    _management->wireManagementServices();

    // Phase 5 — group services from the stack into the group runtime.
    _groups->attachToStackPort();

    // Phase 6 — the interface-object inventory and what was persisted in it.
    const auto modelInit = _model->init(persistenceNamespace, persistenceSchemaVersion);
    if (modelInit.isError()) {
        KNX_LOGE(TAG, "Interface object manager init failed: %s", util::errorCodeToString(modelInit.error()));
    } else {
        // Restore KNX tables (address/association/group object) saved from a
        // previous ETS commissioning session.
        const size_t loaded = _model->restore();
        KNX_LOGI(TAG, "Restored %zu properties from persistence", loaded);

        const auto restoredAddress = _deviceObject.readIndividualAddress();
        auto addressSync = _stackPort->setOwnAddress(restoredAddress);
        if (addressSync.isError()) {
            KNX_LOGW(TAG, "Failed to sync restored own address to stack: %s",
                     util::errorCodeToString(addressSync.error()));
        }
        _stackPort->setProgrammingModeEnabled(_deviceObject.getProgMode());

        // Re-subscribe all group addresses from the restored address table so
        // the data link layer accepts incoming telegrams on those addresses.
        _model->resubscribeGroupAddresses();

        // The key tables are restored as opaque blobs like every other
        // property, so the derived key maps have to be rebuilt from them —
        // after the address table is back, since group keys resolve through
        // it. Without this the device boots into Security Mode with no
        // usable group key and quietly sends everything in plain.
        _model->applySecurityKeyTables();

        // The per-partner Last Valid SeqNr values came back inside the
        // Security Individual Address Table that applySecurityKeyTables()
        // just adopted; the Sequence Number for Tool Access is not a
        // property and is restored from its own record (03/05/01 §6.2).
        (void)_model->loadSecuritySequenceState();

        // The A_Key_Write key table (03/03/07 §3.5.8) is application
        // layer state; without it a device keyed by ETS would be open
        // again after every restart.
        if (const auto keys = _model->loadAuthorizationKeyTable()) {
            _stackPort->setAuthorizationKeys(*keys);
        }

        _model->bind();

        // Phase 7 — the reference objects exist only from phase 6, but phase 4
        // published the medium objects before it, when there was nothing yet
        // to seed or to hang the write hooks on: the KNXnet/IP Parameter
        // Object then answered ETS with zeros and PID_BACKBONE_KEY /
        // PID_ROUTING_MULTICAST_ADDRESS writes went unobserved. Published
        // again now that they exist, and after the restore, so the seeds see
        // the restored address and the stored values coming back are not
        // mistaken for writes.
        _management->publishMediumInterfaceObjects();
    }

    // Phase 8 — run.
    // 03/05/01 §4.2.14.4: "The device shall set the value of this Property
    // to 00h on start-up." PID_DEVICE_CONTROL is persisted with the rest of
    // the Device Object, so without this a Verify Mode that ETS switched on
    // once would outlive the connection and every reboot after it.
    _deviceObject.setDeviceControl(0u);

    (void)_deviceObject.processLoadEvent(objects::LoadEvent::StartLoad);
    (void)_deviceObject.processLoadEvent(objects::LoadEvent::LoadComplete);
    (void)_deviceObject.processLoadEvent(objects::LoadEvent::LoadComplete);
    _deviceObject.setRunState(objects::RunState::Running);
    _initialized = true;
    _groups->setRunning(true);
    _management->setRunning(true);

    // Report the address actually in effect, not the `ownAddress` parameter:
    // the restore above may have brought back a programmed address (e.g.
    // 1.1.3), while `ownAddress` still holds the pre-restore default.
    const IndividualAddress effectiveAddress = _deviceObject.readIndividualAddress();
    KNX_LOGI(TAG, "BAU initialized, address: %d.%d.%d",
             effectiveAddress.area(), effectiveAddress.line(), effectiveAddress.device());

    return util::Result<void>::ok();
}

void BusAccessUnit::close() {
    if (_initialized) {
        if (_stackPort) {
            _stackPort->close();
        }

        _groups->resetQueues();

        _deviceObject.setRunState(objects::RunState::Halted);
        (void)_deviceObject.processLoadEvent(objects::LoadEvent::StartUnload);
        (void)_deviceObject.processLoadEvent(objects::LoadEvent::UnloadComplete);
        _initialized = false;
        _groups->setRunning(false);
        _management->setRunning(false);
    }

    _groups->resetWorkAvailability();
}

void BusAccessUnit::loop() {
    // Coalesced NVS flush. Runs before executePendingRestart() so a burst that
    // has just gone quiet is written before a queued restart takes the device
    // down; a restart arriving mid-burst is covered by the explicit flush in
    // the restart handler.
    const uint32_t now = _groups->nowMs();
    _model->pumpPersistence(now);
    _model->pumpSecuritySequenceCheckpoint(now);

    // Service deferred lower-layer work outside callback context.
    if (_stackPort) {
        _stackPort->executePendingRestart();
        _stackPort->processBackgroundWork();
    }

    _groups->processInboundGroupEvents();
    _groups->pumpReadOnInit();
    _groups->pumpGroupObjectTransmissions(_groups->nowMs());
    _groups->refreshWorkAvailabilityState();
}

void BusAccessUnit::flushPendingPersistence() {
    _model->flushPersistenceIfDirty();
}

BusAccessUnit::OwnerWorkHint BusAccessUnit::ownerWorkHint() const {
    return _groups->ownerWorkHint();
}

void BusAccessUnit::setWorkAvailableCallback(WorkAvailableCallback callback) {
    _groups->setWorkAvailableCallback(std::move(callback));
}

// ─── Properties ──────────────────────────────────────────────────────────────

util::Result<size_t> BusAccessUnit::readProperty(InterfaceObjectType objectType,
                                                 InterfaceObjectInstance objectInstance,
                                                 application::PropertyID propertyId,
                                                 uint16_t startIndex,
                                                 uint8_t elementCount,
                                                 std::span<uint8_t> out)
{
    return _model->readProperty(objectType, objectInstance, propertyId, startIndex, elementCount, out);
}

util::Result<void> BusAccessUnit::writeProperty(InterfaceObjectType objectType,
                                                InterfaceObjectInstance objectInstance,
                                                application::PropertyID propertyId,
                                                uint16_t startIndex,
                                                std::span<const uint8_t> data)
{
    return _model->writeProperty(objectType, objectInstance, propertyId, startIndex, data);
}

// ─── Group objects (GroupRuntime) ────────────────────────────────────────────

application::GroupObject* BusAccessUnit::groupObject(GroupObjectIndex index) {
    return _groups->groupObject(index);
}

const application::GroupObject* BusAccessUnit::groupObject(GroupObjectIndex index) const {
    return _groups->groupObject(index);
}

GroupObjectIndex BusAccessUnit::addGroupObject(const GroupAddress& address,
                                               application::DptId dpt,
                                               const application::GroupObjectFlags& flags) {
    return _groups->addGroupObject(address, dpt, flags);
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
    return _groups->addGroupObject(address, dpt, flags);
}

util::Result<void> BusAccessUnit::bindGroupObjectToAddress(GroupObjectIndex index, const GroupAddress& address) {
    return _groups->bindGroupObjectToAddress(index, address);
}

bool BusAccessUnit::isGroupObjectValid(GroupObjectIndex index) const {
    return _groups->isGroupObjectValid(index);
}

size_t BusAccessUnit::groupObjectAssociationCount(GroupObjectIndex index) const {
    return _groups->groupObjectAssociationCount(index);
}

bool BusAccessUnit::isGroupObjectLinked(GroupObjectIndex index) const {
    return _groups->isGroupObjectLinked(index);
}

util::Result<application::DptValue> BusAccessUnit::groupObjectValue(GroupObjectIndex index) const {
    return _groups->groupObjectValue(index);
}

util::Result<void> BusAccessUnit::setGroupObjectValue(GroupObjectIndex index, const application::DptValue& value) {
    return _groups->setGroupObjectValue(index, value);
}

util::Result<void> BusAccessUnit::requestGroupObjectValue(GroupObjectIndex index) {
    return _groups->requestGroupObjectValue(index);
}

void BusAccessUnit::setGroupObjectWriteCallback(GroupObjectWriteCallback callback) {
    _groups->setGroupObjectWriteCallback(std::move(callback));
}

void BusAccessUnit::setGroupObjectReadCallback(GroupObjectReadCallback callback) {
    _groups->setGroupObjectReadCallback(std::move(callback));
}

util::Result<void> BusAccessUnit::sendGroupValue(const GroupAddress& address, std::span<const uint8_t> data) {
    return _groups->sendGroupValue(address, data);
}

util::Result<void> BusAccessUnit::beginSendGroupValue(const GroupAddress& address, std::span<const uint8_t> data) {
    return _groups->beginSendGroupValue(address, data);
}

util::Result<void> BusAccessUnit::sendGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data) {
    return _groups->sendGroupValueForObject(objectIndex, data);
}

util::Result<void> BusAccessUnit::respondGroupValueForObject(GroupObjectIndex objectIndex, std::span<const uint8_t> data) {
    return _groups->respondGroupValueForObject(objectIndex, data);
}

void BusAccessUnit::setTimeSource(TimeSourceFn timeSource) {
    _groups->setTimeSource(std::move(timeSource));
}

void BusAccessUnit::setTelegramRateLimit(const application::TelegramRateLimitConfig& config) {
    _groups->setTelegramRateLimit(config);
}

util::Result<void> BusAccessUnit::setGroupObjectTransmitPolicy(GroupObjectIndex objectIndex,
                                                               const application::GroupObjectTransmitPolicy& policy) {
    return _groups->setGroupObjectTransmitPolicy(objectIndex, policy);
}

util::Result<application::GroupObjectTransmitPolicy>
BusAccessUnit::groupObjectTransmitPolicy(GroupObjectIndex objectIndex) const {
    return _groups->groupObjectTransmitPolicy(objectIndex);
}

util::Result<void> BusAccessUnit::requestGroupValue(const GroupAddress& address) {
    return _groups->requestGroupValue(address);
}

util::Result<void> BusAccessUnit::beginRequestGroupValue(const GroupAddress& address) {
    return _groups->beginRequestGroupValue(address);
}

util::Result<BusAccessUnit::TransmissionProgressState> BusAccessUnit::pollOutboundTransmission() {
    return _groups->pollOutboundTransmission();
}

// ─── Facets ──────────────────────────────────────────────────────────────────

TransmissionControl BusAccessUnit::transmission() { return TransmissionControl(*_groups); }
ConstTransmissionControl BusAccessUnit::transmission() const { return ConstTransmissionControl(*_groups); }
ManagementControl BusAccessUnit::management() { return ManagementControl(*_management); }
ConstManagementControl BusAccessUnit::management() const { return ConstManagementControl(*_management); }
LinkAccess BusAccessUnit::link() { return LinkAccess(_stackPort.get()); }
ConstLinkAccess BusAccessUnit::link() const { return ConstLinkAccess(_stackPort.get()); }

util::Result<void> LinkAccess::sendFrame(const datalink::LDataFrame& frame) {
    return _stackPort != nullptr ? _stackPort->sendDataLinkFrame(frame)
                                 : util::Result<void>::err(util::ErrorCode::InvalidParameter);
}

void LinkAccess::setPromiscuousMode(datalink::PromiscuousMode mode) {
    if (_stackPort != nullptr) {
        _stackPort->setDataLinkPromiscuousMode(mode);
    }
}

network::TwoPortCoupler* LinkAccess::coupler() {
    return _stackPort != nullptr ? _stackPort->coupler() : nullptr;
}

util::Result<void> LinkAccess::refreshMulticastMembership() {
    return _stackPort != nullptr ? _stackPort->refreshMulticastMembership()
                                 : util::Result<void>::err(util::ErrorCode::NotInitialized);
}

LinkDiagnostics LinkAccess::diagnostics() const {
    return _stackPort != nullptr ? _stackPort->linkDiagnostics() : LinkDiagnostics{};
}

} // namespace bau
} // namespace knx
