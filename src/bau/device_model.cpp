// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file device_model.cpp
 * @brief DeviceModel: interface-object inventory, property providers,
 *        coalesced persistence and the Data Secure key maps.
 */

#include "device_model.hpp"

#include "knx/application/apci_services.hpp"
#include "knx/objects/interface_object.hpp"
#include "knx/objects/reference_object_registry.hpp"
#if KNX_SECURE_ENABLED
#include "knx/objects/security_interface_object.hpp"
#endif
#include "knx/util/log.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace knx {
namespace bau {

namespace {

constexpr const char* TAG = "KNX.BAU";

bool isTableObject(InterfaceObjectType objectType)
{
    return objectType == InterfaceObjectType::addressTable()
        || objectType == InterfaceObjectType::associationTable()
        || objectType == InterfaceObjectType::groupObjectTable()
        || objectType == InterfaceObjectType::applicationProgram();
}

} // namespace

DeviceModel::DeviceModel(objects::DeviceObject& deviceObject,
                         objects::AddressTableObject& addressTable,
                         objects::AssociationTableObject& associationTable,
                         objects::ApplicationProgramObject& applicationProgram,
                         objects::GroupObjectTableObject& groupObjectTable,
#if KNX_SECURE_ENABLED
                         objects::SecurityInterfaceObject& securityObject,
#endif
                         BusAccessStackPort* stackPort)
    : _deviceObject(deviceObject)
    , _addressTable(addressTable)
    , _associationTable(associationTable)
    , _applicationProgram(applicationProgram)
    , _groupObjectTable(groupObjectTable)
#if KNX_SECURE_ENABLED
    , _securityObject(securityObject)
#endif
    , _stackPort(stackPort)
{
}

// ─── Start-up ────────────────────────────────────────────────────────────────

util::Result<void> DeviceModel::init(std::string_view persistenceNamespace, uint16_t schemaVersion)
{
    if (!_interfaceObjectManager) {
        _interfaceObjectManager = std::make_unique<objects::InterfaceObjectManager>(
            _deviceObject,
            _addressTable,
            _associationTable,
            _applicationProgram,
            _groupObjectTable
#if KNX_SECURE_ENABLED
            , _securityObject
#endif
            );
        const auto initRes = _interfaceObjectManager->init(true, persistenceNamespace, schemaVersion);
        if (initRes.isError()) {
            return initRes;
        }
    }

    instantiateReferenceObjects();
    return util::Result<void>::ok();
}

size_t DeviceModel::restore()
{
    if (!_interfaceObjectManager) {
        return 0;
    }
    _restoring = true;
    const size_t loaded = _interfaceObjectManager->loadFromPersistence();
    _restoring = false;
    return loaded;
}

size_t DeviceModel::restoreReferenceObject(InterfaceObjectType objectType)
{
    if (!_interfaceObjectManager) {
        return 0;
    }
    _restoring = true;
    const size_t loaded = _interfaceObjectManager->loadObjectFromPersistence(objectType);
    _restoring = false;
    return loaded;
}

void DeviceModel::bind()
{
    if (_stackPort == nullptr) {
        return;
    }
    BusAccessStackPort& stackPort = *_stackPort;

    // Object INDEX order must match the canonical SystemB (mask 07B0)
    // interface object layout that ETS assumes when it synthesizes the
    // DefaultProcedure download: 0 = Device, 1 = Group Address Table,
    // 2 = Association Table, 3 = Group Object Table, 4 = Application
    // Program. ETS addresses load-state writes by these indices.
    std::vector<objects::InterfaceObject*> interfaceObjects = {
        &_deviceObject,
        &_addressTable,
        &_associationTable,
        &_groupObjectTable,
        &_applicationProgram
#if KNX_SECURE_ENABLED
        , &_securityObject
#endif
    };
    interfaceObjects.reserve(interfaceObjects.size() + _referenceObjects.size());
    for (const auto& ref : _referenceObjects) {
        interfaceObjects.push_back(ref.get());
    }

    _indexToTypeMap.clear();
    _indexToTypeMap.reserve(interfaceObjects.size());

    for (size_t i = 0; i < interfaceObjects.size(); ++i) {
        objects::InterfaceObject* object = interfaceObjects[i];
        const InterfaceObjectIndex objectIndex(static_cast<uint8_t>(i));

        // Both property service families and the Access Policy answer
        // from the interface objects through the providers below; the
        // application layer only needs to know which type sits at which
        // index.
        stackPort.registerPropertyObject(object->objectType(), objectIndex);
        _indexToTypeMap.push_back(object->objectType());
    }

    stackPort.setPropertyReadProvider(
        [this](const IndividualAddress& /*source*/, const application::PropertyValueReadRequest& request, std::span<uint8_t> out)
            -> std::optional<util::Result<size_t>> {
            application::PropertyServiceDataBuffer data;
            if (out.size() > data.capacity()) {
                return util::ErrorCode::BufferTooSmall;
            }
            const auto objectType = objectTypeForIndex(request.objectIndex);
            if (!objectType.has_value()) {
                return std::nullopt;
            }
            const auto res = _interfaceObjectManager->readProperty(
                *objectType,
                InterfaceObjectInstance(1),
                request.propertyId,
                request.startIndex,
                request.elementCount,
                data);
            if (res.isError()) {
                return std::nullopt;
            }
            std::copy(data.begin(), data.end(), out.begin());
            return util::Result<size_t>(data.size());
        });

    stackPort.setPropertyWriteProvider(
        [this](const IndividualAddress& /*source*/, const application::PropertyValueWriteRequest& request) {
            return writeProperty(request);
        });

    // Memory-mapped table load segments (ETS SystemB DefaultProcedure):
    // ETS allocates a segment via PID_LOAD_STATE_CONTROL additional load
    // controls, reads its address from PID_TABLE_REFERENCE and streams the
    // table content with A_Memory_Write / verifies with A_Memory_Read.
    (void)stackPort.registerMemoryRegion(
        application::MemoryRegion{MemoryAddress(objects::tableseg::kGroupAddressTableBase),
                                  objects::tableseg::kGroupAddressTableSize,
                                  application::MemoryAccessMode::ReadWrite, "GrAT"},
        std::span<uint8_t>(_groupAddressSegment));
    (void)stackPort.registerMemoryRegion(
        application::MemoryRegion{MemoryAddress(objects::tableseg::kAssociationTableBase),
                                  objects::tableseg::kAssociationTableSize,
                                  application::MemoryAccessMode::ReadWrite, "AssocTable"},
        std::span<uint8_t>(_associationSegment));
    (void)stackPort.registerMemoryRegion(
        application::MemoryRegion{MemoryAddress(objects::tableseg::kGroupObjectTableBase),
                                  objects::tableseg::kGroupObjectTableSize,
                                  application::MemoryAccessMode::ReadWrite, "GrOT"},
        std::span<uint8_t>(_groupObjectSegment));
    (void)stackPort.registerMemoryRegion(
        application::MemoryRegion{MemoryAddress(objects::tableseg::kApplicationCodeBase),
                                  objects::tableseg::kApplicationCodeSize,
                                  application::MemoryAccessMode::ReadWrite, "AppCode"},
        std::span<uint8_t>(_applicationCodeSegment));

    stackPort.setPropertyDescriptionProvider(
        [this](const IndividualAddress& /*source*/, InterfaceObjectIndex objectIndex, application::PropertyID propertyId, PropertyIndex propertyIndex)
            -> std::optional<application::PropertyDescriptionInfo> {
            const auto objectType = objectTypeForIndex(objectIndex);
            if (!objectType.has_value()) {
                return std::nullopt;
            }

            objects::PropertyDescription description;
            const auto res = _interfaceObjectManager->describeProperty(
                *objectType,
                InterfaceObjectInstance(1),
                propertyId,
                propertyIndex.value(),
                description);
            if (res.isError()) {
                return std::nullopt;
            }

            application::PropertyDescriptionInfo info;
            info.objectIndex = objectIndex;
            info.propertyId = description.id;
            // 03/03/07 §3.4.3.1: "For new implementations the
            // property_index shall contain the correct value of the
            // addressed Property", also when it was addressed by PID.
            info.propertyIndex = PropertyIndex(static_cast<uint8_t>(description.index));
            info.type = description.type;
            info.maxElements = description.maxElements;
            info.writeAccess = objects::canWrite(description.capability) ? PropertyWriteAccess::Allowed
                                                                         : PropertyWriteAccess::Denied;
            info.readable = objects::canRead(description.capability);
            info.readLevel = description.readLevel;
            info.writeLevel = description.writeLevel;
            return info;
        });
}

// ─── Reference interface objects ─────────────────────────────────────────────

void DeviceModel::setReferenceObjectTypes(std::vector<InterfaceObjectType> objectTypes)
{
    _referenceObjectTypes.clear();
    _referenceObjectTypes.reserve(objectTypes.size());

    for (const auto objectType : objectTypes) {
        if (!objects::isReferenceObjectType(objectType)) {
            continue;
        }
        if (std::find(_referenceObjectTypes.begin(), _referenceObjectTypes.end(), objectType)
            != _referenceObjectTypes.end()) {
            continue;
        }
        _referenceObjectTypes.push_back(objectType);
    }

    // A type added after init() still needs its object; before init() this
    // is a no-op and init() does the work.
    instantiateReferenceObjects();
}

/**
 * Create and register a GenericInterfaceObject for every configured
 * reference type that does not have one yet.
 *
 * Incremental on purpose. This used to run only when the list was empty,
 * so a type added after init() — which is exactly what
 * configureRouterRole() does when a coupler is configured at runtime — was
 * recorded but never instantiated. Every property access on it then failed
 * with InvalidAddress, and because the seeding path ignored those errors
 * the Router Object silently answered nothing.
 */
void DeviceModel::instantiateReferenceObjects()
{
    if (!_interfaceObjectManager) {
        return;
    }

    for (const auto objectType : _referenceObjectTypes) {
        if (referenceObject(objectType) != nullptr) {
            continue;
        }
        // GenericInterfaceObject builds its handlers from referenceObjectProperties().
        _referenceObjects.emplace_back(std::make_unique<objects::GenericInterfaceObject>(objectType));
        if (_interfaceObjectManager->registerReferenceObject(*_referenceObjects.back()).isError()) {
            // The manager has room for one object of every reference
            // type, so this is a sizing bug; the manager logged it.
            _referenceObjects.pop_back();
        }
    }
}

objects::GenericInterfaceObject* DeviceModel::referenceObject(InterfaceObjectType type)
{
    for (auto& object : _referenceObjects) {
        if (object && object->objectType() == type) {
            return object.get();
        }
    }
    return nullptr;
}

const objects::GenericInterfaceObject* DeviceModel::referenceObject(InterfaceObjectType type) const
{
    for (const auto& object : _referenceObjects) {
        if (object && object->objectType() == type) {
            return object.get();
        }
    }
    return nullptr;
}

util::Result<void> DeviceModel::setReferenceObjectProperty(InterfaceObjectType objectType,
                                                           application::PropertyID propertyId,
                                                           std::span<const uint8_t> value)
{
    // Reference objects are created by init(); before it there is nothing to
    // hold the value, and the restore would replace it anyway.
    if (!_interfaceObjectManager) {
        return util::ErrorCode::NotInitialized;
    }
    auto* object = referenceObject(objectType);
    if (object == nullptr) {
        return util::ErrorCode::InvalidAddress;
    }
    return object->setPropertyValue(propertyId, value);
}

std::vector<uint8_t> DeviceModel::referenceObjectPropertyValue(InterfaceObjectType objectType,
                                                               application::PropertyID propertyId) const
{
    const auto* object = referenceObject(objectType);
    if (object == nullptr) {
        return {};
    }
    return object->propertyValue(propertyId);
}

// ─── Property access ─────────────────────────────────────────────────────────

util::Result<size_t> DeviceModel::readProperty(InterfaceObjectType objectType,
                                               InterfaceObjectInstance objectInstance,
                                               application::PropertyID propertyId,
                                               uint16_t startIndex,
                                               uint8_t elementCount,
                                               std::span<uint8_t> out)
{
    if (!_interfaceObjectManager) {
        return util::ErrorCode::NotInitialized;
    }

    application::PropertyServiceDataBuffer data;
    if (out.size() > data.capacity()) {
        return util::ErrorCode::BufferTooSmall;
    }

    const auto res = _interfaceObjectManager->readProperty(objectType,
                                                           objectInstance,
                                                           propertyId,
                                                           startIndex,
                                                           elementCount,
                                                           data);
    if (res.isError()) {
        return res.error();
    }
    if (data.size() > out.size()) {
        return util::ErrorCode::BufferTooSmall;
    }

    std::copy(data.begin(), data.end(), out.begin());
    return data.size();
}

util::Result<void> DeviceModel::writeProperty(InterfaceObjectType objectType,
                                              InterfaceObjectInstance objectInstance,
                                              application::PropertyID propertyId,
                                              uint16_t startIndex,
                                              std::span<const uint8_t> data)
{
    if (!_interfaceObjectManager) {
        return util::ErrorCode::NotInitialized;
    }

    const auto res = _interfaceObjectManager->writeProperty(objectType,
                                                            objectInstance,
                                                            propertyId,
                                                            startIndex,
                                                            data);
    if (res.isError()) {
        return res;
    }
    // Same reason as the bus-driven write path below: whatever is written
    // into the Security Interface Object is a credential the next
    // conversation depends on, and losing it across a restart leaves the
    // device answering with the wrong key. The local API is now the only
    // way to install one — the wire path refuses unsecured writes.
#if KNX_SECURE_ENABLED
    if (objectType == InterfaceObjectType::security()) {
        markPersistenceDirty();
        applySecurityKeyTables();
    }
#endif
    if (objects::isReferenceObjectType(objectType)) {
        afterReferenceObjectWrite(objectType, propertyId);
    }
    return util::Result<void>::ok();
}

std::optional<InterfaceObjectType> DeviceModel::objectTypeForIndex(InterfaceObjectIndex objectIndex) const
{
    if (objectIndex.value() >= _indexToTypeMap.size()) {
        return std::nullopt;
    }
    return _indexToTypeMap[objectIndex.value()];
}

util::Result<void> DeviceModel::writeProperty(const application::PropertyValueWriteRequest& request)
{
    const auto objectType = objectTypeForIndex(request.objectIndex);
    if (!objectType.has_value()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto res = _interfaceObjectManager->writeProperty(
        *objectType,
        InterfaceObjectInstance(1),
        request.propertyId,
        request.startIndex,
        request.data.span());
    if (res.isError()) {
        return res;
    }
#if KNX_SECURE_ENABLED
    if (*objectType == InterfaceObjectType::security()) {
        afterSecurityObjectWrite();
    }
#endif
    if (isTableObject(*objectType)) {
        afterTableObjectWrite(*objectType, request);
    }
    if (objects::isReferenceObjectType(*objectType)) {
        afterReferenceObjectWrite(*objectType, request.propertyId);
    }
    return util::Result<void>::ok();
}

// A reference object's persisted properties (the KNXnet/IP Parameter
// Object's routing multicast address and Secure Routing settings) are
// written by ETS like any other and are just as useless if the restart
// that ends the download loses them. Relying on some other write in the
// same download to have marked the state dirty is what this replaces.
void DeviceModel::afterReferenceObjectWrite(InterfaceObjectType objectType, application::PropertyID propertyId)
{
    if (objects::InterfaceObjectManager::isPersistedProperty(objectType, propertyId)) {
        markPersistenceDirty();
    }
}

#if KNX_SECURE_ENABLED
// The Security Interface Object holds the credentials for every
// later conversation: ETS installs its own Tool Key and the
// Sequence Number Sending here and then immediately restarts the
// device. Losing them across that restart leaves the device
// answering with the factory key, and ETS reports "no
// SyncResponse was received; probably the key did not match".
// The restart path flushes what is marked dirty, so marking is
// enough — no synchronous write in the middle of the download.
void DeviceModel::afterSecurityObjectWrite()
{
    markPersistenceDirty();
    // PID_GRP_KEY_TABLE and PID_P2P_KEY_TABLE are stored as
    // opaque arrays; the Secure Application Layer looks keys up
    // by address, so the download only takes effect once they
    // are resolved against the address tables. Doing it on every
    // write to this object keeps it correct whichever order ETS
    // writes the tables in.
    applySecurityKeyTables();
}

#endif
// Persist table objects after every successful ETS write so
// the KNX tables survive reboot without explicit user action.
void DeviceModel::afterTableObjectWrite(InterfaceObjectType objectType,
                                        const application::PropertyValueWriteRequest& request)
{
    // ETS SystemB streams table content into the memory-mapped
    // segment and signals the end with a LoadCompleted event on
    // PID_LOAD_STATE_CONTROL — apply the segment to the table
    // domain before persisting.
    const bool isLoadControlWrite =
        request.propertyId == static_cast<application::PropertyID>(5)
        && !request.data.empty();
    const bool isLoadCompleted = isLoadControlWrite
        && request.data.span()[0] == objects::loadstate::kEventLoadCompleted;
    if (isLoadCompleted) {
        applyLoadedTableSegment(objectType);
    }
    // Every write to the address table can change what the data link layers
    // filter on (Unload and StartLoading empty it, a table write changes
    // it), so the list is published after each, not only at the end of a load.
    if (objectType == InterfaceObjectType::addressTable()) {
        resubscribeGroupAddresses();
    }
    // Persist only when table content can actually have changed:
    // a completed load or a direct table-data write. Intermediate
    // load-control events (unload/start/segment alloc) would
    // otherwise trigger a full NVS save each — flash wear and
    // latency in the middle of the ETS download.
    if (!isLoadControlWrite || isLoadCompleted) {
        markPersistenceDirty();
        if (objectType == InterfaceObjectType::addressTable()) {
            // Group keys are held against the GA_Index, so a new
            // address table re-points every one of them.
#if KNX_SECURE_ENABLED
            applySecurityKeyTables();
#endif
        }
    }
}

// Apply the content of a memory-mapped load segment (big-endian u16 count
// followed by the entries) to the corresponding table domain. Called when
// ETS writes the LoadCompleted event after streaming the segment.
void DeviceModel::applyLoadedTableSegment(InterfaceObjectType objectType)
{
    const auto u16at = [](std::span<const uint8_t> segment, size_t offset) -> uint16_t {
        return static_cast<uint16_t>((static_cast<uint16_t>(segment[offset]) << 8)
                                     | segment[offset + 1u]);
    };

    if (objectType == InterfaceObjectType::addressTable()) {
        const std::span<const uint8_t> segment(_groupAddressSegment);
        uint16_t count = u16at(segment, 0);
        if (count > objects::AddressTableDomain::kMaxEntries) {
            // A capacity is a promise: the table is refused, not cut short.
            // The load state goes to Error (03/05/01 §4.23), which ETS reads
            // back and reports as a failed download.
            KNX_LOGE(TAG, "Group address segment holds %u addresses, capacity is %u: load refused",
                     count, static_cast<unsigned>(objects::AddressTableDomain::kMaxEntries));
            _addressTable.clearEntries();
            _addressTable.setLoadState(objects::loadstate::kError);
            return;
        }
        _addressTable.clearEntries();
        for (uint16_t i = 0; i < count; ++i) {
            (void)_addressTable.addEntry(GroupAddress(u16at(segment, 2u + 2u * i)));
        }
        KNX_LOGI(TAG, "Applied %u group addresses from load segment", count);
        return;
    }

    if (objectType == InterfaceObjectType::associationTable()) {
        const std::span<const uint8_t> segment(_associationSegment);
        uint16_t count = u16at(segment, 0);
        if (count > objects::AssociationTableDomain::kMaxEntries) {
            KNX_LOGE(TAG, "Association segment holds %u entries, capacity is %u: load refused",
                     count, static_cast<unsigned>(objects::AssociationTableDomain::kMaxEntries));
            _associationTable.clearEntries();
            _associationTable.setLoadState(objects::loadstate::kError);
            return;
        }
        _associationTable.clearEntries();
        for (uint16_t i = 0; i < count; ++i) {
            const uint16_t tsap = u16at(segment, 2u + 4u * i);
            const uint16_t asap = u16at(segment, 4u + 4u * i);
            (void)_associationTable.addEntry(
                objects::AssociationEntry(AddressTableIndex(tsap), GroupObjectIndex(asap)));
        }
        KNX_LOGI(TAG, "Applied %u associations from load segment", count);
        return;
    }

    if (objectType == InterfaceObjectType::applicationProgram()) {
        // The application-program code segment (RS-0000) is the ETS
        // parameter memory: each knxprod <Parameter> carries a <Memory>
        // location in this segment, and ETS memory-writes the values during
        // download. Push the segment into the ApplicationProgramObject's
        // program data — notifyProgramDataChanged() drives the
        // wireParameterDataCallback → ParameterState::applyFromBytes chain,
        // firing the product's onParameterChanged callbacks with the new
        // values. Persistence is handled by the caller (saveToPersistence
        // runs right after this on LoadCompleted).
        // Only apply it if ETS actually downloaded it.  A segment nothing
        // ever wrote reads as all zeros, and applying that silently
        // replaces every firmware parameter default with 0 — a device
        // commissioned with 0 °C setpoints and 0 PID gains that looks like
        // a control bug much later.  ETS skips the parameter download
        // whenever the product's load procedure declares no segment for the
        // application program object, which is exactly what happened here.
        if (_stackPort == nullptr
            || !_stackPort->memoryRegionWritten(MemoryAddress(objects::tableseg::kApplicationCodeBase))) {
            KNX_LOGW(TAG,
                     "No parameter segment downloaded — keeping firmware defaults "
                     "(ETS wrote no memory at 0x%04X)",
                     static_cast<unsigned>(objects::tableseg::kApplicationCodeBase));
            return;
        }

        const std::span<const uint8_t> segment(_applicationCodeSegment);
        _applicationProgram.setProgramData(segment);
        _applicationProgram.notifyProgramDataChanged();
        KNX_LOGI(TAG, "Applied %zu-byte parameter segment from ETS download", segment.size());
        return;
    }

    // Group object table: content is firmware-defined (static
    // ComObjectTable). The segment bytes are accepted for the download to
    // complete but not applied.
}

void DeviceModel::resubscribeGroupAddresses()
{
    // The data link layers filter on the address table's storage; this is
    // the publication point after the table changed.
    if (_stackPort != nullptr) {
        _stackPort->publishGroupAddresses();
    }
}

// ─── Persistence ─────────────────────────────────────────────────────────────

util::Result<size_t> DeviceModel::saveToPersistence()
{
    if (!_interfaceObjectManager) {
        return util::ErrorCode::NotInitialized;
    }

    // A write landing while the save runs (the property providers run on the
    // stack task) bumps the sequence again, so it is still seen as pending.
    const uint32_t markSeq = _persistenceMarkSeq;
    const auto saved = _interfaceObjectManager->saveToPersistence();
    _persistenceBurstActive = false;
    _persistenceSeenSeq = markSeq;

    if (saved.isError()) {
        // Still dirty, whether or not it was before (the address callback
        // saves without marking): the state on flash is not the state in
        // memory, and a restart now would boot into the old one.
        _persistenceDirty = true;
        _persistenceRetryPending = true;
        _persistenceRetryDueMs = _lastPumpMs + kPersistRetryMs;
        KNX_LOGW(TAG, "Persistent state not saved (%s); retrying in %u ms",
                 util::errorCodeToString(saved.error()), static_cast<unsigned>(kPersistRetryMs));
        return saved;
    }

    _persistenceRetryPending = false;
    if (_persistenceMarkSeq == markSeq) {
        _persistenceDirty = false;
    }
    return saved;
}

void DeviceModel::markPersistenceDirty()
{
    _persistenceDirty = true;
    ++_persistenceMarkSeq;
}

void DeviceModel::pumpPersistence(uint32_t nowMs)
{
    _lastPumpMs = nowMs;
    if (!_persistenceDirty) {
        return;
    }
    if (_persistenceMarkSeq != _persistenceSeenSeq) {
        // Writes landed since the previous pump: restart the quiet window.
        _persistenceSeenSeq = _persistenceMarkSeq;
        _persistenceQuietStartMs = nowMs;
        if (!_persistenceBurstActive) {
            _persistenceBurstActive = true;
            _persistenceBurstStartMs = nowMs;
        }
        return;
    }
    if (_persistenceRetryPending) {
        if (static_cast<int32_t>(nowMs - _persistenceRetryDueMs) >= 0) {
            (void)saveToPersistence();  // a failure is logged and rescheduled there
        }
        return;
    }
    if ((nowMs - _persistenceQuietStartMs) >= kPersistQuietMs ||
        (nowMs - _persistenceBurstStartMs) >= kPersistMaxDeferMs) {
        (void)saveToPersistence();  // a failure is logged and rescheduled there
    }
}

void DeviceModel::flushPersistenceIfDirty()
{
    if (_persistenceDirty) {
        (void)saveToPersistence();  // a failure is logged and left dirty there
    }
#if KNX_SECURE_ENABLED
    checkpointSecuritySequenceState();
#endif
}

void DeviceModel::setBankedRecordSource(uint16_t keyId, objects::InterfaceObjectManager::BankedRecordSource source)
{
    if (_interfaceObjectManager) {
        _interfaceObjectManager->setBankedRecordSource(keyId, std::move(source));
    }
}

std::optional<std::vector<uint8_t>> DeviceModel::loadBankedRecord(uint16_t keyId)
{
    if (!_interfaceObjectManager) {
        return std::vector<uint8_t>{};
    }
    return _interfaceObjectManager->loadBankedRecord(keyId);
}

#if KNX_SECURE_ENABLED
// ─── Data Secure state ───────────────────────────────────────────────────────

bool DeviceModel::loadSecuritySequenceState()
{
    return _interfaceObjectManager && _interfaceObjectManager->loadSecuritySequenceState();
}

void DeviceModel::checkpointSecuritySequenceState()
{
    if (!_interfaceObjectManager || !_securityObject.sequenceStateDirty()) {
        return;
    }
    // A failed write leaves the state dirty; the next checkpoint retries.
    (void)_interfaceObjectManager->saveSecuritySequenceState();
}

void DeviceModel::pumpSecuritySequenceCheckpoint(uint32_t nowMs)
{
    if (!_securityObject.sequenceStateDirty()) {
        _sequenceCheckpointDueMs = nowMs + kSequenceCheckpointMs;
        return;
    }
    if (static_cast<int32_t>(nowMs - _sequenceCheckpointDueMs) < 0) {
        return;
    }
    checkpointSecuritySequenceState();
    _sequenceCheckpointDueMs = nowMs + kSequenceCheckpointMs;
}

#endif
void DeviceModel::saveAuthorizationKeyTable(const application::AuthorizationService::KeyTable& keys)
{
    if (!_interfaceObjectManager) {
        return;
    }
    const bool anyKey = std::any_of(keys.begin(), keys.end(), [](const application::AuthorizationKey& key) {
        return key != application::AuthorizationService::kNoKey;
    });
    util::Result<void> saved = util::Result<void>::ok();
    if (!anyKey) {
        saved = _interfaceObjectManager->saveAuthorizationKeyTable({});
    } else {
        std::array<uint8_t, application::AuthorizationService::kSerializedKeyTableSize> record{};
        application::AuthorizationService::serializeKeyTable(keys, record);
        saved = _interfaceObjectManager->saveAuthorizationKeyTable(record);
    }
    if (saved.isError()) {
        KNX_LOGW(TAG, "Authorisation key table could not be stored: %s", util::errorCodeToString(saved.error()));
    }
}

std::optional<application::AuthorizationService::KeyTable> DeviceModel::loadAuthorizationKeyTable()
{
    if (!_interfaceObjectManager) {
        return std::nullopt;
    }
    const auto record = _interfaceObjectManager->loadAuthorizationKeyTable();
    if (record.size() != application::AuthorizationService::kSerializedKeyTableSize) {
        return std::nullopt;
    }
    return application::AuthorizationService::deserializeKeyTable(
        std::span<const uint8_t, application::AuthorizationService::kSerializedKeyTableSize>(
            record.data(), record.size()));
}

#if KNX_SECURE_ENABLED
void DeviceModel::captureFactorySecurityDefaults()
{
    // Once: a BAU closed and initialised again must not take what the first
    // restore brought back for the firmware's own defaults.
    if (_factorySecurityCaptured) {
        return;
    }
    _factorySecurity.toolKey = _securityObject.getToolKey();
    _factorySecurity.securityMode = _securityObject.getSecurityMode();
    _factorySecurity.sendingSequence = _securityObject.getSendingSequenceThreadSafe();
    _factorySecurityCaptured = true;
}

void DeviceModel::settleRestoredSecurityState()
{
    const auto& factoryKey = _factorySecurity.toolKey;
    const bool factoryKeySet = std::any_of(factoryKey.begin(), factoryKey.end(),
                                           [](uint8_t b) { return b != 0u; });
    if (factoryKeySet && !_securityObject.hasToolKey()) {
        // Every save stores PID_TOOL_KEY, so a device that ran without a key
        // (an older firmware, or one that sets it after init()) has zeros on
        // record. Zeros are the absence of a key, not one ETS installed.
        _securityObject.setToolKeySilent(factoryKey);
        KNX_LOGI(TAG, "Factory Tool Key in force (none stored)");
    }
    if (_factorySecurity.sendingSequence > _securityObject.getSendingSequenceThreadSafe()) {
        // A sequence number already used must not be used again; whichever
        // side is ahead wins, as for the Last Valid SeqNr values.
        _securityObject.setSendingSequenceSilent(_factorySecurity.sendingSequence);
    }
}

void DeviceModel::restoreFactorySecurityDefaults()
{
    // §3.7.1.2.3.2.2 requires the key tables dropped whole, so that the FDSK
    // cannot be used to read back what a previous owner downloaded.
    _securityObject.loadControlReset();
    _securityObject.setToolKey(_factorySecurity.toolKey);
    _securityObject.setSecurityMode(_factorySecurity.securityMode);
    applySecurityKeyTables();
}

void DeviceModel::applySecurityKeyTables()
{
    using objects::SecurityProperty;
    namespace security_ia_table = objects::security_ia_table;

    // Entry sizes of 03/05/01 §6.3.6 Figure 72, §6.3.7 Figure 74 and §6.3.8
    // Figure 75. They match the PDT_GENERIC_nn the properties are declared as.
    constexpr size_t kGroupKeyEntryBytes = 18;   // GA_Index(2) + Key(16)
    constexpr size_t kP2PKeyEntryBytes = 20;     // IA_Index(2) + Key(16) + Roles(2)
    constexpr size_t kKeyBytes = 16;

    const auto readIndex = [](const std::vector<uint8_t>& blob, size_t offset) {
        return static_cast<uint16_t>((static_cast<uint16_t>(blob[offset]) << 8) | blob[offset + 1]);
    };

    // The maps are a derived view, rebuilt from empty so an entry ETS deleted
    // in the project does not linger as a usable key. Firmware-provided keys
    // are not part of it: the Security Interface Object keeps them apart and
    // puts them in force when the downloaded table holds no entries.
    objects::SecurityInterfaceObject::KeyMap resolvedGroupKeys;
    objects::SecurityInterfaceObject::KeyMap resolvedDeviceKeys;
    bool groupKeyTableHasEntries = false;
    bool p2pKeyTableHasEntries = false;

    // §6.3.7: the 2-octet field is the GA_Index — the TSAP, i.e. the 1-based
    // index into the Group Address Table — and NOT the group address itself.
    // Resolving it is the whole reason this lives here rather than in the
    // Security Interface Object, which cannot see the address table.
    size_t groupKeys = 0;
    size_t unresolvedGroupKeys = 0;
    if (const auto* blob = _securityObject.findExtraProperty(SecurityProperty::GroupKeyTable)) {
        groupKeyTableHasEntries = blob->size() >= kGroupKeyEntryBytes;
        for (size_t offset = 0; offset + kGroupKeyEntryBytes <= blob->size();
             offset += kGroupKeyEntryBytes) {
            const uint16_t gaIndex = readIndex(*blob, offset);
            const GroupAddress address = _addressTable.getAddress(AddressTableIndex(gaIndex));
            if (address.raw == 0u) {
                // The group address table is downloaded as its own load
                // procedure, so this is expected mid-download and resolves on
                // the next call; a leftover here means the two tables disagree.
                ++unresolvedGroupKeys;
                continue;
            }
            std::array<uint8_t, kKeyBytes> key{};
            std::copy_n(blob->begin() + static_cast<ptrdiff_t>(offset + 2), kKeyBytes, key.begin());
            resolvedGroupKeys[address.raw] = key;
            ++groupKeys;
        }
    }

    // §6.3.8: the Security Individual Address Table is the index space the
    // point-to-point key table refers to, so it has to be read first. Its
    // second field is each partner's Last Valid SeqNr; adopting those values is
    // what carries the replay window across a restart, so it happens here too —
    // this function is the one place called whenever the tables change.
    _securityObject.syncSequencesFromAddressTable();

    std::vector<IndividualAddress> securityAddresses;
    if (const auto* blob = _securityObject.findExtraProperty(
            SecurityProperty::SecurityIndividualAddressTable)) {
        const std::span<const uint8_t> table(*blob);
        const size_t entries = security_ia_table::entryCount(table);
        for (size_t index = 0; index < entries; ++index) {
            securityAddresses.push_back(security_ia_table::addressAt(table, index));
        }
    }

    size_t deviceKeys = 0;
    size_t unresolvedDeviceKeys = 0;
    if (const auto* blob = _securityObject.findExtraProperty(SecurityProperty::P2PKeyTable)) {
        p2pKeyTableHasEntries = blob->size() >= kP2PKeyEntryBytes;
        for (size_t offset = 0; offset + kP2PKeyEntryBytes <= blob->size();
             offset += kP2PKeyEntryBytes) {
            // IA_Index is 1-based, like every other KNX table index.
            const uint16_t iaIndex = readIndex(*blob, offset);
            if (iaIndex == 0u || iaIndex > securityAddresses.size()) {
                ++unresolvedDeviceKeys;
                continue;
            }
            std::array<uint8_t, kKeyBytes> key{};
            std::copy_n(blob->begin() + static_cast<ptrdiff_t>(offset + 2), kKeyBytes, key.begin());
            // The trailing Roles field is deliberately dropped: this stack has
            // no Role model beyond Tool/Unlisted, and silently honouring a
            // subset of the 16 configured Roles would be worse than ignoring
            // them, because the refusals would look arbitrary.
            resolvedDeviceKeys[securityAddresses[iaIndex - 1u].raw] = key;
            ++deviceKeys;
        }
    }

    _securityObject.applyDownloadedGroupKeys(std::move(resolvedGroupKeys), groupKeyTableHasEntries);
    _securityObject.applyDownloadedDeviceKeys(std::move(resolvedDeviceKeys), p2pKeyTableHasEntries);

    if (groupKeys != 0 || deviceKeys != 0 || unresolvedGroupKeys != 0 || unresolvedDeviceKeys != 0) {
        KNX_LOGI(TAG,
                 "Data Secure key tables applied: %zu group key(s), %zu device key(s), "
                 "%zu security IA entries (%zu group / %zu device entries unresolved)",
                 groupKeys, deviceKeys, securityAddresses.size(),
                 unresolvedGroupKeys, unresolvedDeviceKeys);
    } else if (_securityObject.isSecurityEnabled()
               && _securityObject.groupKeyCount() == 0u
               && _securityObject.deviceKeyCount() == 0u) {
        // Worth saying out loud: with Security Mode on and no group key, every
        // group telegram this device sends goes out in plain and every secured
        // one it receives is dropped as "unknown key".
        KNX_LOGW(TAG,
                 "Data Secure is enabled but no group or device keys are configured; "
                 "ETS has not downloaded PID_GRP_KEY_TABLE / PID_P2P_KEY_TABLE");
    }
}
#endif  // KNX_SECURE_ENABLED

} // namespace bau
} // namespace knx
