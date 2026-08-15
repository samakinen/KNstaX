// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bau.cpp
 * @brief BusAccessUnit lifecycle and the interface-object property bridge.
 *
 * The BAU is split across four translation units, one per concern:
 *
 *   bau.cpp               device lifecycle (init/close/loop), the
 *                         PropertyAccessBridge that maps ETS property access
 *                         onto the interface-object inventory, and
 *                         Value-Read-on-Initialisation
 *   bau_stack_port.cpp    lower-stack composition behind BusAccessStackPort
 *   bau_group_runtime.cpp group objects: table surface, outbound publish and
 *                         transmit shaping, inbound event queue and dispatch
 *   bau_management.cpp    management services that are not property access
 *                         (Function Property, serial-number commissioning,
 *                         medium-specific interface objects)
 *
 * The seam is real, not cosmetic: nothing in bau_stack_port.cpp is reachable
 * from a BusAccessUnit method except through the BusAccessStackPort interface.
 */

#include "knx/bau/bau.hpp"
#include "bau_internal.hpp"
#include "knx/objects/interface_object.hpp"
#include "knx/objects/table_segments.hpp"
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/object_property_manifest.hpp"
#include "knx/objects/reference_object_registry.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/application/property_store.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/platform/raii_resources.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/testing/mock_tp1_physical.hpp"
#include <array>
#include <algorithm>
#include <span>
#include <cstdint>
#include <type_traits>
#include <utility>
#if KNX_SECURE_ENABLED
#include "knx/security/data_secure.hpp"
#include <map>
#include <memory>
#endif

static const char* TAG = "KNX.BAU";

namespace knx {
namespace bau {

using StackPort = BusAccessStackPort;

using detail::PendingInboundGroupEvent;

class PropertyAccessBridge final {
public:
    static util::ErrorCode toErrorCode(objects::PropertyAccessResult result)
    {
        switch (result) {
            case objects::PropertyAccessResult::Success:
                return util::ErrorCode::Success;
            case objects::PropertyAccessResult::InvalidObject:
                return util::ErrorCode::InvalidAddress;
            case objects::PropertyAccessResult::InvalidProperty:
            case objects::PropertyAccessResult::InvalidValue:
                return util::ErrorCode::InvalidParameter;
            case objects::PropertyAccessResult::ReadOnly:
            case objects::PropertyAccessResult::WriteOnly:
            case objects::PropertyAccessResult::NotImplemented:
                return util::ErrorCode::OperationNotSupported;
        }

        return util::ErrorCode::OperationFailed;
    }

    PropertyAccessBridge(objects::DeviceObject& deviceObject,
                         objects::AddressTableObject& addressTable,
                         objects::AssociationTableObject& associationTable,
                         objects::ApplicationProgramObject& applicationProgram,
                         objects::GroupObjectTableObject& groupObjectTable,
                         objects::SecurityInterfaceObject& securityObject)
        : _deviceObject(deviceObject)
        , _addressTable(addressTable)
        , _associationTable(associationTable)
        , _applicationProgram(applicationProgram)
        , _groupObjectTable(groupObjectTable)
        , _securityObject(securityObject)
    {
    }

    void setReferenceObjectTypes(std::vector<InterfaceObjectType> objectTypes)
    {
        _referenceObjectTypes.clear();
        _referenceObjectTypes.reserve(objectTypes.size());

        for (const auto objectType : objectTypes) {
            if (!objects::isReferenceObjectType(objectType)) {
                continue;
            }

            const auto alreadyPresent = std::find_if(
                _referenceObjectTypes.begin(),
                _referenceObjectTypes.end(),
                [objectType](InterfaceObjectType configured) {
                    return configured == objectType;
                });
            if (alreadyPresent != _referenceObjectTypes.end()) {
                continue;
            }

            _referenceObjectTypes.push_back(objectType);
        }
    }

    const std::vector<InterfaceObjectType>& referenceObjectTypes() const
    {
        return _referenceObjectTypes;
    }

    /// Live reference object of `type`, or nullptr when it is not registered.
    /// Lets the runtime seed real values into objects that would otherwise
    /// answer every read with zeros.
    objects::GenericInterfaceObject* referenceObject(InterfaceObjectType type)
    {
        for (auto& object : _referenceObjects) {
            if (object && object->objectType() == type) {
                return object.get();
            }
        }
        return nullptr;
    }

    using GroupAddressSubscriber = std::function<util::Result<void>(const GroupAddress&)>;

    void setGroupAddressSubscriber(GroupAddressSubscriber subscriber) {
        _groupAddressSubscriber = std::move(subscriber);
    }

    /// Reports whether a management client ever wrote into a memory region.
    using MemoryRegionWrittenQuery = std::function<bool(MemoryAddress)>;

    void setMemoryRegionWrittenQuery(MemoryRegionWrittenQuery query) {
        _memoryRegionWrittenQuery = std::move(query);
    }

    /// Re-derives the live Data Secure key maps from the downloaded key tables.
    /// The bridge cannot do it itself: resolving a GA_Index needs the address
    /// table *and* the security object, and the BAU owns that pairing.
    using SecurityKeyTableSync = std::function<void()>;

    void setSecurityKeyTableSync(SecurityKeyTableSync sync) {
        _securityKeyTableSync = std::move(sync);
    }

    void syncSecurityKeyTables() {
        if (_securityKeyTableSync) {
            _securityKeyTableSync();
        }
    }

    size_t loadFromPersistence() {
        if (!_interfaceObjectManager) {
            return 0;
        }
        return _interfaceObjectManager->loadFromPersistence();
    }

    size_t saveToPersistence() {
        if (!_interfaceObjectManager) {
            return 0;
        }
        _persistenceDirty = false;
        _persistenceBurstActive = false;
        _persistenceSeenSeq = _persistenceMarkSeq;
        return _interfaceObjectManager->saveToPersistence();
    }

    // --- Data Secure replay state ---------------------------------------
    //
    // Last Valid SeqNr values (03/05/01 §6.3.8.4) and the Sequence Number for
    // Tool Access (§6.2) must survive a power cycle, or every partner's already
    // used sequence numbers become acceptable again after a reboot and the
    // replay protection is only as old as the current session. They also move
    // on every secured telegram, so they get their own two-record checkpoint
    // rather than riding on the full save.

    bool loadSecuritySequenceState() {
        return _interfaceObjectManager && _interfaceObjectManager->loadSecuritySequenceState();
    }

    void checkpointSecuritySequenceState() {
        if (!_interfaceObjectManager || !_securityObject.sequenceStateDirty()) {
            return;
        }
        (void)_interfaceObjectManager->saveSecuritySequenceState();
    }

    /// Checkpoint at most every kSequenceCheckpointMs, so sustained secure
    /// traffic costs two NVS records a minute rather than two per telegram.
    /// The window that an unexpected power loss can reopen is bounded by that
    /// interval; a planned restart flushes through flushPersistenceIfDirty().
    void pumpSecuritySequenceCheckpoint(uint32_t nowMs) {
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

    // --- Deferred persistence -------------------------------------------
    //
    // saveToPersistence() rewrites every persisted property (~23 blobs,
    // including a 256-byte parameter block) in one go. An ETS download issues
    // hundreds of table writes, so saving per write meant hundreds of full NVS
    // rewrites: flash wear, and long CPU-blocking flash operations interleaved
    // with bus traffic. Instead writes mark the state dirty and the flush is
    // coalesced in loop().

    // Writes arrive in bus-callback context, which has no clock handy, so the
    // mark only bumps a counter; loop() supplies the timebase in pumpPersistence.
    void markPersistenceDirty() {
        _persistenceDirty = true;
        ++_persistenceMarkSeq;
    }

    bool persistenceDirty() const { return _persistenceDirty; }

    /// Flush when the write burst has gone quiet, or when the burst has run so
    /// long that deferring further would risk losing too much on power loss.
    void pumpPersistence(uint32_t nowMs) {
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
        if ((nowMs - _persistenceQuietStartMs) >= kPersistQuietMs ||
            (nowMs - _persistenceBurstStartMs) >= kPersistMaxDeferMs) {
            (void)saveToPersistence();
        }
    }

    /// Unconditional flush of pending state, for the paths that must not defer
    /// (a restart is about to take the device down). A master reset is exactly
    /// when the replay state matters most: ETS restarts the device at the end
    /// of every secure download and immediately talks to it again.
    void flushPersistenceIfDirty() {
        if (_persistenceDirty) {
            (void)saveToPersistence();
        }
        checkpointSecuritySequenceState();
    }

    /// Resubscribe all group addresses from the address table to the DLL.
    void resubscribeGroupAddresses() {
        if (!_groupAddressSubscriber) {
            return;
        }
        const uint16_t count = _addressTable.entryCount();
        for (uint16_t i = 1; i <= count; ++i) {
            const auto addr = _addressTable.getAddress(AddressTableIndex(i));
            if (addr.raw != 0) {
                (void)_groupAddressSubscriber(addr);
            }
        }
    }

    util::Result<void> init(std::string_view persistenceNamespace = {},
                            uint16_t schemaVersion = 1) {
        if (!_interfaceObjectManager) {
            _interfaceObjectManager = std::make_unique<objects::InterfaceObjectManager>(
                _deviceObject,
                _addressTable,
                _associationTable,
                _applicationProgram,
                _groupObjectTable,
                _securityObject);
            const auto initRes = _interfaceObjectManager->init(true, persistenceNamespace, schemaVersion);
            if (initRes.isError()) {
                return initRes;
            }
        }

        instantiateReferenceObjects();
        return util::Result<void>::ok();
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
    void instantiateReferenceObjects()
    {
        if (!_interfaceObjectManager) {
            return;
        }

        for (const auto objectType : _referenceObjectTypes) {
            if (referenceObject(objectType) != nullptr) {
                continue;
            }
            // GenericInterfaceObject pulls its schema from the reference manifest directly.
            _referenceObjects.emplace_back(std::make_unique<objects::GenericInterfaceObject>(objectType));
            _interfaceObjectManager->registerReferenceObject(*_referenceObjects.back());
        }
    }

    util::Result<size_t> readProperty(InterfaceObjectType objectType,
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
        if (res != objects::PropertyAccessResult::Success) {
            return toErrorCode(res);
        }
        if (data.size() > out.size()) {
            return util::ErrorCode::BufferTooSmall;
        }

        std::copy(data.begin(), data.end(), out.begin());
        return data.size();
    }

    util::Result<void> writeProperty(InterfaceObjectType objectType,
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
        if (res != objects::PropertyAccessResult::Success) {
            return toErrorCode(res);
        }
        // Same reason as the bus-driven write path below: whatever is written
        // into the Security Interface Object is a credential the next
        // conversation depends on, and losing it across a restart leaves the
        // device answering with the wrong key. The local API is now the only
        // way to install one — the wire path refuses unsecured writes.
        if (objectType == InterfaceObjectType::security()) {
            markPersistenceDirty();
            syncSecurityKeyTables();
        }
        return util::Result<void>::ok();
    }

    void bind(StackPort& stackPort) {
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
            &_applicationProgram,
            &_securityObject
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

            stackPort.registerPropertyObject(object->objectType(), objectIndex, [object](application::PropertyStore& store) {
                object->registerProperties(store);
            });
            _indexToTypeMap.push_back(object->objectType());
        }

        stackPort.setPropertyReadProvider(
            [this](const IndividualAddress& /*source*/, const application::PropertyValueReadRequest& request, std::span<uint8_t> out)
                -> std::optional<util::Result<size_t>> {
                application::PropertyServiceDataBuffer data;
                if (out.size() > data.capacity()) {
                    return util::ErrorCode::BufferTooSmall;
                }
                const auto objectType = resolveObjectType(request.objectIndex);
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
                if (res != objects::PropertyAccessResult::Success) {
                    return std::nullopt;
                }
                std::copy(data.begin(), data.end(), out.begin());
                return util::Result<size_t>(data.size());
            });

        stackPort.setPropertyWriteProvider(
            [this](const IndividualAddress& /*source*/, const application::PropertyValueWriteRequest& request) -> util::Result<void> {
                const auto objectType = resolveObjectType(request.objectIndex);
                if (!objectType.has_value()) {
                    return util::Result<void>::err(util::ErrorCode::InvalidParameter);
                }
                const auto res = _interfaceObjectManager->writeProperty(
                    *objectType,
                    InterfaceObjectInstance(1),
                    request.propertyId,
                    request.startIndex,
                    request.data.span());
                if (res != objects::PropertyAccessResult::Success) {
                    return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
                }
                // The Security Interface Object holds the credentials for every
                // later conversation: ETS installs its own Tool Key and the
                // Sequence Number Sending here and then immediately restarts the
                // device. Losing them across that restart leaves the device
                // answering with the factory key, and ETS reports "no
                // SyncResponse was received; probably the key did not match".
                // The restart path flushes what is marked dirty, so marking is
                // enough — no synchronous write in the middle of the download.
                if (*objectType == InterfaceObjectType::security()) {
                    markPersistenceDirty();
                    // PID_GRP_KEY_TABLE and PID_P2P_KEY_TABLE are stored as
                    // opaque arrays; the Secure Application Layer looks keys up
                    // by address, so the download only takes effect once they
                    // are resolved against the address tables. Doing it on every
                    // write to this object keeps it correct whichever order ETS
                    // writes the tables in.
                    syncSecurityKeyTables();
                }

                // Persist table objects after every successful ETS write so
                // the KNX tables survive reboot without explicit user action.
                const bool isTableObject = (*objectType == InterfaceObjectType::addressTable())
                    || (*objectType == InterfaceObjectType::associationTable())
                    || (*objectType == InterfaceObjectType::groupObjectTable())
                    || (*objectType == InterfaceObjectType::applicationProgram());
                if (isTableObject) {
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
                        applyLoadedTableSegment(*objectType);
                    }
                    // Persist only when table content can actually have changed:
                    // a completed load or a direct table-data write. Intermediate
                    // load-control events (unload/start/segment alloc) would
                    // otherwise trigger a full NVS save each — flash wear and
                    // latency in the middle of the ETS download.
                    if (!isLoadControlWrite || isLoadCompleted) {
                        markPersistenceDirty();
                        // If the address table was updated, subscribe newly added
                        // GAs to the data link layer immediately so they work
                        // this session.
                        if (*objectType == InterfaceObjectType::addressTable()) {
                            if (_groupAddressSubscriber) {
                                resubscribeGroupAddresses();
                            }
                            // Group keys are held against the GA_Index, so a new
                            // address table re-points every one of them.
                            syncSecurityKeyTables();
                        }
                    }
                }
                return util::Result<void>::ok();
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
                const auto objectType = resolveObjectType(objectIndex);
                if (!objectType.has_value()) {
                    return std::nullopt;
                }

                application::PropertyID resolvedId = static_cast<application::PropertyID>(0);
                application::PropertyDataType type = application::PropertyDataType::GenericData;
                uint16_t maxElements = 0;
                uint8_t access = 0;
                uint8_t readLevel = 0;
                uint8_t writeLevel = 0;
                const auto res = _interfaceObjectManager->describeProperty(
                    *objectType,
                    InterfaceObjectInstance(1),
                    propertyId,
                    propertyIndex,
                    resolvedId,
                    type,
                    maxElements,
                    access,
                    readLevel,
                    writeLevel);
                if (res != objects::PropertyAccessResult::Success) {
                    return std::nullopt;
                }

                application::PropertyDescriptionInfo info;
                info.objectIndex = objectIndex;
                info.propertyId = resolvedId;
                info.propertyIndex = propertyIndex;
                info.type = type;
                info.maxElements = maxElements;
                info.writeAccess = (access & 0x02) != 0 ? PropertyWriteAccess::Allowed : PropertyWriteAccess::Denied;
                info.readLevel = readLevel;
                info.writeLevel = writeLevel;
                return info;
            });
    }

    /// Public view of the index->type mapping ETS addresses objects by.
    /// Management services outside the property path (Function Property, in
    /// particular) need the same mapping the property services use.
    std::optional<InterfaceObjectType> objectTypeForIndex(InterfaceObjectIndex objectIndex) const {
        return resolveObjectType(objectIndex);
    }

private:
    std::optional<InterfaceObjectType> resolveObjectType(InterfaceObjectIndex objectIndex) const {
        if (objectIndex.value() >= _indexToTypeMap.size()) {
            return std::nullopt;
        }
        return _indexToTypeMap[objectIndex.value()];
    }

    // Apply the content of a memory-mapped load segment (big-endian u16 count
    // followed by the entries) to the corresponding table domain. Called when
    // ETS writes the LoadCompleted event after streaming the segment.
    void applyLoadedTableSegment(InterfaceObjectType objectType) {
        const auto u16at = [](std::span<const uint8_t> segment, size_t offset) -> uint16_t {
            return static_cast<uint16_t>((static_cast<uint16_t>(segment[offset]) << 8)
                                         | segment[offset + 1u]);
        };

        if (objectType == InterfaceObjectType::addressTable()) {
            const std::span<const uint8_t> segment(_groupAddressSegment);
            uint16_t count = u16at(segment, 0);
            if (count > objects::AddressTableDomain::kMaxEntries) {
                KNX_LOGW(TAG, "Group address segment count %u exceeds capacity, truncating", count);
                count = objects::AddressTableDomain::kMaxEntries;
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
                KNX_LOGW(TAG, "Association segment count %u exceeds capacity, truncating", count);
                count = objects::AssociationTableDomain::kMaxEntries;
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
            if (_memoryRegionWrittenQuery
                && !_memoryRegionWrittenQuery(
                       MemoryAddress(objects::tableseg::kApplicationCodeBase))) {
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

    objects::DeviceObject& _deviceObject;
    objects::AddressTableObject& _addressTable;
    objects::AssociationTableObject& _associationTable;
    objects::ApplicationProgramObject& _applicationProgram;
    objects::GroupObjectTableObject& _groupObjectTable;
    objects::SecurityInterfaceObject& _securityObject;
    std::unique_ptr<objects::InterfaceObjectManager> _interfaceObjectManager;
    std::vector<std::unique_ptr<objects::GenericInterfaceObject>> _referenceObjects;
    std::vector<InterfaceObjectType> _referenceObjectTypes;
    std::vector<InterfaceObjectType> _indexToTypeMap;
    GroupAddressSubscriber _groupAddressSubscriber;
    MemoryRegionWrittenQuery _memoryRegionWrittenQuery;
    SecurityKeyTableSync _securityKeyTableSync;

    // Deferred-persistence bookkeeping. Quiet window is short enough that the
    // flush still lands well inside the gap between ETS download steps.
    static constexpr uint32_t kPersistQuietMs = 400;
    static constexpr uint32_t kPersistMaxDeferMs = 5000;
    bool _persistenceDirty{false};
    bool _persistenceBurstActive{false};
    uint32_t _persistenceMarkSeq{0};
    uint32_t _persistenceSeenSeq{0};
    uint32_t _persistenceQuietStartMs{0};
    uint32_t _persistenceBurstStartMs{0};

    // Data Secure replay-state checkpoint interval. Long on purpose: it trades
    // a bounded window of replayable history after an *unexpected* power loss
    // against flash wear, and every orderly restart flushes anyway.
    static constexpr uint32_t kSequenceCheckpointMs = 60000;
    uint32_t _sequenceCheckpointDueMs{kSequenceCheckpointMs};

    // Backing storage for the memory-mapped table load segments (registered
    // with the application layer's memory service in bind()).
    std::array<uint8_t, objects::tableseg::kGroupAddressTableSize> _groupAddressSegment{};
    std::array<uint8_t, objects::tableseg::kAssociationTableSize> _associationSegment{};
    std::array<uint8_t, objects::tableseg::kGroupObjectTableSize> _groupObjectSegment{};
    std::array<uint8_t, objects::tableseg::kApplicationCodeSize> _applicationCodeSegment{};
};


BusAccessUnit::BusAccessUnit(platform::Platform& platform,
                             std::unique_ptr<physical::Tp1MediumBackend> mediumBackend)
    : BusAccessUnit(platform, createTp1StackPort(platform, std::move(mediumBackend)))
{
}

BusAccessUnit::BusAccessUnit(platform::Platform& platform,
                             std::unique_ptr<BusAccessStackPort> stackPort)
    : _platform(platform)
    , _deviceObject()
    , _addressTable()
    , _associationTable()
    , _applicationProgram()
    , _groupObjectTable()
    , _securityObject()
    , _initialized(false)
    , _inboundGroupEventQueue(std::make_unique<platform::Queue>(
          _platform,
          sizeof(PendingInboundGroupEvent),
          INBOUND_GROUP_EVENT_QUEUE_CAPACITY))
    , _stackPort(std::move(stackPort))
    , _propertyAccessBridge(std::make_unique<PropertyAccessBridge>(_deviceObject,
                                                                   _addressTable,
                                                                   _associationTable,
                                                                   _applicationProgram,
                                                                   _groupObjectTable,
                                                                   _securityObject))
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
    if (!_inboundGroupEventQueue || !_inboundGroupEventQueue->handle()) {
        KNX_LOGE(TAG, "Cannot initialize BAU without an inbound group event queue");
        return util::Result<void>::err(util::ErrorCode::ResourceUnavailable);
    }

    clearInboundGroupEvents();
    _droppedInboundGroupEvents = 0u;
    _droppedAutomaticResponses = 0u;
    _autoResponseQueueHead = 0u;
    _autoResponseQueueCount = 0u;
    _autoResponseOperation = AutoResponseOperationState{};
    _hadImmediateWork = false;

    // Set device address
    _deviceObject.setIndividualAddress(ownAddress);

    auto stackInit = _stackPort->init(ownAddress);
    if (!stackInit) {
        KNX_LOGE(TAG, "Failed to initialize BAU lower stack: %s",
                 util::errorCodeToString(stackInit.error()));
        return stackInit;
    }

    _stackPort->setProgrammingModeEnabled(_deviceObject.getProgMode());
    _deviceObject.registerProgModeCallback([this](Toggle mode) {
        if (_stackPort) {
            _stackPort->setProgrammingModeEnabled(mode == Toggle::Enable);
        }
    });
    // Firmware sets the real serial number (MAC-derived here) after start(),
    // which is after wireManagementServices() cached it.  Re-push it so the
    // serial-number-addressed services and the programming-mode scan answer
    // with the device's actual identity instead of the all-zero placeholder.
    _deviceObject.registerSerialNumberCallback([this](std::span<const uint8_t> serial) {
        if (!_stackPort || serial.size() < application::kKnxSerialNumberBytes) {
            return;
        }
        application::KnxSerialNumber knxSerial{};
        std::copy_n(serial.begin(), application::kKnxSerialNumberBytes, knxSerial.begin());
        _stackPort->setCommissioningSerialNumber(knxSerial);
    });
    _deviceObject.registerIndividualAddressCallback([this](const IndividualAddress& newAddress) {
        // The address is stored as two independent properties (PID_SUBNET_ADDRESS
        // and PID_DEVICE_ADDRESS), so restoring it from persistence necessarily
        // arrives in two steps. Propagating the intermediate value would put a
        // half-restored address on the stack — e.g. 1.1.255 between restoring
        // the subnet and the device octet. init() pushes the final address to
        // the stack once the restore completes, so stay quiet until then.
        if (_restoringPersistentState) {
            return;
        }
        if (_stackPort) {
            auto res = _stackPort->setOwnAddress(newAddress);
            if (res.isError()) {
                KNX_LOGW(TAG, "Failed to propagate new own address to stack: %s",
                         util::errorCodeToString(res.error()));
            }
        }
        if (_propertyAccessBridge) {
            (void)_propertyAccessBridge->saveToPersistence();
        }
    });
    _stackPort->setIndividualAddressUpdateCallback([this](const IndividualAddress& newAddress) {
        if (_deviceObject.writeIndividualAddress(newAddress).isError()) {
            KNX_LOGW(TAG, "Failed to update BAU device object individual address for 0x%04X",
                     newAddress.raw);
        }
    });

#if KNX_SECURE_ENABLED
    _stackPort->configureDataSecure(_securityObject);
#endif

    wireManagementServices();

    // Route group value services back into BAU/group objects
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

    // Populate property store with interface-object properties and route property
    // services back through the BAU-owned interface-object inventory.
    if (_propertyAccessBridge) {
        // Wire DLL group-address subscription so the bridge can add new GAs
        // immediately when ETS writes the address table during this session.
        _propertyAccessBridge->setGroupAddressSubscriber([this](const GroupAddress& addr) {
            return _stackPort ? _stackPort->addGroupAddress(addr)
                              : util::Result<void>::err(util::ErrorCode::InvalidParameter);
        });
        _propertyAccessBridge->setMemoryRegionWrittenQuery([this](MemoryAddress regionStart) {
            return _stackPort ? _stackPort->memoryRegionWritten(regionStart) : false;
        });
        _propertyAccessBridge->setSecurityKeyTableSync([this]() {
            applySecurityKeyTables();
        });

        const auto propertyInit = _propertyAccessBridge->init(persistenceNamespace, persistenceSchemaVersion);
        if (propertyInit.isError()) {
            KNX_LOGE(TAG, "Interface object manager init failed: %s", util::errorCodeToString(propertyInit.error()));
        } else {
            // Restore KNX tables (address/association/group object) saved from a
            // previous ETS commissioning session.
            _restoringPersistentState = true;
            const size_t loaded = _propertyAccessBridge->loadFromPersistence();
            _restoringPersistentState = false;
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
            _propertyAccessBridge->resubscribeGroupAddresses();

            // The key tables are restored as opaque blobs like every other
            // property, so the derived key maps have to be rebuilt from them —
            // after the address table is back, since group keys resolve through
            // it. Without this the device boots into Security Mode with no
            // usable group key and quietly sends everything in plain.
            applySecurityKeyTables();

            // The per-partner Last Valid SeqNr values came back inside the
            // Security Individual Address Table that applySecurityKeyTables()
            // just adopted; the Sequence Number for Tool Access is not a
            // property and is restored from its own record (03/05/01 §6.2).
            (void)_propertyAccessBridge->loadSecuritySequenceState();

            _propertyAccessBridge->bind(*_stackPort);
        }
    }

    (void)_deviceObject.processLoadEvent(objects::LoadEvent::StartLoad);
    (void)_deviceObject.processLoadEvent(objects::LoadEvent::LoadComplete);
    (void)_deviceObject.processLoadEvent(objects::LoadEvent::LoadComplete);
    _deviceObject.setRunState(objects::RunState::Running);
    _initialized = true;

    // Report the address actually in effect, not the `ownAddress` parameter:
    // loadFromPersistence() above may have restored a programmed address (e.g.
    // 1.1.3) into the device object and synced it to the stack, while
    // `ownAddress` still holds the pre-restore default (15.15.255).
    const IndividualAddress effectiveAddress = _deviceObject.readIndividualAddress();
    KNX_LOGI(TAG, "BAU initialized, address: %d.%d.%d",
             effectiveAddress.area(), effectiveAddress.line(), effectiveAddress.device());

    return util::Result<void>::ok();
}

void BusAccessUnit::setReferenceInterfaceObjectTypes(std::vector<InterfaceObjectType> objectTypes)
{
    if (_propertyAccessBridge) {
        _propertyAccessBridge->setReferenceObjectTypes(std::move(objectTypes));
        // A type added after init() still needs its object; before init() this
        // is a no-op and init() does the work.
        _propertyAccessBridge->instantiateReferenceObjects();
    }
}

const std::vector<InterfaceObjectType>& BusAccessUnit::referenceInterfaceObjectTypes() const
{
    static const std::vector<InterfaceObjectType> kEmpty;

    if (!_propertyAccessBridge) {
        return kEmpty;
    }

    return _propertyAccessBridge->referenceObjectTypes();
}

void BusAccessUnit::close() {
    if (_initialized) {
        if (_stackPort) {
            _stackPort->close();
        }

        clearInboundGroupEvents();
        _droppedInboundGroupEvents = 0u;
        _droppedAutomaticResponses = 0u;
        _autoResponseQueueHead = 0u;
        _autoResponseQueueCount = 0u;
        _autoResponseOperation = AutoResponseOperationState{};

        _deviceObject.setRunState(objects::RunState::Halted);
        (void)_deviceObject.processLoadEvent(objects::LoadEvent::StartUnload);
        (void)_deviceObject.processLoadEvent(objects::LoadEvent::UnloadComplete);
        _initialized = false;
    }

    _hadImmediateWork = false;
}

void BusAccessUnit::loop() {
    // Coalesced NVS flush. Runs before executePendingRestart() so a burst that
    // has just gone quiet is written before a queued restart takes the device
    // down; a restart arriving mid-burst is covered by the explicit flush in
    // the restart handler.
    if (_propertyAccessBridge) {
        const uint32_t now = nowMs();
        _propertyAccessBridge->pumpPersistence(now);
        _propertyAccessBridge->pumpSecuritySequenceCheckpoint(now);
    }

    // Service deferred lower-layer work outside callback context.
    if (_stackPort) {
        _stackPort->executePendingRestart();
        _stackPort->processBackgroundWork();
    }

    processInboundGroupEvents();
    pumpReadOnInit();
    pumpGroupObjectTransmissions(nowMs());
    refreshWorkAvailabilityState();
}

void BusAccessUnit::flushPendingPersistence() {
    if (_propertyAccessBridge) {
        _propertyAccessBridge->flushPersistenceIfDirty();
    }
}

util::Result<void> BusAccessUnit::setReferenceObjectProperty(InterfaceObjectType objectType,
                                                             application::PropertyID propertyId,
                                                             std::span<const uint8_t> value) {
    if (!_propertyAccessBridge) {
        return util::ErrorCode::NotInitialized;
    }
    auto* object = _propertyAccessBridge->referenceObject(objectType);
    if (object == nullptr) {
        return util::ErrorCode::InvalidAddress;
    }
    return object->setPropertyValue(propertyId, value);
}

network::TwoPortCoupler* BusAccessUnit::stackPortCoupler() {
    return _stackPort ? _stackPort->coupler() : nullptr;
}

std::vector<uint8_t> BusAccessUnit::referenceObjectPropertyValue(
    InterfaceObjectType objectType, application::PropertyID propertyId) const {
    if (!_propertyAccessBridge) {
        return {};
    }
    auto* object = _propertyAccessBridge->referenceObject(objectType);
    if (object == nullptr) {
        return {};
    }
    return object->propertyValue(propertyId);
}

std::optional<InterfaceObjectType> BusAccessUnit::interfaceObjectTypeForIndex(
    InterfaceObjectIndex objectIndex) const {
    if (!_propertyAccessBridge) {
        return std::nullopt;
    }
    return _propertyAccessBridge->objectTypeForIndex(objectIndex);
}

void BusAccessUnit::pumpReadOnInit() {
    if (_readOnInitState == ReadOnInitState::Done) {
        return;
    }

    // Value-Read-on-Initialisation is only meaningful once the device has an
    // individual address and can actually be answered.  Before that the reads
    // would be emitted from the unprogrammed address and thrown away.
    if (!_initialized || !_deviceObject.getIndividualAddress().isValid()) {
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

BusAccessUnit::OwnerWorkHint BusAccessUnit::ownerWorkHint() const {
    OwnerWorkHint hint;
    hint.pendingLoopWorkItems = queuedInboundGroupEventCount();
    hint.pendingDeferredWorkItems = _autoResponseQueueCount;
    return hint;
}

void BusAccessUnit::setWorkAvailableCallback(WorkAvailableCallback callback) {
    _workAvailableCallback = std::move(callback);
    if (_stackPort) {
        _stackPort->setWorkAvailableCallback([this]() {
            notifyWorkAvailableIfTransitioned();
        });
    }
    refreshWorkAvailabilityState();
}

bool BusAccessUnit::hasStackPort() const {
    return static_cast<bool>(_stackPort);
}

util::Result<void> BusAccessUnit::sendDataLinkFrame(const datalink::LDataFrame& frame) {
    return _stackPort ? _stackPort->sendDataLinkFrame(frame)
                      : util::Result<void>::err(util::ErrorCode::InvalidParameter);
}

void BusAccessUnit::setDataLinkPromiscuousMode(datalink::PromiscuousMode mode) {
    if (_stackPort) {
        _stackPort->setDataLinkPromiscuousMode(mode);
    }
}

util::Result<size_t> BusAccessUnit::readProperty(InterfaceObjectType objectType,
                                                 InterfaceObjectInstance objectInstance,
                                                 application::PropertyID propertyId,
                                                 uint16_t startIndex,
                                                 uint8_t elementCount,
                                                 std::span<uint8_t> out)
{
    if (!_propertyAccessBridge) {
        return util::ErrorCode::NotInitialized;
    }
    return _propertyAccessBridge->readProperty(objectType,
                                               objectInstance,
                                               propertyId,
                                               startIndex,
                                               elementCount,
                                               out);
}

util::Result<void> BusAccessUnit::writeProperty(InterfaceObjectType objectType,
                                                InterfaceObjectInstance objectInstance,
                                                application::PropertyID propertyId,
                                                uint16_t startIndex,
                                                std::span<const uint8_t> data)
{
    if (!_propertyAccessBridge) {
        return util::ErrorCode::NotInitialized;
    }
    return _propertyAccessBridge->writeProperty(objectType,
                                                objectInstance,
                                                propertyId,
                                                startIndex,
                                                data);
}

util::Result<void> BusAccessUnit::requestCommissioningMode(bool enabled)
{
    _deviceObject.setProgMode(enabled ? Toggle::Enable : Toggle::Disable);
    if (_stackPort) {
        _stackPort->setProgrammingModeEnabled(enabled);
    }
    return util::Result<void>::ok();
}

bool BusAccessUnit::commissioningModeEnabled() const
{
    return _deviceObject.getProgMode();
}

void BusAccessUnit::setDefaultTransmissionOptions(const TransmissionOptions& options) {
    if (_stackPort) {
        _stackPort->setDefaultTransmissionOptions(options);
    }
}

const BusAccessUnit::TransmissionOptions& BusAccessUnit::defaultTransmissionOptions() const {
    static const TransmissionOptions fallback{};
    return _stackPort ? _stackPort->defaultTransmissionOptions() : fallback;
}

} // namespace bau
} // namespace knx
