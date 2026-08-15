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
#include "knx/objects/security_interface_object.hpp"
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/application/property.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace knx {
namespace objects {

/**
 * @brief Property access result
 */
enum class PropertyAccessResult {
    Success,
    InvalidObject,
    InvalidProperty,
    ReadOnly,
    WriteOnly,
    InvalidValue,
    NotImplemented
};

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
        GroupObjectTableObject& groupObjectTable,
        SecurityInterfaceObject& security);
    ~InterfaceObjectManager() = default;
    
    // Disable copy, enable move
    InterfaceObjectManager(const InterfaceObjectManager&) = delete;
    InterfaceObjectManager& operator=(const InterfaceObjectManager&) = delete;
    InterfaceObjectManager(InterfaceObjectManager&&) = default;
    InterfaceObjectManager& operator=(InterfaceObjectManager&&) = default;
    
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

    /**
     * @brief Load all objects from persistence
     * @return Number of objects loaded
     */
    size_t loadFromPersistence();

private:
    /// Compare the stored layout version against this firmware's and discard
    /// the namespace when they differ.  Called from init(), before anything is
    /// loaded.
    void enforcePersistenceSchemaVersion(uint16_t schemaVersion);

public:
    
    /**
     * @brief Save all objects to persistence
     * @return Number of objects saved
     */
    size_t saveToPersistence();

    /**
     * @brief Checkpoint the Data Secure replay state, and nothing else.
     *
     * 03/05/01 §6.3.8.4 requires every Last Valid SeqNr to survive a power
     * cycle, and §6.2 the same for the Sequence Number for Tool Access. They
     * move on *every* secured telegram, so routing them through
     * saveToPersistence() would rewrite all ~23 persisted blobs at bus rate.
     * This writes the two records that actually changed.
     *
     * @return Number of records written.
     */
    size_t saveSecuritySequenceState();

    /// Restore the Sequence Number for Tool Access written by the last
    /// checkpoint. The per-partner numbers ride along inside
    /// PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE and need no separate step.
    /// @return true when a stored value was found and adopted.
    bool loadSecuritySequenceState();

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
    
    /**
     * @brief Get security interface object
     */
    SecurityInterfaceObject& securityObject() { return *_securityObjectPtr; }
    const SecurityInterfaceObject& securityObject() const { return *_securityObjectPtr; }

    /**
     * @brief Register a reference interface object for routing and descriptors.
     */
    void registerReferenceObject(GenericInterfaceObject& object);
    
    // === Property Services ===

    struct RegisteredObjectHandlers {
        // Return detailed access results so callers can distinguish invalid values
        // from missing properties, etc.
        std::function<PropertyAccessResult(
            application::PropertyID propertyId,
            uint16_t startIndex,
            uint8_t elementCount,
            application::PropertyServiceDataBuffer& value)> read;

        std::function<PropertyAccessResult(
            application::PropertyID propertyId,
            uint16_t startIndex,
            std::span<const uint8_t> value)> write;

        std::function<PropertyAccessResult(
            application::PropertyID propertyId,
            PropertyIndex propertyIndex,
            application::PropertyID& resolvedPropertyId,
            application::PropertyDataType& type,
            uint16_t& maxElements,
            uint8_t& access,
            uint8_t& readLevel,
            uint8_t& writeLevel)> describe;
    };

    /**
     * @brief Register a specific instance of an interface object type.
     *
     * This enables multi-instance handling beyond the built-in singleton objects.
     * The registered handlers are used when routing read/write/describe for that
     * (objectType, objectInstance) pair.
     */
    util::Result<void> registerObjectInstance(InterfaceObjectType objectType, InterfaceObjectInstance objectInstance, RegisteredObjectHandlers handlers);

    /**
     * @brief Unregister a previously registered object instance.
     */
    util::Result<void> unregisterObjectInstance(InterfaceObjectType objectType, InterfaceObjectInstance objectInstance);
    
    /**
     * @brief Read property value from an object
     * @param objectType Interface object type (0-65535)
     * @param objectInstance Object instance (1-based, 0=all instances)
    * @param propertyId Property ID
     * @param startIndex Start element index for array properties
     * @param elementCount Number of elements to read
     * @param value Output buffer for property value
     * @return Access result
     */
    PropertyAccessResult readProperty(
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
     * @return Access result
     */
    PropertyAccessResult writeProperty(
        InterfaceObjectType objectType,
        InterfaceObjectInstance objectInstance,
        application::PropertyID propertyId,
        uint16_t startIndex,
        std::span<const uint8_t> value);

    PropertyAccessResult writeProperty(
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
     * @brief Get property description
     * @param objectType Interface object type
     * @param objectInstance Object instance (1-based)
     * @param propertyId Property ID
     * @param propertyIndex Property index
     * @param resolvedPropertyId Output: resolved property ID (when selecting by index)
     * @param type Output: property data type
     * @param maxElements Output: maximum number of elements
     * @param access Output: access level (read/write)
     * @param readLevel Output: required authorization level to read
     * @param writeLevel Output: required authorization level to write
     * @return Access result
     */
    PropertyAccessResult describeProperty(
        InterfaceObjectType objectType,
        InterfaceObjectInstance objectInstance,
        application::PropertyID propertyId,
        PropertyIndex propertyIndex,
        application::PropertyID& resolvedPropertyId,
        application::PropertyDataType& type,
        uint16_t& maxElements,
        uint8_t& access,
        uint8_t& readLevel,
        uint8_t& writeLevel);
    
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
    // Owned interface objects (default configuration)
    DeviceObject _deviceObject;
    AddressTableObject _addressTable;
    AssociationTableObject _associationTable;
    ApplicationProgramObject _applicationProgram;
    GroupObjectTableObject _groupObjectTable;
    SecurityInterfaceObject _securityObject;

    // Active object pointers (may point to external objects)
    DeviceObject* _deviceObjectPtr;
    AddressTableObject* _addressTablePtr;
    AssociationTableObject* _associationTablePtr;
    ApplicationProgramObject* _applicationProgramPtr;
    GroupObjectTableObject* _groupObjectTablePtr;
    SecurityInterfaceObject* _securityObjectPtr;

    // Unified kernel-backed objects (built-ins + registered reference objects).
    std::map<uint16_t, InterfaceObject*> _kernelObjects;
    
    // Persistence
    std::unique_ptr<ObjectPersistence> _persistence;
    std::unique_ptr<PersistenceManager> _persistenceManager;
    bool _persistenceEnabled;
    bool _initialized;

    struct RegisteredObject {
        RegisteredObjectHandlers handlers;
    };

    std::map<uint16_t, std::map<InterfaceObjectInstance, RegisteredObject>> _registeredObjects;

    enum class DispatchTargetKind : uint8_t {
        None,
        Registered,
        Kernel,
    };

    struct DispatchTarget {
        DispatchTargetKind kind{DispatchTargetKind::None};
        InterfaceObjectInstance registeredInstance{0};
    };

    InterfaceObject* findKernelObject(InterfaceObjectType objectType);
    const InterfaceObject* findKernelObject(InterfaceObjectType objectType) const;

    const RegisteredObject* findRegisteredObject(InterfaceObjectType objectType, InterfaceObjectInstance objectInstance) const;
    RegisteredObject* findRegisteredObject(InterfaceObjectType objectType, InterfaceObjectInstance objectInstance);
    uint8_t getRegisteredObjectCount(InterfaceObjectType objectType) const;
    bool hasRegisteredObjectType(InterfaceObjectType objectType) const;
    DispatchTarget resolveDispatchTarget(InterfaceObjectType objectType, InterfaceObjectInstance objectInstance) const;
    
};

} // namespace objects
} // namespace knx
