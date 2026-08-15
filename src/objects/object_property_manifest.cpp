// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file object_property_manifest.cpp
 * @brief Unified object-property schema manifests for core and reference objects
 */

#include "knx/objects/object_property_manifest.hpp"

#include "knx/objects/address_table_object.hpp"
#include "knx/objects/application_program_object.hpp"
#include "knx/objects/association_table_object.hpp"
#include "knx/objects/device_object.hpp"
#include "knx/objects/group_object_table_object.hpp"
#include "knx/objects/reference_object_registry.hpp"
#include "knx/objects/security_interface_object.hpp"

namespace knx {
namespace objects {

namespace {

using PID = application::PropertyID;
using PDT = application::PropertyDataType;
using PA = application::PropertyAccess;

constexpr PropertyManifestEntry entry(application::PropertyID propertyId,
                                      application::PropertyDataType dataType,
                                      application::PropertyAccess access,
                                      uint16_t maxElements,
                                      PropertyPersistenceMode persistence = PropertyPersistenceMode::None,
                                      uint8_t readLevel = 0,
                                      uint8_t writeLevel = 0)
{
    return PropertyManifestEntry{{propertyId, dataType, access, maxElements, readLevel, writeLevel}, persistence};
}
constexpr PropertyManifestEntry entry_opt(application::PropertyID propertyId,
                                          application::PropertyDataType dataType,
                                          application::PropertyAccess access,
                                          uint16_t maxElements,
                                          PropertyPersistenceMode persistence = PropertyPersistenceMode::None,
                                          uint8_t readLevel = 0,
                                          uint8_t writeLevel = 0)
{
    return PropertyManifestEntry{{propertyId, dataType, access, maxElements, readLevel, writeLevel},
                                 persistence,
                                 PropertySupportLevel::Optional};
}

static const PropertyManifestEntry kDeviceManifest[] = {
    entry(PID::ObjectType, PDT::UnsignedInt, PA::ReadOnly, 1),
    entry(PID::FirmwareRevision, PDT::UnsignedChar, PA::ReadOnly, 1),
    entry(PID::ManufacturerId, PDT::UnsignedInt, PA::ReadOnly, 1),
    entry(static_cast<PID>(DeviceProperty::SerialNumber), PDT::Generic06, PA::ReadOnly, 1),
    // Device identification ETS reads during commissioning. PID_HARDWARE_TYPE
    // in particular gates whether ETS will download an application program
    // into this hardware; its access level is 3/1 per Volume 6 Profiles.
    entry(static_cast<PID>(DeviceProperty::HardwareType), PDT::Generic06, PA::ReadOnly, 1),
    entry_opt(static_cast<PID>(DeviceProperty::OrderInfo), PDT::Generic10, PA::ReadOnly, 1),
    entry_opt(static_cast<PID>(DeviceProperty::Version), PDT::UnsignedInt, PA::ReadOnly, 1),
    // Programming mode must NOT survive a restart (KNX devices leave prog
    // mode on power-up); persisting it left the device stuck in the
    // Commissioning lifecycle after a reboot, muting group communication.
    entry(PID::ProgMode, PDT::Bitset8, PA::ReadWrite, 1),
    entry(PID::MaxApduLength, PDT::UnsignedInt, PA::ReadOnly, 1),
    entry(PID::SubnetAddress, PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(PID::DeviceAddress, PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(DeviceProperty::RoutingCount), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(DeviceProperty::MaxRetryCount), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(DeviceProperty::ErrorFlags), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(DeviceProperty::ProgramVersion), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(DeviceProperty::DeviceControl), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(DeviceProperty::LoadStateControl), PDT::UnsignedChar, PA::ReadOnly, 1),
    entry(static_cast<PID>(DeviceProperty::RunStateControl), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
};

static const PropertyManifestEntry kAddressTableManifest[] = {
    entry(PID::ObjectType, PDT::UnsignedInt, PA::ReadOnly, 1),
    entry(static_cast<PID>(AddressTableProperty::LoadState), PDT::Control, PA::ReadWrite, 1),
    entry(static_cast<PID>(AddressTableProperty::TableReference), PDT::UnsignedLong, PA::ReadOnly, 1),
    entry(static_cast<PID>(AddressTableProperty::TableData), PDT::UnsignedInt, PA::ReadWrite, AddressTableDomain::kMaxEntries, PropertyPersistenceMode::Persist),
};

static const PropertyManifestEntry kAssociationTableManifest[] = {
    entry(PID::ObjectType, PDT::UnsignedInt, PA::ReadOnly, 1),
    entry(static_cast<PID>(AssociationTableProperty::LoadState), PDT::Control, PA::ReadWrite, 1),
    entry(static_cast<PID>(AssociationTableProperty::TableReference), PDT::UnsignedLong, PA::ReadOnly, 1),
    // PDT_GENERIC_04: ETS SystemB derives the association entry format from this.
    entry(static_cast<PID>(AssociationTableProperty::TableData), PDT::Generic04, PA::ReadWrite, AssociationTableDomain::kMaxEntries, PropertyPersistenceMode::Persist),
};

static const PropertyManifestEntry kApplicationProgramManifest[] = {
    entry(PID::ObjectType, PDT::UnsignedInt, PA::ReadOnly, 1),
    entry(static_cast<PID>(AppProgramProperty::LoadState), PDT::Control, PA::ReadWrite, 1),
    entry(static_cast<PID>(AppProgramProperty::TableReference), PDT::UnsignedLong, PA::ReadOnly, 1),
    entry(static_cast<PID>(AppProgramProperty::ProgramVersion), PDT::Generic05, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ApplicationId), PDT::UnsignedInt, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ApplicationVersion), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ApplicationNumber), PDT::UnsignedInt, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ApplicationArea), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ApplicationManufacturer), PDT::UnsignedInt, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ProgramState), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ProgramControl), PDT::Control, PA::WriteOnly, 1),
    entry(static_cast<PID>(AppProgramProperty::ParameterStart), PDT::UnsignedInt, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ParameterEnd), PDT::UnsignedInt, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ProgramData), PDT::GenericData, PA::ReadWrite, 0xFFFFu, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ProgramName), PDT::GenericData, PA::ReadWrite, 0xFFFFu, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(AppProgramProperty::ProgramDescription), PDT::GenericData, PA::ReadWrite, 0xFFFFu, PropertyPersistenceMode::Persist),
};

static const PropertyManifestEntry kGroupObjectTableManifest[] = {
    entry(PID::ObjectType, PDT::UnsignedInt, PA::ReadOnly, 1),
    entry(static_cast<PID>(GroupObjectTableProperty::LoadStateControl), PDT::Control, PA::ReadWrite, 1),
    entry(static_cast<PID>(GroupObjectTableProperty::TableReference), PDT::UnsignedLong, PA::ReadOnly, 1),
    entry(static_cast<PID>(GroupObjectTableProperty::TableData), PDT::GenericData, PA::ReadOnly, GroupObjectTableDomain::kMaxSerializedBytes),
    // PID_GO_DIAGNOSTICS: reached through the Function Property services, not
    // through a property read, but ETS looks the descriptor up to decide the
    // device supports Group Object Diagnostics at all.
    entry(static_cast<PID>(GroupObjectTableProperty::GoDiagnostics), PDT::Function, PA::ReadWrite, 1),
};

static const PropertyManifestEntry kSecurityManifest[] = {
    entry(PID::ObjectType, PDT::UnsignedInt, PA::ReadOnly, 1),
    entry(static_cast<PID>(SecurityProperty::LoadStateControl), PDT::Control, PA::ReadWrite, 1),
    entry(static_cast<PID>(SecurityProperty::SecurityMode), PDT::UnsignedChar, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    // The key tables must survive a restart. 03/05/01 §6.3.6-6.3.8 "Master
    // Reset": erase code 01h (ConfirmedRestart) leaves all three "not
    // influenced: no change", and §6.3.8 additionally requires the Last Valid
    // SeqNr values to "be saved in full at power-down and be restored in full
    // at power-up". ETS ends every secure download with a ConfirmedRestart, so
    // without this the keys it just wrote are gone before they are ever used
    // and the device silently falls back to plain group communication.
    entry(static_cast<PID>(SecurityProperty::P2PKeyTable), PDT::Generic20, PA::ReadWrite, 0xFFFFu, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(SecurityProperty::GroupKeyTable), PDT::Generic18, PA::ReadWrite, 0xFFFFu, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(SecurityProperty::SecurityIndividualAddressTable), PDT::Generic08, PA::ReadWrite, 0xFFFFu, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(SecurityProperty::SecurityFailuresLog), PDT::Generic07, PA::ReadWrite, 1),
    entry(static_cast<PID>(SecurityProperty::ToolKey), PDT::Generic16, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(SecurityProperty::SecurityReport), PDT::Bitset8, PA::ReadWrite, 1),
    entry(static_cast<PID>(SecurityProperty::SecurityReportControl), PDT::Bitset8, PA::ReadWrite, 1),
    entry(static_cast<PID>(SecurityProperty::SequenceNumberSending), PDT::Generic06, PA::ReadWrite, 1, PropertyPersistenceMode::Persist),
    entry(static_cast<PID>(SecurityProperty::ZoneKeyTable), PDT::Generic19, PA::ReadWrite, 0xFFFFu, PropertyPersistenceMode::Persist),
    // PID_GO_SECURITY_FLAGS says which group objects are secured at all; it is
    // downloaded once alongside the key tables and is just as useless if it
    // does not survive the restart that immediately follows.
    entry(static_cast<PID>(SecurityProperty::GoSecurityFlags), PDT::GenericData, PA::ReadWrite, 0xFFFFu, PropertyPersistenceMode::Persist),
};

} // namespace

bool isCoreObjectType(InterfaceObjectType type)
{
    return type == InterfaceObjectType::device() ||
           type == InterfaceObjectType::addressTable() ||
           type == InterfaceObjectType::associationTable() ||
           type == InterfaceObjectType::applicationProgram() ||
           type == InterfaceObjectType::groupObjectTable() ||
           type == InterfaceObjectType::security();
}

std::span<const PropertyManifestEntry> coreObjectPropertyManifestEntries(InterfaceObjectType type)
{
    if (type == InterfaceObjectType::device()) return kDeviceManifest;
    if (type == InterfaceObjectType::addressTable()) return kAddressTableManifest;
    if (type == InterfaceObjectType::associationTable()) return kAssociationTableManifest;
    if (type == InterfaceObjectType::applicationProgram()) return kApplicationProgramManifest;
    if (type == InterfaceObjectType::groupObjectTable()) return kGroupObjectTableManifest;
    if (type == InterfaceObjectType::security()) return kSecurityManifest;
    return std::span<const PropertyManifestEntry>{};
}

std::span<const PropertyManifestEntry> objectPropertyManifestEntries(InterfaceObjectType type)
{
    const auto core = coreObjectPropertyManifestEntries(type);
    if (!core.empty()) return core;
    return referenceObjectPropertyManifestEntries(type);
}

} // namespace objects
} // namespace knx
