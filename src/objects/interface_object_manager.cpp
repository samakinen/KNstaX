// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file interface_object_manager.cpp
 * @brief Interface object manager implementation
 */

#include "knx/objects/interface_object_manager.hpp"
#include "knx/objects/generic_interface_object.hpp"
#include "knx/objects/property_kernel.hpp"
#include "knx/util/hex.hpp"
#include "knx/util/log.hpp"
#include <optional>
#include <utility>

static const char* TAG = "KNX.ObjectMgr";

namespace {

/// Compact 16-bit persistence key: high byte = object type, low byte = property ID.
/// Instance is always 1 in the persistence path; encoding it is unnecessary.
constexpr uint16_t makePersistenceKeyId(knx::InterfaceObjectType objectType, knx::application::PropertyID propertyId)
{
    return (static_cast<uint16_t>(objectType.value()) << 8) | static_cast<uint16_t>(static_cast<uint8_t>(propertyId));
}

/// The key a persisted property has in @p bank (see kPersistenceBankMarkerKeyId).
constexpr uint16_t bankedKeyId(uint16_t keyId, uint8_t bank)
{
    return bank == 0u ? keyId
                      : static_cast<uint16_t>(keyId | knx::objects::InterfaceObjectManager::kPersistenceBank1KeyBit);
}

template <typename Visitor>
void forEachPersistedProperty(Visitor&& visit)
{
    for (const auto& property : knx::objects::InterfaceObjectManager::persistedProperties()) {
        visit(property.objectType, property.propertyId);
    }
}

knx::objects::PropertyAccessResult mapKernelResult(const knx::util::Result<void>& res, knx::AccessType accessType) {
    using knx::objects::PropertyAccessResult;
    using knx::util::ErrorCode;

    if (res.isOk()) {
        return PropertyAccessResult::Success;
    }

    switch (res.error()) {
        case ErrorCode::AccessDenied:
            return (accessType == knx::AccessType::Read) ? PropertyAccessResult::WriteOnly : PropertyAccessResult::ReadOnly;
        case ErrorCode::OperationNotSupported:
            return PropertyAccessResult::InvalidProperty;
        case ErrorCode::InvalidParameter:
        case ErrorCode::OutOfRange:
        case ErrorCode::BufferTooSmall:
        case ErrorCode::DecodeFailed:
        case ErrorCode::EncodeFailed:
            return PropertyAccessResult::InvalidValue;
        default:
            // Unexpected internal error — report as invalid value rather than
            // "not implemented" (which would tell ETS the property doesn't exist).
            return PropertyAccessResult::InvalidValue;
    }
}
} // namespace

namespace knx {
namespace objects {

std::span<const InterfaceObjectManager::PersistedProperty> InterfaceObjectManager::persistedProperties()
{
    using PID = application::PropertyID;
    constexpr auto device = InterfaceObjectType::device();
    constexpr auto addressTable = InterfaceObjectType::addressTable();
    constexpr auto associationTable = InterfaceObjectType::associationTable();
    constexpr auto applicationProgram = InterfaceObjectType::applicationProgram();
    constexpr auto security = InterfaceObjectType::security();
    // The KNXnet/IP Parameter Object is here for its routing multicast
    // address and its Secure Routing settings (backbone key, latency
    // tolerance, sync fraction): without them a device that ETS commissioned
    // for Secure Routing fell back to plain routing at the next boot. On a device without that object the reads fail and
    // nothing is stored for it.
    constexpr auto knxNetIp = InterfaceObjectType::knxNetIpParameter();

    // PID_PROG_MODE is deliberately absent: KNX devices leave programming mode
    // on power-up, and persisting it left the device stuck in the
    // Commissioning lifecycle after a reboot, muting group communication.
    static const PersistedProperty kPersisted[] = {
        {device, static_cast<PID>(DeviceProperty::SubnetAddress)},
        {device, static_cast<PID>(DeviceProperty::DeviceAddress)},
        {device, static_cast<PID>(DeviceProperty::RoutingCount)},
        {device, static_cast<PID>(DeviceProperty::MaxRetryCount)},
        {device, static_cast<PID>(DeviceProperty::ErrorFlags)},
        {device, static_cast<PID>(DeviceProperty::ProgramVersion)},
        {device, static_cast<PID>(DeviceProperty::DeviceControl)},
        {device, static_cast<PID>(DeviceProperty::RunStateControl)},
        {addressTable, static_cast<PID>(AddressTableProperty::TableData)},
        {associationTable, static_cast<PID>(AssociationTableProperty::TableData)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ProgramVersion)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ApplicationId)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ApplicationVersion)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ApplicationNumber)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ApplicationArea)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ApplicationManufacturer)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ProgramState)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ParameterStart)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ParameterEnd)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ProgramData)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ProgramName)},
        {applicationProgram, static_cast<PID>(AppProgramProperty::ProgramDescription)},
        // The key tables must survive a restart. 03/05/01 §6.3.6-6.3.8
        // "Master Reset": erase code 01h (ConfirmedRestart) leaves all three
        // "not influenced: no change", and §6.3.8 additionally requires the
        // Last Valid SeqNr values to "be saved in full at power-down and be
        // restored in full at power-up". ETS ends every secure download with a
        // ConfirmedRestart, so without this the keys it just wrote are gone
        // before they are ever used.
        {security, static_cast<PID>(SecurityProperty::SecurityMode)},
        {security, static_cast<PID>(SecurityProperty::P2PKeyTable)},
        {security, static_cast<PID>(SecurityProperty::GroupKeyTable)},
        {security, static_cast<PID>(SecurityProperty::SecurityIndividualAddressTable)},
        {security, static_cast<PID>(SecurityProperty::ToolKey)},
        {security, static_cast<PID>(SecurityProperty::SequenceNumberSending)},
        {security, static_cast<PID>(SecurityProperty::ZoneKeyTable)},
        // PID_GO_SECURITY_FLAGS says which group objects are secured at all;
        // it is downloaded alongside the key tables and is just as useless if
        // it does not survive the restart that immediately follows.
        {security, static_cast<PID>(SecurityProperty::GoSecurityFlags)},
        // PID_ROUTING_MULTICAST_ADDRESS: 03_08_03 §2.5.17 lets it be set at
        // run time and makes the new value active "after a reset", so a value
        // that did not survive that reset would never become active at all.
        {knxNetIp, static_cast<PID>(66)},
        {knxNetIp, static_cast<PID>(91)},  // PID_BACKBONE_KEY
        {knxNetIp, static_cast<PID>(95)},  // PID_MULTICAST_LATENCY_TOLERANCE
        {knxNetIp, static_cast<PID>(96)},  // PID_SYNC_LATENCY_FRACTION
    };
    return kPersisted;
}

bool InterfaceObjectManager::isPersistedProperty(InterfaceObjectType objectType,
                                                 application::PropertyID propertyId)
{
    for (const auto& property : persistedProperties()) {
        if (property.objectType == objectType && property.propertyId == propertyId) {
            return true;
        }
    }
    return false;
}

const InterfaceObjectManager::RegisteredObject* InterfaceObjectManager::findRegisteredObject(InterfaceObjectType objectType,
                                                                                             InterfaceObjectInstance objectInstance) const {
    auto tIt = _registeredObjects.find(objectType.value());
    if (tIt == _registeredObjects.end()) {
        return nullptr;
    }
    auto iIt = tIt->second.find(objectInstance);
    if (iIt == tIt->second.end()) {
        return nullptr;
    }
    return &iIt->second;
}

InterfaceObjectManager::RegisteredObject* InterfaceObjectManager::findRegisteredObject(InterfaceObjectType objectType,
                                                                                       InterfaceObjectInstance objectInstance) {
    auto tIt = _registeredObjects.find(objectType.value());
    if (tIt == _registeredObjects.end()) {
        return nullptr;
    }
    auto iIt = tIt->second.find(objectInstance);
    if (iIt == tIt->second.end()) {
        return nullptr;
    }
    return &iIt->second;
}

uint8_t InterfaceObjectManager::getRegisteredObjectCount(InterfaceObjectType objectType) const {
    auto tIt = _registeredObjects.find(objectType.value());
    if (tIt == _registeredObjects.end()) {
        return 0;
    }
    const auto n = tIt->second.size();
    return n > 255 ? 255 : static_cast<uint8_t>(n);
}

bool InterfaceObjectManager::hasRegisteredObjectType(InterfaceObjectType objectType) const {
    return getRegisteredObjectCount(objectType) != 0;
}

InterfaceObjectManager::DispatchTarget InterfaceObjectManager::resolveDispatchTarget(InterfaceObjectType objectType,
                                                                                     InterfaceObjectInstance objectInstance) const {
    const auto registeredCount = getRegisteredObjectCount(objectType);
    const bool hasKernel = (findKernelObject(objectType) != nullptr);

    // KNX objectInstance=0 means unspecified/all.
    // Resolve only when unambiguous across registered + kernel-backed singleton.
    if (objectInstance.value() == 0) {
        bool kernelReachable = hasKernel;
        if (kernelReachable && findRegisteredObject(objectType, InterfaceObjectInstance(1)) != nullptr) {
            // Registered instance 1 overrides kernel singleton instance 1.
            kernelReachable = false;
        }

        const uint8_t reachableCount = static_cast<uint8_t>(registeredCount + (kernelReachable ? 1 : 0));
        if (reachableCount != 1) {
            return {};
        }

        if (registeredCount == 1) {
            const auto& instances = _registeredObjects.at(objectType.value());
            return {DispatchTargetKind::Registered, instances.begin()->first};
        }

        if (kernelReachable) {
            return {DispatchTargetKind::Kernel, InterfaceObjectInstance(0)};
        }

        return {};
    }

    // Prefer explicitly registered instances.
    if (findRegisteredObject(objectType, objectInstance) != nullptr) {
        return {DispatchTargetKind::Registered, objectInstance};
    }

    // Kernel-backed built-ins are singleton instance 1.
    if (hasKernel && objectInstance.value() == 1) {
        return {DispatchTargetKind::Kernel, InterfaceObjectInstance(0)};
    }

    return {};
}

InterfaceObject* InterfaceObjectManager::findKernelObject(InterfaceObjectType objectType) {
    auto it = _kernelObjects.find(objectType.value());
    return (it == _kernelObjects.end()) ? nullptr : it->second;
}

const InterfaceObject* InterfaceObjectManager::findKernelObject(InterfaceObjectType objectType) const {
    auto it = _kernelObjects.find(objectType.value());
    return (it == _kernelObjects.end()) ? nullptr : it->second;
}

util::Result<void> InterfaceObjectManager::registerObjectInstance(InterfaceObjectType objectType,
                                                                   InterfaceObjectInstance objectInstance,
                                                                   RegisteredObjectHandlers handlers) {
    if (objectType == InterfaceObjectType::device() || objectInstance.value() == 0) {
        // Device object type is built-in; objectInstance=0 means "all/unspecified".
        // Registration requires a non-built-in type and a concrete 1-based instance.
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    if (!handlers.read || !handlers.write || !handlers.describe) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    auto& byInstance = _registeredObjects[objectType.value()];
    if (byInstance.find(objectInstance) != byInstance.end()) {
        return util::Result<void>::err(util::ErrorCode::AlreadyInitialized);
    }

    byInstance.emplace(objectInstance, RegisteredObject{std::move(handlers)});
    return util::Result<void>::ok();
}

util::Result<void> InterfaceObjectManager::unregisterObjectInstance(InterfaceObjectType objectType, InterfaceObjectInstance objectInstance) {
    auto tIt = _registeredObjects.find(objectType.value());
    if (tIt == _registeredObjects.end()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    const auto erased = tIt->second.erase(objectInstance);
    if (tIt->second.empty()) {
        _registeredObjects.erase(tIt);
    }
    if (erased == 0) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    return util::Result<void>::ok();
}

InterfaceObjectManager::InterfaceObjectManager()
    : _deviceObjectPtr(&_deviceObject)
    , _addressTablePtr(&_addressTable)
    , _associationTablePtr(&_associationTable)
    , _applicationProgramPtr(&_applicationProgram)
    , _groupObjectTablePtr(&_groupObjectTable)
    , _securityObjectPtr(&_securityObject)
    , _persistenceEnabled(false)
    , _initialized(false)
{
    _kernelObjects[InterfaceObjectType::device().value()] = _deviceObjectPtr;
    _kernelObjects[InterfaceObjectType::addressTable().value()] = _addressTablePtr;
    _kernelObjects[InterfaceObjectType::associationTable().value()] = _associationTablePtr;
    _kernelObjects[InterfaceObjectType::applicationProgram().value()] = _applicationProgramPtr;
    _kernelObjects[InterfaceObjectType::groupObjectTable().value()] = _groupObjectTablePtr;
    _kernelObjects[InterfaceObjectType::security().value()] = _securityObjectPtr;
}

InterfaceObjectManager::InterfaceObjectManager(
    DeviceObject& device,
    AddressTableObject& addressTable,
    AssociationTableObject& associationTable,
    ApplicationProgramObject& applicationProgram,
    GroupObjectTableObject& groupObjectTable,
    SecurityInterfaceObject& security)
    : _deviceObjectPtr(&device)
    , _addressTablePtr(&addressTable)
    , _associationTablePtr(&associationTable)
    , _applicationProgramPtr(&applicationProgram)
    , _groupObjectTablePtr(&groupObjectTable)
    , _securityObjectPtr(&security)
    , _persistenceEnabled(false)
    , _initialized(false)
{
    _kernelObjects[InterfaceObjectType::device().value()] = _deviceObjectPtr;
    _kernelObjects[InterfaceObjectType::addressTable().value()] = _addressTablePtr;
    _kernelObjects[InterfaceObjectType::associationTable().value()] = _associationTablePtr;
    _kernelObjects[InterfaceObjectType::applicationProgram().value()] = _applicationProgramPtr;
    _kernelObjects[InterfaceObjectType::groupObjectTable().value()] = _groupObjectTablePtr;
    _kernelObjects[InterfaceObjectType::security().value()] = _securityObjectPtr;
}

void InterfaceObjectManager::registerReferenceObject(GenericInterfaceObject& object) {
    _kernelObjects[object.objectType().value()] = &object;
}

util::Result<void> InterfaceObjectManager::init(bool enablePersistence,
                                               std::string_view persistenceNamespace,
                                               uint16_t schemaVersion) {
    if (_initialized) {
        KNX_LOGW(TAG, "Already initialized");
        return util::Result<void>::ok();
    }
    
    _persistenceEnabled = enablePersistence;
    
    if (_persistenceEnabled) {
        _persistence = createPersistence();
        const std::string ns = persistenceNamespace.empty() ? "knx_objects" : std::string(persistenceNamespace);
        auto initRes = _persistence->init(ns);
        if (initRes.isError()) {
            KNX_LOGE(TAG, "Failed to initialize persistence");
            return initRes;
        }

        enforcePersistenceSchemaVersion(schemaVersion);
        _activeBank = readActivePersistenceBank();

        _persistenceManager = std::make_unique<PersistenceManager>(*_persistence);
        KNX_LOGD(TAG, "Persistence enabled");
    }
    
    _initialized = true;
    KNX_LOGD(TAG, "Interface object manager initialized");
    
    return util::Result<void>::ok();
}

void InterfaceObjectManager::enforcePersistenceSchemaVersion(uint16_t schemaVersion) {
    // Persisted commissioned state is a positional byte layout: the parameter
    // block is decoded in declaration order, and the interface-object blobs are
    // written back into properties without a length negotiation.  A firmware
    // update that inserts, removes or reorders anything therefore cannot safely
    // read what the previous firmware wrote — the bytes would land in the wrong
    // fields and the device would come up mis-parameterised with no error
    // anywhere.  Discarding is the only safe response; ETS re-downloads.
    std::vector<uint8_t> stored;
    const PersistenceResult res = _persistence->loadById(kPersistenceSchemaVersionKeyId, stored);

    const auto writeCurrent = [&]() {
        const std::array<uint8_t, 2> bytes{
            static_cast<uint8_t>((schemaVersion >> 8) & 0xFFu),
            static_cast<uint8_t>(schemaVersion & 0xFFu)};
        if (_persistence->saveById(kPersistenceSchemaVersionKeyId, bytes) != PersistenceResult::Success) {
            // Non-fatal, but worth shouting about: without the marker the next
            // boot cannot tell a layout change from a first boot.
            KNX_LOGE(TAG, "Failed to store persistence schema version %u",
                     static_cast<unsigned>(schemaVersion));
        }
    };

    if (res == PersistenceResult::NotFound) {
        // First boot in this namespace, or state written before versioning
        // existed.  Either way there is nothing trustworthy to keep: an
        // unversioned blob is exactly the case this guard exists to catch.
        const auto keys = _persistence->listKeys();
        if (!keys.empty()) {
            KNX_LOGW(TAG,
                     "Persisted state carries no schema version; discarding %zu key(s) "
                     "and starting at version %u",
                     keys.size(), static_cast<unsigned>(schemaVersion));
            (void)_persistence->eraseAll();
        }
        writeCurrent();
        return;
    }

    if (res != PersistenceResult::Success || stored.size() != 2u) {
        KNX_LOGW(TAG, "Persistence schema version unreadable; discarding stored state");
        (void)_persistence->eraseAll();
        writeCurrent();
        return;
    }

    const uint16_t storedVersion =
        static_cast<uint16_t>((static_cast<uint16_t>(stored[0]) << 8) | stored[1]);
    if (storedVersion == schemaVersion) {
        return;
    }

    KNX_LOGW(TAG,
             "Persisted state was written by schema version %u, this firmware uses %u; "
             "discarding it. ETS must re-download.",
             static_cast<unsigned>(storedVersion), static_cast<unsigned>(schemaVersion));
    (void)_persistence->eraseAll();
    writeCurrent();
}

size_t InterfaceObjectManager::loadFromPersistence() {
    if (!_persistenceEnabled || !_persistenceManager) {
        KNX_LOGW(TAG, "Persistence not enabled");
        return 0;
    }

    size_t loaded = 0;
    const InterfaceObjectInstance instance(1);

    auto loadProp = [&](InterfaceObjectType objectType, application::PropertyID propertyId) {
        const uint16_t keyId = bankedKeyId(makePersistenceKeyId(objectType, propertyId), _activeBank);
        std::vector<uint8_t> data;
        const PersistenceResult res = _persistence->loadById(keyId, data);
        if (res == PersistenceResult::NotFound) {
            return;
        }
        if (res != PersistenceResult::Success) {
            KNX_LOGW(TAG, "Failed to load keyId=0x%04x (res=%d)", keyId, static_cast<int>(res));
            return;
        }
        const auto writeResult = writeProperty(objectType, instance, propertyId, 1, data);
        if (writeResult != PropertyAccessResult::Success) {
            KNX_LOGW(TAG, "Loaded keyId=0x%04x but property write failed (res=%d)", keyId, static_cast<int>(writeResult));
            return;
        }
        KNX_LOGD(TAG,
                 "Restored persistence objectType=0x%02X propertyId=0x%02X bytes=%zu data=%s",
                 static_cast<unsigned>(objectType.value()),
                 static_cast<unsigned>(static_cast<uint8_t>(propertyId)),
                 data.size(),
                 knx::util::formatHexBytes(data).c_str());
        ++loaded;
    };

    forEachPersistedProperty(loadProp);

    // Whatever the other bank holds is a save that never reached its marker
    // switch (or the bank a completed save could not finish erasing). It is
    // never read, but it would be mixed into the next save's bank, so clear it.
    erasePersistenceBank(static_cast<uint8_t>(_activeBank ^ 1u));

    KNX_LOGD(TAG, "Loaded %zu properties from persistence bank %u", loaded,
             static_cast<unsigned>(_activeBank));
    return loaded;
}

uint8_t InterfaceObjectManager::readActivePersistenceBank() {
    std::vector<uint8_t> stored;
    if (_persistence->loadById(kPersistenceBankMarkerKeyId, stored) == PersistenceResult::Success &&
        stored.size() == 1u && stored[0] <= 1u) {
        return stored[0];
    }
    // No marker: state written before banks existed, which is bank 0.
    return 0u;
}

void InterfaceObjectManager::erasePersistenceBank(uint8_t bank) {
    forEachPersistedProperty([&](InterfaceObjectType objectType, application::PropertyID propertyId) {
        (void)_persistence->eraseById(bankedKeyId(makePersistenceKeyId(objectType, propertyId), bank));
    });
}

size_t InterfaceObjectManager::saveToPersistence() {
    if (!_persistenceEnabled || !_persistenceManager) {
        KNX_LOGW(TAG, "Persistence not enabled");
        return 0;
    }

    size_t saved = 0;
    const InterfaceObjectInstance instance(1);

    // Blob-backed array properties of the Security Interface Object, i.e. the
    // ones whose whole value has to be persisted rather than element 1 of it.
    auto securityBlobProperty =
        [](application::PropertyID propertyId) -> std::optional<SecurityProperty> {
        static const SecurityProperty kBlobProperties[] = {
            SecurityProperty::P2PKeyTable,
            SecurityProperty::GroupKeyTable,
            SecurityProperty::SecurityIndividualAddressTable,
            SecurityProperty::ZoneKeyTable,
            SecurityProperty::GoSecurityFlags,
        };
        for (const auto property : kBlobProperties) {
            if (static_cast<uint8_t>(property) == static_cast<uint8_t>(propertyId)) {
                return property;
            }
        }
        return std::nullopt;
    };

    // The whole state goes into the inactive bank; the marker switch at the
    // end makes it current in one write (see kPersistenceBankMarkerKeyId).
    // Writing each property over its live copy, as this used to, meant a reset
    // part-way through a save booted with a mixture of two downloads, e.g. a
    // new address table against the old association table.
    const uint8_t nextBank = static_cast<uint8_t>(_activeBank ^ 1u);
    bool bankComplete = true;

    auto saveProp = [&](InterfaceObjectType objectType, application::PropertyID propertyId, auto getter) {
        const uint16_t baseKeyId = makePersistenceKeyId(objectType, propertyId);
        const uint16_t keyId = bankedKeyId(baseKeyId, nextBank);
        std::vector<uint8_t> data;
        if (!getter(data)) {
            // Nothing readable now (an object this device does not have, say).
            // The old bank is about to be dropped, so carry its copy across
            // rather than lose it.
            data.clear();
            if (_persistence->loadById(bankedKeyId(baseKeyId, _activeBank), data) != PersistenceResult::Success) {
                data.clear();
            }
        }

        // Avoid saving empty blobs for NVS safety; erase instead.
        if (data.empty()) {
            (void)_persistence->eraseById(keyId);
            return;
        }

        const PersistenceResult res = _persistence->saveById(keyId, data);
        if (res != PersistenceResult::Success) {
            KNX_LOGW(TAG, "Failed to save keyId=0x%04x (res=%d)", keyId, static_cast<int>(res));
            bankComplete = false;
            return;
        }
        ++saved;
    };

    auto savePropViaManager = [&](InterfaceObjectType objectType, application::PropertyID propertyId) {
        saveProp(objectType, propertyId,
                 [&](std::vector<uint8_t>& out) {
                     application::PropertyServiceDataBuffer value;
                     const auto res = readProperty(objectType,
                                                   instance,
                                                   propertyId,
                                                   1,
                                                   1,
                                                   value);
                     if (res != PropertyAccessResult::Success) {
                         return false;
                     }
                     out.assign(value.begin(), value.end());
                     return true;
                 });
    };

    auto savePropertyFromKernel = [&](InterfaceObjectType objectType,
                                      application::PropertyID propertyId,
                                      InterfaceObject& object,
                                      uint16_t elementCount) {
        saveProp(objectType, propertyId,
                 [&](std::vector<uint8_t>& out) {
                     out.clear();
                     const auto binding = object.kernelBinding();
                     if (!binding.handlers || binding.handlerCount == 0) {
                         return false;
                     }
                     if (elementCount == 0) {
                         return true;
                     }
                     const auto res = ::knx::objects::readProperty(binding.handlers,
                                                                   binding.handlerCount,
                                                                   binding.context,
                                                                   propertyId,
                                                                   1,
                                                                   elementCount,
                                                                   out);
                     return res.isOk();
                 });
    };

    forEachPersistedProperty([&](InterfaceObjectType objectType, application::PropertyID propertyId) {
        {

            // The Security Interface Object's key tables are variable-length
            // arrays held as one blob. savePropViaManager reads a *single*
            // element, which would store only the first 18-octet entry of an
            // 8-entry group key table and silently drop the rest, so save the
            // array whole and let the restore write it back at index 1.
            if (objectType == InterfaceObjectType::security() && _securityObjectPtr != nullptr) {
                if (const auto property = securityBlobProperty(propertyId)) {
                    saveProp(objectType, propertyId, [&](std::vector<uint8_t>& out) {
                        const auto* blob = _securityObjectPtr->findExtraProperty(*property);
                        // An absent table is a real state, not a skip: ETS
                        // erases these on Unload/StartLoading, and leaving the
                        // previous download's keys in NVS would resurrect them
                        // at the next boot. An empty vector makes saveProp
                        // erase the stored copy instead.
                        out = (blob != nullptr) ? *blob : std::vector<uint8_t>{};
                        return true;
                    });
                    return;
                }
            }

            if (objectType == InterfaceObjectType::addressTable() &&
                propertyId == static_cast<application::PropertyID>(AddressTableProperty::TableData)) {
                savePropertyFromKernel(objectType, propertyId, *_addressTablePtr, _addressTablePtr->entryCount());
                return;
            }

            if (objectType == InterfaceObjectType::associationTable() &&
                propertyId == static_cast<application::PropertyID>(AssociationTableProperty::TableData)) {
                savePropertyFromKernel(objectType, propertyId, *_associationTablePtr, _associationTablePtr->entryCount());
                return;
            }

            if (objectType == InterfaceObjectType::applicationProgram()) {
                if (propertyId == static_cast<application::PropertyID>(AppProgramProperty::ProgramName)) {
                    savePropertyFromKernel(objectType, propertyId, *_applicationProgramPtr,
                                           static_cast<uint16_t>(_applicationProgramPtr->getProgramName().size()));
                    return;
                }
                if (propertyId == static_cast<application::PropertyID>(AppProgramProperty::ProgramDescription)) {
                    savePropertyFromKernel(objectType, propertyId, *_applicationProgramPtr,
                                           static_cast<uint16_t>(_applicationProgramPtr->getProgramDescription().size()));
                    return;
                }
                if (propertyId == static_cast<application::PropertyID>(AppProgramProperty::ProgramData)) {
                    savePropertyFromKernel(objectType, propertyId, *_applicationProgramPtr,
                                           static_cast<uint16_t>(_applicationProgramPtr->getProgramData().size()));
                    return;
                }
            }

            savePropViaManager(objectType, propertyId);
        }
    });

    if (!bankComplete) {
        // The marker still names the old bank, so the device keeps booting
        // with the previous complete state; the partial bank is cleared at the
        // next load.
        KNX_LOGE(TAG, "Saving to persistence bank %u failed; keeping bank %u",
                 static_cast<unsigned>(nextBank), static_cast<unsigned>(_activeBank));
        return 0;
    }

    // The switch: one key, one commit.
    const std::array<uint8_t, 1> marker{nextBank};
    if (_persistence->saveById(kPersistenceBankMarkerKeyId, marker) != PersistenceResult::Success) {
        KNX_LOGE(TAG, "Failed to switch persistence bank marker to %u; keeping bank %u",
                 static_cast<unsigned>(nextBank), static_cast<unsigned>(_activeBank));
        return 0;
    }
    const uint8_t previousBank = _activeBank;
    _activeBank = nextBank;
    erasePersistenceBank(previousBank);

    KNX_LOGI(TAG, "Saved %zu properties to persistence bank %u", saved, static_cast<unsigned>(_activeBank));
    return saved;
}

size_t InterfaceObjectManager::saveSecuritySequenceState() {
    if (!_persistenceEnabled || !_persistenceManager || _securityObjectPtr == nullptr) {
        return 0;
    }

    size_t saved = 0;

    // The per-partner numbers live in the array property itself (the Security
    // Interface Object writes them through on every accepted telegram), so
    // storing that one blob stores all of them.
    // Written in place in the active bank: it is the same property the last
    // full save stored, only with newer sequence numbers.
    const uint16_t tableKeyId = bankedKeyId(
        makePersistenceKeyId(InterfaceObjectType::security(),
                             static_cast<application::PropertyID>(SecurityProperty::SecurityIndividualAddressTable)),
        _activeBank);
    if (const auto* blob = _securityObjectPtr->findExtraProperty(
            SecurityProperty::SecurityIndividualAddressTable);
        blob != nullptr && !blob->empty()) {
        if (_persistence->saveById(tableKeyId, *blob) == PersistenceResult::Success) {
            ++saved;
        }
    }

    const uint64_t toolSequence = _securityObjectPtr->getToolAccessSequence();
    std::array<uint8_t, 6> encoded{};
    for (size_t i = 0; i < encoded.size(); ++i) {
        encoded[i] = static_cast<uint8_t>((toolSequence >> (8u * (encoded.size() - 1u - i))) & 0xFFu);
    }
    if (_persistence->saveById(kToolAccessSequenceKeyId, encoded) == PersistenceResult::Success) {
        ++saved;
    }

    _securityObjectPtr->clearSequenceStateDirty();
    KNX_LOGD(TAG, "Checkpointed Data Secure sequence state (%zu record(s))", saved);
    return saved;
}

bool InterfaceObjectManager::loadSecuritySequenceState() {
    if (!_persistenceEnabled || !_persistenceManager || _securityObjectPtr == nullptr) {
        return false;
    }

    std::vector<uint8_t> stored;
    if (_persistence->loadById(kToolAccessSequenceKeyId, stored) != PersistenceResult::Success ||
        stored.size() != 6u) {
        return false;
    }

    uint64_t sequence = 0;
    for (const uint8_t octet : stored) {
        sequence = (sequence << 8) | octet;
    }
    // Never move the window backwards: a value already accepted this session
    // outranks whatever the last checkpoint managed to store.
    if (sequence > _securityObjectPtr->getToolAccessSequence()) {
        _securityObjectPtr->setToolAccessSequence(sequence);
    }
    _securityObjectPtr->clearSequenceStateDirty();
    return true;
}

bool InterfaceObjectManager::saveAuthorizationKeyTable(std::span<const uint8_t> record) {
    if (!_persistenceEnabled || !_persistenceManager) {
        return false;
    }
    if (record.empty()) {
        const auto erased = _persistence->eraseById(kAuthorizationKeyTableKeyId);
        return erased == PersistenceResult::Success || erased == PersistenceResult::NotFound;
    }
    return _persistence->saveById(kAuthorizationKeyTableKeyId, record) == PersistenceResult::Success;
}

std::vector<uint8_t> InterfaceObjectManager::loadAuthorizationKeyTable() {
    std::vector<uint8_t> stored;
    if (!_persistenceEnabled || !_persistenceManager ||
        _persistence->loadById(kAuthorizationKeyTableKeyId, stored) != PersistenceResult::Success) {
        stored.clear();
    }
    return stored;
}

PropertyAccessResult InterfaceObjectManager::readProperty(
    InterfaceObjectType objectType,
    InterfaceObjectInstance objectInstance,
    application::PropertyID propertyId,
    uint16_t startIndex,
    uint8_t elementCount,
    application::PropertyServiceDataBuffer& value)
{
    const auto target = resolveDispatchTarget(objectType, objectInstance);
    if (target.kind == DispatchTargetKind::None) {
        return PropertyAccessResult::InvalidObject;
    }

    if (target.kind == DispatchTargetKind::Registered) {
        const auto* obj = findRegisteredObject(objectType, target.registeredInstance);
        if (!obj) {
            return PropertyAccessResult::InvalidObject;
        }
        if (startIndex == 0 || elementCount == 0) {
            return PropertyAccessResult::InvalidValue;
        }
        return obj->handlers.read(propertyId, startIndex, elementCount, value);
    }

    // KNX uses 1-based indexing for property elements; index 0 is the array's
    // current element count, handled below once the property is known to exist.
    if (elementCount == 0) {
        return PropertyAccessResult::InvalidValue;
    }

    const auto* object = findKernelObject(objectType);
    if (!object) {
        return PropertyAccessResult::InvalidObject;
    }

    const auto binding = object->kernelBinding();
    if (!binding.handlers || binding.handlerCount == 0) {
        return PropertyAccessResult::NotImplemented;
    }

    // 03/03/07 §3.4.4.1: start_index 0 reads "the current number of elements of
    // the Property Value array", answered as one 2-octet element. ETS asks this
    // before downloading a table, to find out how much of it is in use.
    // 03/03/07 §3.4.4.1: start_index 0 reads "the current number of elements of
    // the Property Value array", answered as one 2-octet element. ETS asks this
    // before downloading a table, to find out how much of it is in use.
    if (startIndex == 0) {
        const auto current = ::knx::objects::propertyElementCount(binding.handlers,
                                                                  binding.handlerCount,
                                                                  binding.context,
                                                                  propertyId);
        if (current.isError()) {
            return mapKernelResult(util::Result<void>::err(current.error()), AccessType::Read);
        }
        value.resize(2);
        value[0] = static_cast<uint8_t>((current.value() >> 8) & 0xFFu);
        value[1] = static_cast<uint8_t>(current.value() & 0xFFu);
        return PropertyAccessResult::Success;
    }

    const auto res = ::knx::objects::readProperty(binding.handlers,
                                                  binding.handlerCount,
                                                  binding.context,
                                                  propertyId,
                                                  startIndex,
                                                  elementCount,
                                                  value);
    return mapKernelResult(res, AccessType::Read);
}

PropertyAccessResult InterfaceObjectManager::writeProperty(
    InterfaceObjectType objectType,
    InterfaceObjectInstance objectInstance,
    application::PropertyID propertyId,
    uint16_t startIndex,
    std::span<const uint8_t> value)
{
    const auto target = resolveDispatchTarget(objectType, objectInstance);
    if (target.kind == DispatchTargetKind::None) {
        return PropertyAccessResult::InvalidObject;
    }

    if (target.kind == DispatchTargetKind::Registered) {
        const auto* obj = findRegisteredObject(objectType, target.registeredInstance);
        if (!obj) {
            return PropertyAccessResult::InvalidObject;
        }
        if (startIndex == 0) {
            return PropertyAccessResult::InvalidValue;
        }
        return obj->handlers.write(propertyId, startIndex, value);
    }

    // Device Object policy: SubnetAddress/DeviceAddress writes must go through
    // the explicit programming-mode gated API.
    if (objectType.value() == InterfaceObjectType::device().value() &&
        (propertyId == static_cast<application::PropertyID>(DeviceProperty::SubnetAddress) ||
         propertyId == static_cast<application::PropertyID>(DeviceProperty::DeviceAddress))) {
        // Scalar properties: only allow writing the first element.
        if (startIndex != 1) {
            return PropertyAccessResult::InvalidValue;
        }
        if (value.empty()) {
            return PropertyAccessResult::InvalidValue;
        }
        const uint16_t current = _deviceObjectPtr->readIndividualAddress().raw;
        uint16_t updated = current;
        if (propertyId == static_cast<application::PropertyID>(DeviceProperty::SubnetAddress)) {
            updated = static_cast<uint16_t>((value[0] << 8) | (current & 0x00FFu));
        } else {
            updated = static_cast<uint16_t>((current & 0xFF00u) | value[0]);
        }
        // PID_SUBNET_ADDRESS / PID_DEVICE_ADDRESS via property service (and the
        // persistence restore that replays these writes at boot) are not gated
        // on programming mode — that gate applies only to the A_IndividualAddress_Write
        // management service. See DeviceObject::applyIndividualAddress.
        if (_deviceObjectPtr->applyIndividualAddress(IndividualAddress(updated)).isError()) {
            return PropertyAccessResult::InvalidValue;
        }
        return PropertyAccessResult::Success;
    }

    const auto* object = findKernelObject(objectType);
    if (!object) {
        return PropertyAccessResult::InvalidObject;
    }

    const auto binding = object->kernelBinding();
    if (!binding.handlers || binding.handlerCount == 0) {
        return PropertyAccessResult::NotImplemented;
    }

    // A write to array element 0 sets the current number of elements rather
    // than any element's value — how a Management Client shortens or clears a
    // table (03/05/01: "If the current length is set to 0 …"). ETS does this to
    // the Security Individual Address Table before re-downloading it.
    if (startIndex == 0) {
        if (value.size() != 2u) {
            return PropertyAccessResult::InvalidValue;
        }
        const uint16_t requested = static_cast<uint16_t>((value[0] << 8) | value[1]);
        const auto res = ::knx::objects::resizeProperty(binding.handlers,
                                                        binding.handlerCount,
                                                        binding.context,
                                                        propertyId,
                                                        requested);
        return mapKernelResult(res, AccessType::Write);
    }

    const auto res = ::knx::objects::writeProperty(binding.handlers,
                                                   binding.handlerCount,
                                                   binding.context,
                                                   propertyId,
                                                   startIndex,
                                                   value);
    return mapKernelResult(res, AccessType::Write);
}

PropertyAccessResult InterfaceObjectManager::describeProperty(
    InterfaceObjectType objectType,
    InterfaceObjectInstance objectInstance,
    application::PropertyID propertyId,
    uint16_t propertyIndex,
    PropertyDescription& description)
{
    const auto target = resolveDispatchTarget(objectType, objectInstance);
    if (target.kind == DispatchTargetKind::None) {
        return PropertyAccessResult::InvalidObject;
    }

    if (target.kind == DispatchTargetKind::Registered) {
        const auto* obj = findRegisteredObject(objectType, target.registeredInstance);
        if (!obj) {
            return PropertyAccessResult::InvalidObject;
        }
        return obj->handlers.describe(propertyId, propertyIndex, description);
    }

    const auto* object = findKernelObject(objectType);
    if (!object) {
        return PropertyAccessResult::InvalidObject;
    }

    // The handler table is the declaration: a property exists, and has the
    // type, size and access it has, because its handler says so. There is no
    // second list to consult.
    const auto binding = object->kernelBinding();
    if (!binding.handlers || binding.handlerCount == 0) {
        return PropertyAccessResult::NotImplemented;
    }

    const auto res = ::knx::objects::describeProperty(binding.handlers,
                                                      binding.handlerCount,
                                                      propertyId,
                                                      propertyIndex,
                                                      description);
    if (res.isError()) {
        return PropertyAccessResult::InvalidProperty;
    }
    return PropertyAccessResult::Success;
}

uint8_t InterfaceObjectManager::getObjectCount(InterfaceObjectType objectType) const {
    uint16_t count = getRegisteredObjectCount(objectType);

    if (findKernelObject(objectType) != nullptr && findRegisteredObject(objectType, InterfaceObjectInstance(1)) == nullptr) {
        ++count;
    }

    return count > 255 ? 255 : static_cast<uint8_t>(count);
}

bool InterfaceObjectManager::hasObjectType(InterfaceObjectType objectType) const {
    if (hasRegisteredObjectType(objectType)) {
        return true;
    }
    return _kernelObjects.find(objectType.value()) != _kernelObjects.end();
}

} // namespace objects
} // namespace knx
