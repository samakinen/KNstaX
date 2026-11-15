// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file interface_object_manager.hpp
 * @brief Central interface object manager
 * 
 * Manages all interface objects in the device and provides property dispatch.
 */

#pragma once

#include "knx/objects/device_object.hpp"
#include "knx/objects/address_table_object.hpp"
#include "knx/objects/association_table_object.hpp"
#include "knx/objects/application_program_object.hpp"
#include "knx/objects/group_object_table_object.hpp"
#include "knx/config.hpp"
#if KNX_SECURE_ENABLED
#include "knx/objects/security_interface_object.hpp"
#endif
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/objects/reference_object_registry.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/application/property.hpp"
#include "knx/types.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"
#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace knx {
namespace objects {

// Property access results are util::Result<void>:
//   InvalidAddress         no such object (type, or an instance other than 0/1)
//   NotFound               the object has no such property
//   InvalidParameter       the index, count or value is not acceptable
//   ReadOnly / WriteOnly   the property refuses that direction
//   OperationNotSupported  the object has no property handlers at all

/**
 * @brief Interface object manager
 * 
 * Central manager for all interface objects. Provides:
 * - Lifecycle management of interface objects
 * - Property routing and dispatch
 * - Persistence integration
 * - Object discovery
 */
class InterfaceObjectManager {
public:
    InterfaceObjectManager();
    InterfaceObjectManager(
        DeviceObject& device,
        AddressTableObject& addressTable,
        AssociationTableObject& associationTable,
        ApplicationProgramObject& applicationProgram,
        GroupObjectTableObject& groupObjectTable
#if KNX_SECURE_ENABLED
        , SecurityInterfaceObject& security
#endif
        );
    ~InterfaceObjectManager() = default;
    
    // Neither copyable nor movable: the object pointers and the object list
    // point at this manager's own members, and the BAU hands out references
    // to it.
    InterfaceObjectManager(const InterfaceObjectManager&) = delete;
    InterfaceObjectManager& operator=(const InterfaceObjectManager&) = delete;
    InterfaceObjectManager(InterfaceObjectManager&&) = delete;
    InterfaceObjectManager& operator=(InterfaceObjectManager&&) = delete;

    /// The core objects every device has: six, or five without KNX Secure (the
    /// Security Interface Object is not part of a build that has it off).
    static constexpr size_t kCoreObjectCount = KNX_SECURE_ENABLED ? 6 : 5;

    /// Upper bound on the objects the manager routes to: the core objects
    /// plus one of each reference object type. registerReferenceObject()
    /// replaces an object of a type already present, so this is never
    /// exceeded by a well-formed configuration.
    static constexpr size_t kMaxInterfaceObjects = kCoreObjectCount + kReferenceObjectTypeCount;
    
    /**
     * @brief Initialize object manager
     * @param enablePersistence Enable NVS persistence
     * @param persistenceNamespace NVS namespace to use for KNX commissioned state.
     *        Defaults to "knx_objects" for backward compatibility.  Pass the
     *        product's namespacePrefix here so multi-product deployments on
     *        the same NVS partition do not collide.
     * @return Result<void> indicating success or error
     */
    /// @param schemaVersion Layout version of the persisted state this firmware
    ///        understands.  Stored alongside the data; when a stored version
    ///        differs, the whole namespace is discarded rather than
    ///        reinterpreted.  See kPersistenceSchemaVersionKeyId.
    util::Result<void> init(bool enablePersistence = true,
                            std::string_view persistenceNamespace = "knx_objects",
                            uint16_t schemaVersion = 1);

    /// Reserved persistence key holding the schema version.
    ///
    /// Object types are 16-bit and interface-object keys are built as
    /// (objectType << 8) | propertyId, so 0xFFFF cannot collide with a real
    /// property: it would require object type 0x00FF and property 0xFF, and
    /// object type 255 is not assigned.
    static constexpr uint16_t kPersistenceSchemaVersionKeyId = 0xFFFFu;

    /// Reserved persistence key holding the Sequence Number for Tool Access.
    ///
    /// 03/05/01 §6.2 defines it as a Resource that is deliberately *not* a
    /// Property of the Security Interface Object, so it has no PID to be keyed
    /// by and no bus service may read it. 0xF0 is outside the PID range any
    /// object of type 17 uses, which keeps it unreachable from the property
    /// paths while sharing the same key space.
    static constexpr uint16_t kToolAccessSequenceKeyId = 0x11F0u;

    /// Reserved persistence key holding the A_Authorize / A_Key_Write key
    /// table (03/03/07 §3.5.8, Figure 88).
    ///
    /// A device Resource with no Property to be keyed by and no service that
    /// reads it back, like the Sequence Number for Tool Access. 0xF1 is outside
    /// the PID range of the Device Object (type 0), so the key shares the key
    /// space without being reachable from the property paths.
    static constexpr uint16_t kAuthorizationKeyTableKeyId = 0x00F1u;

    /// Reserved persistence key naming the bank that holds the commissioned
    /// state (0 or 1).
    ///
    /// The persisted properties are written as a whole into the bank that is
    /// *not* active, and only then is this marker switched. It is one NVS
    /// write and one commit, so a reset during a save leaves the previous
    /// download intact rather than half of each. Bank 0 keys are the plain
    /// (objectType << 8) | propertyId; bank 1 sets bit 15, which no object
    /// type below 0x80 can produce. A namespace without the marker is the
    /// layout written before banks existed, and it is exactly bank 0, so
    /// devices already in the field keep their download and move to bank 1
    /// on their next save. 0xFFFE cannot collide for the same reason as
    /// kPersistenceSchemaVersionKeyId.
    static constexpr uint16_t kPersistenceBankMarkerKeyId = 0xFFFEu;

    /// Bit that selects bank 1 in a property key.
    static constexpr uint16_t kPersistenceBank1KeyBit = 0x8000u;

    /// Banked record holding a coupler's Filter Table with its load state,
    /// allocation, PID_ERROR_CODE and PID_MCB_TABLE
    /// (network::FilterTableLoadControl::serialize()).
    ///
    /// A Resource of the Router Object (type 6) that is memory-mapped rather
    /// than a property, so it has no PID to be keyed by. 0xF0 is outside the
    /// PID range of the Router Object. Unlike the two keys above it is part
    /// of the banked state: it is written into the inactive bank with every
    /// other persisted property and becomes current with the same marker
    /// switch, so a reset during a save keeps the previous table, load state
    /// and download together.
#if KNX_FEATURE_COUPLER
    static constexpr uint16_t kRouterFilterTableKeyId = 0x06F0u;
    static constexpr size_t kBankedRecordCount = 1;
#else
    /// No coupler role, no Filter Table: there is no banked record.
    static constexpr size_t kBankedRecordCount = 0;
#endif

    /// Fills the bytes of a banked record for a save; empty stores nothing.
    using BankedRecordSource = util::InplaceFunction<void(std::vector<uint8_t>&), ::knx::config::callbackBytes(16)>;

    /// Give the save a source for the banked record @p keyId (one of
    /// bankedRecordKeys()). Without a source a save carries the stored record
    /// over unchanged, so state whose owner is not set up yet is not lost.
    void setBankedRecordSource(uint16_t keyId, BankedRecordSource source);

    /// The banked record @p keyId as the active bank holds it: empty when
    /// none is stored, nullopt when one is stored but cannot be read.
    std::optional<std::vector<uint8_t>> loadBankedRecord(uint16_t keyId);

    /// The keys of the banked records that are not properties.
    static std::span<const uint16_t> bankedRecordKeys();

    /// The bank the next load reads and saveSecuritySequenceState() writes.
    /// Exposed for tests.
    uint8_t activePersistenceBank() const { return _activeBank; }

    /**
     * @brief Load all objects from persistence
     * @return Number of objects loaded
     */
    size_t loadFromPersistence();

    /**
     * @brief Restore the persisted properties of one object type only
     *
     * For a reference object created after loadFromPersistence() ran (a
     * Router Object declared once the device is running): its stored values
     * are in the active bank like every other, but nothing restored them.
     * @return Number of properties restored
     */
    size_t loadObjectFromPersistence(InterfaceObjectType objectType);

private:
    /// Compare the stored layout version against this firmware's and discard
    /// the namespace when they differ.  Called from init(), before anything is
    /// loaded.
    void enforcePersistenceSchemaVersion(uint16_t schemaVersion);

    /// Read kPersistenceBankMarkerKeyId; an absent marker means bank 0.
    uint8_t readActivePersistenceBank();

    /// Erase every persisted-property key of @p bank. A failure is logged:
    /// the next save overwrites or erases every key of the bank again.
    void erasePersistenceBank(uint8_t bank);

    /// Erase the whole namespace; a failure is logged, since stored state of
    /// another layout then survives.
    void eraseAllPersistence(const char* reason);

public:
    
    /**
     * @brief Save all objects to persistence
     * @return Number of records written; NotInitialized when persistence is
     *         off, StorageError when the new bank could not be completed or
     *         made current (the previous bank then stays in force).
     */
    util::Result<size_t> saveToPersistence();

#if KNX_SECURE_ENABLED
    /**
     * @brief Checkpoint the Data Secure replay state, and nothing else.
     *
     * 03/05/01 §6.3.8.4 requires every Last Valid SeqNr to survive a power
     * cycle, and §6.2 the same for the Sequence Number for Tool Access. They
     * move on *every* secured telegram, so routing them through
     * saveToPersistence() would rewrite all ~23 persisted blobs at bus rate.
     * This writes the two records that actually changed.
     *
     * @return Number of records written. When a write fails the state stays
     *         dirty, so the next checkpoint tries again.
     */
    size_t saveSecuritySequenceState();

    /// Restore the Sequence Number for Tool Access written by the last
    /// checkpoint. The per-partner numbers ride along inside
    /// PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE and need no separate step.
    /// @return true when a stored value was found and adopted.
    bool loadSecuritySequenceState();
#endif

    /// Store the authorisation key table, as serialised by the application
    /// layer. An empty @p record erases it (no key on any level, the
    /// ex-factory state). Written at once: A_Key_Write is rare and its answer
    /// promises the key is set.
    util::Result<void> saveAuthorizationKeyTable(std::span<const uint8_t> record);

    /// The stored authorisation key table; empty when none is stored.
    std::vector<uint8_t> loadAuthorizationKeyTable();

    // === Object Access ===
    
    /**
     * @brief Get device object
     */
    DeviceObject& deviceObject() { return *_deviceObjectPtr; }
    const DeviceObject& deviceObject() const { return *_deviceObjectPtr; }
    
    /**
     * @brief Get address table
     */
    AddressTableObject& addressTable() { return *_addressTablePtr; }
    const AddressTableObject& addressTable() const { return *_addressTablePtr; }
    
    /**
     * @brief Get association table
     */
    AssociationTableObject& associationTable() { return *_associationTablePtr; }
    const AssociationTableObject& associationTable() const { return *_associationTablePtr; }
    
    /**
     * @brief Get application program object
     */
    ApplicationProgramObject& applicationProgram() { return *_applicationProgramPtr; }
    const ApplicationProgramObject& applicationProgram() const { return *_applicationProgramPtr; }
    
    /**
     * @brief Get group object table
     */
    GroupObjectTableObject& groupObjectTable() { return *_groupObjectTablePtr; }
    const GroupObjectTableObject& groupObjectTable() const { return *_groupObjectTablePtr; }
    
#if KNX_SECURE_ENABLED
    /**
     * @brief Get security interface object
     */
    SecurityInterfaceObject& securityObject() { return *_securityObjectPtr; }
    const SecurityInterfaceObject& securityObject() const { return *_securityObjectPtr; }
#endif

    /**
     * @brief Register a reference interface object for routing and descriptors.
     *
     * An object of a type already registered replaces it.
     * @return OutOfRange when kMaxInterfaceObjects objects are registered.
     */
    util::Result<void> registerReferenceObject(GenericInterfaceObject& object);
    
    // === Property Services ===

    
    /**
     * @brief Read property value from an object
     * @param objectType Interface object type (0-65535)
     * @param objectInstance Object instance (1-based, 0=all instances)
    * @param propertyId Property ID
     * @param startIndex Start element index for array properties
     * @param elementCount Number of elements to read
     * @param value Output buffer for property value
     * @return ok, or an error code as listed at the top of this file
     */
    util::Result<void> readProperty(
        InterfaceObjectType objectType,
        InterfaceObjectInstance objectInstance,
        application::PropertyID propertyId,
        uint16_t startIndex,
        uint8_t elementCount,
        application::PropertyServiceDataBuffer& value);
    
    /**
     * @brief Write property value to an object
     * @param objectType Interface object type
     * @param objectInstance Object instance (1-based)
    * @param propertyId Property ID
     * @param startIndex Start element index for array properties
     * @param value Property value to write
     * @return ok, or an error code as listed at the top of this file
     */
    util::Result<void> writeProperty(
        InterfaceObjectType objectType,
        InterfaceObjectInstance objectInstance,
        application::PropertyID propertyId,
        uint16_t startIndex,
        std::span<const uint8_t> value);

    util::Result<void> writeProperty(
        InterfaceObjectType objectType,
        InterfaceObjectInstance objectInstance,
        application::PropertyID propertyId,
        uint16_t startIndex,
        std::initializer_list<uint8_t> value) {
        return writeProperty(objectType,
                     objectInstance,
                     propertyId,
                     startIndex,
                     std::span<const uint8_t>(value.begin(), value.end()));
    }
    
    /**
     * @brief Describe a property, as the description services report it
     *
     * Addressed the way 03/03/07 §3.4.3 addresses it: by @p propertyId when it
     * is not zero, otherwise by the 0-based @p propertyIndex ("property_index
     * = 0 shall mean the first Property"). The description carries both the
     * PID and the index, so a by-PID request learns the property's index too.
     * This is the one description both A_PropertyDescription_Read and
     * A_PropertyExtDescription_Read answer from.
     */
    util::Result<void> describeProperty(
        InterfaceObjectType objectType,
        InterfaceObjectInstance objectInstance,
        application::PropertyID propertyId,
        uint16_t propertyIndex,
        PropertyDescription& description);

    /**
     * @brief The (object type, PID) pairs that make up the commissioned state.
     *
     * saveToPersistence() stores and loadFromPersistence() restores exactly
     * these. The type, size and access of each are those of the live property;
     * this list only says which ones survive a restart.
     */
    struct PersistedProperty {
        InterfaceObjectType objectType;
        application::PropertyID propertyId;
    };
    static std::span<const PersistedProperty> persistedProperties();

    /// Whether (objectType, propertyId) is one of persistedProperties().
    static bool isPersistedProperty(InterfaceObjectType objectType, application::PropertyID propertyId);
    
    /**
     * @brief Get number of objects of a specific type
     * @param objectType Interface object type
     * @return Number of instances (0 if not found)
     */
    uint8_t getObjectCount(InterfaceObjectType objectType) const;
    
    /**
     * @brief Check if object type is supported
     * @param objectType Interface object type
     * @return true if object exists
     */
    bool hasObjectType(InterfaceObjectType objectType) const;

private:
    /// Restore the persisted properties from the active bank, of @p only
    /// when given, else of every object.
    size_t loadPersistedProperties(std::optional<InterfaceObjectType> only);

    // Owned interface objects (default configuration)
    DeviceObject _deviceObject;
    AddressTableObject _addressTable;
    AssociationTableObject _associationTable;
    ApplicationProgramObject _applicationProgram;
    GroupObjectTableObject _groupObjectTable;
#if KNX_SECURE_ENABLED
    SecurityInterfaceObject _securityObject;
#endif

    // Active object pointers (may point to external objects)
    DeviceObject* _deviceObjectPtr;
    AddressTableObject* _addressTablePtr;
    AssociationTableObject* _associationTablePtr;
    ApplicationProgramObject* _applicationProgramPtr;
    GroupObjectTableObject* _groupObjectTablePtr;
#if KNX_SECURE_ENABLED
    SecurityInterfaceObject* _securityObjectPtr;
#endif

    // Kernel-backed objects (built-ins + registered reference objects), at
    // most one per type, searched linearly.
    util::FixedVector<InterfaceObject*, kMaxInterfaceObjects> _kernelObjects;
    
    // Persistence
    std::unique_ptr<ObjectPersistence> _persistence;
    std::unique_ptr<PersistenceManager> _persistenceManager;
    bool _persistenceEnabled;
    bool _initialized;
    uint8_t _activeBank{0};  ///< See kPersistenceBankMarkerKeyId.
    /// Indexed like bankedRecordKeys().
    std::array<BankedRecordSource, kBankedRecordCount> _bankedRecordSources{};

    InterfaceObject* findKernelObject(InterfaceObjectType objectType);
    const InterfaceObject* findKernelObject(InterfaceObjectType objectType) const;

    /// The kernel object addressed by (@p objectType, @p objectInstance), or
    /// nullptr. Every object is a singleton: instance 1, or 0 ("unspecified").
    const InterfaceObject* resolveObject(InterfaceObjectType objectType, InterfaceObjectInstance objectInstance) const;

    void registerCoreObjects();

    
};

} // namespace objects
} // namespace knx
