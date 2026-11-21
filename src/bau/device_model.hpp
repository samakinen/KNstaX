// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file device_model.hpp
 * @brief The device's interface-object inventory and its persistence.
 *
 * Private to the BAU. Holds what ETS sees of the device as properties: the
 * InterfaceObjectManager over the six system objects, the optional reference
 * objects (KNXnet/IP Parameter, Router), the property and memory providers the
 * application layer answers from, the coalesced persistence flush, and the
 * Data Secure key maps derived from the downloaded key tables.
 *
 * The interface objects themselves are owned by BusAccessUnit, which hands
 * them out through its public accessors; this holds references.
 *
 * @thread_safety Owner context, except the property providers installed by
 * bind(), which run on the stack task that handles the request. Those only
 * mark persistence dirty; the flush itself runs from loop().
 */

#pragma once

#include "knx/bau/bau.hpp"
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/table_segments.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace knx {
namespace bau {

class DeviceModel final {
public:
    DeviceModel(objects::DeviceObject& deviceObject,
                objects::AddressTableObject& addressTable,
                objects::AssociationTableObject& associationTable,
                objects::ApplicationProgramObject& applicationProgram,
                objects::GroupObjectTableObject& groupObjectTable,
#if KNX_SECURE_ENABLED
                objects::SecurityInterfaceObject& securityObject,
#endif
                BusAccessStackPort* stackPort);

    // Hands `this` to the stack port's property providers.
    DeviceModel(const DeviceModel&) = delete;
    DeviceModel& operator=(const DeviceModel&) = delete;
    DeviceModel(DeviceModel&&) = delete;
    DeviceModel& operator=(DeviceModel&&) = delete;
    // The device object may outlive the model; PID_IO_LIST points into it.
    ~DeviceModel() { _deviceObject.setInterfaceObjectList(nullptr); }

    objects::DeviceObject& deviceObject() { return _deviceObject; }
    const objects::DeviceObject& deviceObject() const { return _deviceObject; }
    objects::AddressTableObject& addressTable() { return _addressTable; }
    const objects::AddressTableObject& addressTable() const { return _addressTable; }
    objects::AssociationTableObject& associationTable() { return _associationTable; }
    const objects::AssociationTableObject& associationTable() const { return _associationTable; }
    objects::ApplicationProgramObject& applicationProgram() { return _applicationProgram; }
    objects::GroupObjectTableObject& groupObjectTable() { return _groupObjectTable; }
    const objects::GroupObjectTableObject& groupObjectTable() const { return _groupObjectTable; }
#if KNX_SECURE_ENABLED
    objects::SecurityInterfaceObject& securityObject() { return _securityObject; }
    const objects::SecurityInterfaceObject& securityObject() const { return _securityObject; }
#endif

    // --- Start-up ------------------------------------------------------------

    /// Create the InterfaceObjectManager and the configured reference objects.
    util::Result<void> init(std::string_view persistenceNamespace, uint16_t schemaVersion);

    /// Restore every persisted property. restoring() is true for the duration,
    /// so observers can tell stored values coming back from remote writes.
    size_t restore();
    /// restore() for one reference object created after it ran.
    size_t restoreReferenceObject(InterfaceObjectType objectType);
    bool restoring() const { return _restoring; }

    /// Register the interface objects, property providers and memory-mapped
    /// load segments with the stack port. After init(): the reference objects
    /// take the indices after the six system objects.
    void bind();

    // --- Reference interface objects ----------------------------------------

    /// Record the optional object types and instantiate any not yet present.
    /// Before init() only records; init() instantiates.
    void setReferenceObjectTypes(std::vector<InterfaceObjectType> objectTypes);
    const std::vector<InterfaceObjectType>& referenceObjectTypes() const { return _referenceObjectTypes; }
    /// Whether init() has created the reference objects. Before that a type
    /// can be declared but its object, and so its properties, do not exist.
    bool referenceObjectsInstantiated() const { return _interfaceObjectManager != nullptr; }

    /// Live reference object of `type`, or nullptr when it is not registered.
    objects::GenericInterfaceObject* referenceObject(InterfaceObjectType type);
    const objects::GenericInterfaceObject* referenceObject(InterfaceObjectType type) const;

    util::Result<void> setReferenceObjectProperty(InterfaceObjectType objectType,
                                                  application::PropertyID propertyId,
                                                  std::span<const uint8_t> value);
    /// Current value of a reference object property, empty if never written.
    std::vector<uint8_t> referenceObjectPropertyValue(InterfaceObjectType objectType,
                                                      application::PropertyID propertyId) const;

    // --- Property access (local API) ----------------------------------------

    util::Result<size_t> readProperty(InterfaceObjectType objectType,
                                      InterfaceObjectInstance objectInstance,
                                      application::PropertyID propertyId,
                                      uint16_t startIndex,
                                      uint8_t elementCount,
                                      std::span<uint8_t> out);
    util::Result<void> writeProperty(InterfaceObjectType objectType,
                                     InterfaceObjectInstance objectInstance,
                                     application::PropertyID propertyId,
                                     uint16_t startIndex,
                                     std::span<const uint8_t> data);

    /// The index->type mapping ETS addresses objects by; the Function
    /// Property services need the same mapping the property services use.
    std::optional<InterfaceObjectType> objectTypeForIndex(InterfaceObjectIndex objectIndex) const;

    // --- Persistence ----------------------------------------------------------
    //
    // saveToPersistence() rewrites every persisted property (~23 blobs,
    // including a 256-byte parameter block) in one go. An ETS download issues
    // hundreds of table writes, so saving per write meant hundreds of full NVS
    // rewrites: flash wear, and long CPU-blocking flash operations interleaved
    // with bus traffic. Instead writes mark the state dirty and the flush is
    // coalesced in loop().

    /// Save everything now. The state stays dirty until a save succeeds; a
    /// failed one is retried from pumpPersistence() every kPersistRetryMs.
    util::Result<size_t> saveToPersistence();

    /// Writes arrive in bus-callback context, which has no clock handy, so the
    /// mark only bumps a counter; loop() supplies the timebase.
    void markPersistenceDirty();
    bool persistenceDirty() const { return _persistenceDirty; }

    /// Flush when the write burst has gone quiet, when it has run so long that
    /// deferring further would risk too much on power loss, or — after a
    /// failed save — when the retry interval has passed.
    void pumpPersistence(uint32_t nowMs);

    /// Unconditional flush of pending state, for the paths that must not defer
    /// (a restart is about to take the device down). A master reset is exactly
    /// when the replay state matters most: ETS restarts the device at the end
    /// of every secure download and immediately talks to it again.
    void flushPersistenceIfDirty();

    /// Banked state that is not a property (InterfaceObjectManager::
    /// bankedRecordKeys()): saved with every other persisted property.
    void setBankedRecordSource(uint16_t keyId, objects::InterfaceObjectManager::BankedRecordSource source);
    /// Empty when none is stored (or before init()), nullopt when unreadable.
    std::optional<std::vector<uint8_t>> loadBankedRecord(uint16_t keyId);

#if KNX_SECURE_ENABLED
    // --- Data Secure state ----------------------------------------------------
    //
    // Last Valid SeqNr values (03/05/01 §6.3.8.4) and the Sequence Number for
    // Tool Access (§6.2) must survive a power cycle, or every partner's already
    // used sequence numbers become acceptable again after a reboot and the
    // replay protection is only as old as the current session. They also move
    // on every secured telegram, so they get their own two-record checkpoint
    // rather than riding on the full save.

    bool loadSecuritySequenceState();

    /// Checkpoint at most every kSequenceCheckpointMs
    /// (CONFIG_KNX_SECURE_REPLAY_CHECKPOINT_S), and only the records that
    /// changed, so sustained secure traffic costs one or two records per
    /// interval rather than per telegram. The window that an unexpected reset
    /// can reopen is bounded by that interval; a planned restart flushes
    /// through flushPersistenceIfDirty() and a power failure through
    /// requestPowerFailSave(). Also stores a new sending ceiling once half of
    /// the reserved window is used.
    void pumpSecuritySequenceCheckpoint(uint32_t nowMs);

    /// Ask the next pump to write the checkpoint at once (if anything is
    /// owed). Only sets a flag: safe from an interrupt or another task.
    void requestPowerFailSave() { _powerFailSaveRequested.store(true, std::memory_order_relaxed); }
#endif

    /// Store the A_Key_Write key table now; a failure is logged.
    void saveAuthorizationKeyTable(const application::AuthorizationService::KeyTable& keys);
    /// The stored key table, or nullopt when none (or a malformed one) is stored.
    std::optional<application::AuthorizationService::KeyTable> loadAuthorizationKeyTable();

#if KNX_SECURE_ENABLED
    /// Rebuild the live Data Secure key maps from the array properties ETS
    /// downloads (PID_GRP_KEY_TABLE, PID_P2P_KEY_TABLE, and the Security
    /// Individual Address Table they index into).  Those properties are stored
    /// verbatim; the Secure Application Layer looks keys up by address, so
    /// without this step a downloaded key table has no effect at all.
    /// Idempotent — safe to call after every write that can change either the
    /// key tables or the group address table they resolve against.
    void applySecurityKeyTables();
#endif

    /// Subscribe every group address of the address table at the data link.
    void resubscribeGroupAddresses();

#if KNX_SECURE_ENABLED
    // --- Factory defaults vs restored state (BusAccessUnit::init) ------------
    //
    // What firmware sets on the Security Interface Object before init() is its
    // factory default. init() records it before the restore and settles the
    // precedence after it; a Factory Reset goes back to it. See the rule on
    // BusAccessUnit::init().

    /// Record the Tool Key, Security Mode and Sequence Number Sending in force
    /// now as the factory defaults. Called once, before the first restore.
    void captureFactorySecurityDefaults();
    /// After the restore: an all-zero restored Tool Key is "no key", not a
    /// key, so the factory Tool Key stays; Sequence Number Sending never moves
    /// backwards, so the higher of the factory and restored values is kept.
    void settleRestoredSecurityState();
    /// Master Reset (Factory Reset): the key tables dropped, the factory Tool
    /// Key and Security Mode back in force, firmware group and device keys in
    /// force again. Sequence Number Sending is not touched (03/03/07 §5).
    void restoreFactorySecurityDefaults();
#endif

private:
    void instantiateReferenceObjects();
#if KNX_SECURE_ENABLED
    void checkpointSecuritySequenceState();
    /// SecurityInterfaceObject::SendingReserveWriter: stores the ceiling.
    static bool storeSendingCeiling(void* context, uint64_t ceiling);
#endif
    util::Result<void> writeProperty(const application::PropertyValueWriteRequest& request);
    void afterReferenceObjectWrite(InterfaceObjectType objectType, application::PropertyID propertyId);
#if KNX_SECURE_ENABLED
    void afterSecurityObjectWrite();
#endif
    void afterTableObjectWrite(InterfaceObjectType objectType,
                               const application::PropertyValueWriteRequest& request);
    void applyLoadedTableSegment(InterfaceObjectType objectType);

    objects::DeviceObject& _deviceObject;
    objects::AddressTableObject& _addressTable;
    objects::AssociationTableObject& _associationTable;
    objects::ApplicationProgramObject& _applicationProgram;
    objects::GroupObjectTableObject& _groupObjectTable;
#if KNX_SECURE_ENABLED
    objects::SecurityInterfaceObject& _securityObject;
#endif
    BusAccessStackPort* const _stackPort;

    std::unique_ptr<objects::InterfaceObjectManager> _interfaceObjectManager;
    std::vector<std::unique_ptr<objects::GenericInterfaceObject>> _referenceObjects;
    std::vector<InterfaceObjectType> _referenceObjectTypes;
    std::vector<InterfaceObjectType> _indexToTypeMap;
    bool _restoring{false};

#if KNX_SECURE_ENABLED
    struct FactorySecurityDefaults {
        std::array<uint8_t, 16> toolKey{};
        objects::SecurityMode securityMode{objects::SecurityMode::Disabled};
        uint64_t sendingSequence{0};
    };
    FactorySecurityDefaults _factorySecurity{};
    bool _factorySecurityCaptured{false};
#endif

    // Deferred-persistence bookkeeping. Quiet window is short enough that the
    // flush still lands well inside the gap between ETS download steps. A
    // failed save is retried no faster than the longest deferral, so a flash
    // that keeps failing costs one attempt every few seconds, not every loop.
    static constexpr uint32_t kPersistQuietMs = 400;
    static constexpr uint32_t kPersistMaxDeferMs = 5000;
    static constexpr uint32_t kPersistRetryMs = kPersistMaxDeferMs;
    bool _persistenceDirty{false};
    bool _persistenceBurstActive{false};
    bool _persistenceRetryPending{false};
    uint32_t _persistenceMarkSeq{0};
    uint32_t _persistenceSeenSeq{0};
    uint32_t _persistenceQuietStartMs{0};
    uint32_t _persistenceBurstStartMs{0};
    uint32_t _persistenceRetryDueMs{0};
    uint32_t _lastPumpMs{0};

    // Data Secure replay-state checkpoint interval. Long on purpose: it trades
    // a bounded window of replayable history after an *unexpected* reset
    // against flash wear, and every orderly restart and reported power failure
    // saves anyway.
#if KNX_SECURE_ENABLED
    static constexpr uint32_t kSequenceCheckpointMs = config::SECURE_REPLAY_CHECKPOINT_MS;
    uint32_t _sequenceCheckpointDueMs{kSequenceCheckpointMs};
    // A failed sending-ceiling top-up is retried no faster than a failed save.
    uint32_t _sendingTopUpRetryDueMs{0};
    bool _sendingTopUpRetryPending{false};
    std::atomic<bool> _powerFailSaveRequested{false};
#endif

    // Backing storage for the memory-mapped table load segments (registered
    // with the application layer's memory service in bind()).
#if KNX_SHARED_LOAD_SEGMENT
    // One staging buffer for the three segments (Kconfig KNX_SHARED_LOAD_SEGMENT).
    // StartLoading of a table claims it, LoadCompleted (after the parse) and
    // Unload release it; only the claiming table's segment accepts writes, and
    // the other segments are read from their tables.
    enum class StagedTable : uint8_t { None, GroupAddress, Association, GroupObject };
    static constexpr size_t kStagingBytes = std::max({objects::tableseg::kGroupAddressTableSize,
                                                      objects::tableseg::kAssociationTableSize,
                                                      objects::tableseg::kGroupObjectTableSize});
    static StagedTable stagedTableFor(InterfaceObjectType objectType);
    void readSegment(StagedTable table, uint32_t offset, std::span<uint8_t> out) const;
    std::array<uint8_t, kStagingBytes> _staging{};
    StagedTable _staged{StagedTable::None};
#else
    std::array<uint8_t, objects::tableseg::kGroupAddressTableSize> _groupAddressSegment{};
    std::array<uint8_t, objects::tableseg::kAssociationTableSize> _associationSegment{};
    std::array<uint8_t, objects::tableseg::kGroupObjectTableSize> _groupObjectSegment{};
#endif
    /// The staged content of @p objectType's segment; empty when it holds none
    /// (only with the shared load segment).
    std::span<const uint8_t> loadedSegment(InterfaceObjectType objectType) const;
};

} // namespace bau
} // namespace knx
