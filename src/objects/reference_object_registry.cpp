// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file reference_object_registry.cpp
 * @brief Property manifests for reference interface objects
 */

#include "knx/objects/reference_object_registry.hpp"

#include "knx/application/property.hpp"

namespace knx {
namespace objects {

namespace {

using PDT = application::PropertyDataType;
using PA = application::PropertyAccess;

enum class ReferencePropertyId : uint8_t {
    // Interface Program
    InterfaceProgramOperationMode = 51,

    // Router Object (KNX 03/05/01 §4.4).  These are the normative names and
    // datatypes; the previous "MediumStatusExt<n>" labels for 52..57 did not
    // correspond to any KNX property and left ETS reading generic blobs where
    // it expects coupler configuration.
    RouterLineStatus = 51,          // PID_LINE_STATUS         §4.4.3
    RouterMainLcConfig = 52,        // PID_MAIN_LCCONFIG       §4.4.4
    RouterSubLcConfig = 53,         // PID_SUB_LCCONFIG        §4.4.4
    RouterMainLcGrpConfig = 54,     // PID_MAIN_LCGRPCONFIG    §4.4.5
    RouterSubLcGrpConfig = 55,      // PID_SUB_LCGRPCONFIG     §4.4.5
    RouterRouteTableControl = 56,   // PID_ROUTETABLE_CONTROL  §4.4.6 (PDT_FUNCTION)
    RouterCouplServControl = 57,    // PID_COUPL_SERV_CONTROL  §4.4.7
    RouterMaxApduLength = 58,       // PID_MAX_APDU_LENGTH     §4.4.8
    RouterL2CouplerType = 59,       // PID_L2_COUPLER_TYPE     §4.4.9
    RouterHopCount = 61,            // PID_HOP_COUNT
    RouterMedium = 63,              // PID_MEDIUM
    RouterFilterTableUse = 67,      // PID_FILTER_TABLE_USE
    RouterPl110SbcControl = 104,
    RouterRfSbcControl = 112,
    RouterIpSbcControl = 120,

    // LTE Routing Table
    LteRouteSelect = 51,
    LteRouteTable = 52,

    // cEMI Server
    CemiMediumType = 51,
    CemiCommMode = 52,
    CemiMediumAvailability = 53,
    CemiAddInfoTypes = 54,
    CemiTimeBase = 55,
    CemiTranspEnable = 56,
    CemiServerDeviceAddress = 58,
    CemiBibAtNextBlock = 59,
    CemiRfModeSelect = 60,
    CemiRfModeSupport = 61,
    CemiRfFilteringModeSelect = 62,
    CemiRfFilteringModeSupport = 63,
    CemiCommModesSupported = 64,
    CemiFilteringModeSupport = 65,
    CemiFilteringModeSelect = 66,
    CemiMaxInterfaceApduLength = 68,
    CemiMaxLocalApduLength = 69,

    // Polling Master
    PollingMasterProperty1 = 51,
    PollingMasterProperty2 = 52,
    PollingMasterProperty3 = 53,

    // KNXnet/IP Parameter
    KnxNetIpProjectInstallationId = 51,
    KnxNetIpIndividualAddress = 52,
    KnxNetIpAdditionalIndividualAddresses = 53,
    KnxNetIpCurrentIpAssignmentMethod = 54,
    KnxNetIpIpAssignmentMethod = 55,
    KnxNetIpIpCapabilities = 56,
    KnxNetIpCurrentIpAddress = 57,
    KnxNetIpCurrentSubnetMask = 58,
    KnxNetIpCurrentDefaultGateway = 59,
    KnxNetIpIpAddress = 60,
    KnxNetIpSubnetMask = 61,
    KnxNetIpDefaultGateway = 62,
    KnxNetIpDhcpBootpServer = 63,
    KnxNetIpMacAddress = 64,
    KnxNetIpSystemSetupMulticastAddress = 65,
    KnxNetIpRoutingMulticastAddress = 66,
    KnxNetIpTtl = 67,
    KnxNetIpDeviceCapabilities = 68,
    KnxNetIpDeviceState = 69,
    KnxNetIpRoutingCapabilities = 70,
    KnxNetIpPriorityFifoEnabled = 71,
    KnxNetIpQueueOverflowToIp = 72,
    KnxNetIpQueueOverflowToKnx = 73,
    KnxNetIpMsgTransmitToIp = 74,
    KnxNetIpMsgTransmitToKnx = 75,
    KnxNetIpFriendlyName = 76,
    KnxNetIpRoutingBusyWaitTime = 78,
    KnxNetIpTunnellingAddresses = 79,

    // KNXnet/IP Secure PIDs (DIN EN ISO 22510:2021, §5.7.2.5)
    KnxNetIpTunnellingUsers = 90,           // PID_TUNNELLING_USERS: access control table for non-management tunnel users
    KnxNetIpBackboneKey = 91,               // PID_BACKBONE_KEY: 16-byte AES-128 key for secure multicast group
    KnxNetIpMulticastLatencyTolerance = 92, // PID_MULTICAST_LATENCY_TOLERANCE: ms, default 2000
    KnxNetIpSyncLatencyFraction = 93,       // PID_SYNC_LATENCY_FRACTION: %, default 10
    KnxNetIpSecuredServiceFamilies = 94,    // PID_SECURED_SERVICE_FAMILIES: per-family secure enforcement flags

    // E-Mode Channel
    EModeChanNumber = 51,
    EModeChanCode = 52,
    EModeChanFlags = 53,
    EModeChanFbList = 54,
    EModeChanAdjLists = 55,
    EModeGoCcodesList = 61,
    EModeGoCflagsList = 62,
    EModeObjectLink = 63,
    EModeGoSubunit = 64,
    EModeGoNameList = 65,
    EModeGoDiagnostics = 66,
    EModeParamTypes = 70,
    EModeParamFlags = 71,
    EModeParamNames = 72,
    EModeParamUnits = 73,
    EModeParamValues = 79,

    // Text Catalogue
    TextCatalogueLocaleList = 51,
    TextCatalogueLocaleSelection = 52,
    TextCatalogueActiveLocale = 53,
    TextCatalogueString001 = 60,

    // E-Mode Device
    EModeLocalisationMode = 60,
    EModeLocalisationReport = 61,
    EModeLocalisationCommand = 62,

    // RF Medium
    RfMultiType = 51,
    RfMultiPhysicalFeatures = 52,
    RfMultiCallChannel = 53,
    RfMultiObjectLink = 54,
    RfMultiExtGaRepeated = 55,
    RfRetransmitter = 57,
    RfBidirTimeout = 60,
    RfDiagSaFilterTable = 61,
    RfDiagQualityTable = 62,
    RfDiagProbe = 63,
    RfTransmissionMode = 70,
    RfReceptionMode = 71,
    RfTestSignal = 72,
    RfFastAck = 73,
    RfFastAckActivate = 74,
    RfTypesSupported = 75,
};

constexpr application::PropertyID toPid(ReferencePropertyId id)
{
    return static_cast<application::PropertyID>(id);
}

constexpr PropertyManifestEntry entry(application::PropertyID propertyId,
                                      application::PropertyDataType dataType,
                                      application::PropertyAccess access,
                                      uint16_t maxElements,
                                      uint8_t readLevel = 0,
                                      uint8_t writeLevel = 0)
{
    return PropertyManifestEntry{{propertyId, dataType, access, maxElements, readLevel, writeLevel},
                                 PropertyPersistenceMode::None};
}

constexpr PropertyManifestEntry objectTypeEntry()
{
    return entry(application::PropertyID::ObjectType, PDT::UnsignedInt, PA::ReadOnly, 1);
}

} // namespace

bool isReferenceObjectType(InterfaceObjectType type)
{
    return type == InterfaceObjectType::interfaceProgram() ||
           type == InterfaceObjectType::eibObjectAssociation() ||
           type == InterfaceObjectType::router() ||
           type == InterfaceObjectType::lteFsm() ||
           type == InterfaceObjectType::cemiServer() ||
           type == InterfaceObjectType::pollingMaster() ||
           type == InterfaceObjectType::knxNetIpParameter() ||
           type == InterfaceObjectType::fileServer() ||
           type == InterfaceObjectType::eModeChannel() ||
           type == InterfaceObjectType::adjustedEModeChannel() ||
           type == InterfaceObjectType::textCatalogue() ||
           type == InterfaceObjectType::eModeDevice() ||
           type == InterfaceObjectType::rfMedium();
}

std::span<const PropertyManifestEntry> referenceObjectPropertyManifestEntries(InterfaceObjectType type)
{
    if (type == InterfaceObjectType::interfaceProgram()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            entry(toPid(ReferencePropertyId::InterfaceProgramOperationMode), PDT::VariableLength, PA::ReadWrite, 1),
        };
        return regs;
    }

    if (type == InterfaceObjectType::eibObjectAssociation()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
        };
        return regs;
    }

    if (type == InterfaceObjectType::router()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            // PID_LINE_STATUS is device-reported, not configured.
            entry(toPid(ReferencePropertyId::RouterLineStatus), PDT::Bitset8, PA::ReadOnly, 1),
            entry(toPid(ReferencePropertyId::RouterMainLcConfig), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterSubLcConfig), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterMainLcGrpConfig), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterSubLcGrpConfig), PDT::UnsignedChar, PA::ReadWrite, 1),
            // Routing-table management is a Function Property: ETS drives it
            // with A_FunctionPropertyCommand, not a property write (§4.4.6).
            entry(toPid(ReferencePropertyId::RouterRouteTableControl), PDT::Function, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterCouplServControl), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterMaxApduLength), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterL2CouplerType), PDT::Bitset8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterHopCount), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterMedium), PDT::UnsignedChar, PA::ReadOnly, 1),
            entry(toPid(ReferencePropertyId::RouterFilterTableUse), PDT::BinaryInfo, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterPl110SbcControl), PDT::Function, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterRfSbcControl), PDT::Function, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RouterIpSbcControl), PDT::Function, PA::ReadWrite, 1),
        };
        return regs;
    }

    if (type == InterfaceObjectType::lteFsm()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            entry(toPid(ReferencePropertyId::LteRouteSelect), PDT::GenericData, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::LteRouteTable), PDT::Generic05, PA::ReadWrite, 1),
        };
        return regs;
    }

    if (type == InterfaceObjectType::cemiServer()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            entry(toPid(ReferencePropertyId::CemiMediumType), PDT::Bitset16, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiCommMode), PDT::Enum8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiMediumAvailability), PDT::Bitset16, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiAddInfoTypes), PDT::Enum8, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::CemiTimeBase), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiTranspEnable), PDT::BinaryInfo, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiServerDeviceAddress), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiBibAtNextBlock), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiRfModeSelect), PDT::Enum8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiRfModeSupport), PDT::Bitset8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiRfFilteringModeSelect), PDT::Enum8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiRfFilteringModeSupport), PDT::Bitset8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiCommModesSupported), PDT::Bitset16, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiFilteringModeSupport), PDT::Bitset16, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiFilteringModeSelect), PDT::Bitset16, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiMaxInterfaceApduLength), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::CemiMaxLocalApduLength), PDT::UnsignedInt, PA::ReadWrite, 1),
        };
        return regs;
    }

    if (type == InterfaceObjectType::pollingMaster()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            entry(toPid(ReferencePropertyId::PollingMasterProperty1), PDT::GenericData, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::PollingMasterProperty2), PDT::GenericData, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::PollingMasterProperty3), PDT::GenericData, PA::ReadWrite, 1),
        };
        return regs;
    }

    if (type == InterfaceObjectType::knxNetIpParameter()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            entry(toPid(ReferencePropertyId::KnxNetIpProjectInstallationId), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpIndividualAddress), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpAdditionalIndividualAddresses), PDT::UnsignedInt, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::KnxNetIpCurrentIpAssignmentMethod), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpIpAssignmentMethod), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpIpCapabilities), PDT::Bitset8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpCurrentIpAddress), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpCurrentSubnetMask), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpCurrentDefaultGateway), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpIpAddress), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpSubnetMask), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpDefaultGateway), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpDhcpBootpServer), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpMacAddress), PDT::Generic06, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpSystemSetupMulticastAddress), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpRoutingMulticastAddress), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpTtl), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpDeviceCapabilities), PDT::Bitset16, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpDeviceState), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpRoutingCapabilities), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpPriorityFifoEnabled), PDT::BinaryInfo, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpQueueOverflowToIp), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpQueueOverflowToKnx), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpMsgTransmitToIp), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpMsgTransmitToKnx), PDT::UnsignedLong, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpFriendlyName), PDT::UnsignedChar, PA::ReadWrite, 30),
            entry(toPid(ReferencePropertyId::KnxNetIpRoutingBusyWaitTime), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpTunnellingAddresses), PDT::UnsignedChar, PA::ReadWrite, 0xFFFFu),
            // KNXnet/IP Secure PIDs (DIN EN ISO 22510:2021, §5.7.2.5)
            entry(toPid(ReferencePropertyId::KnxNetIpTunnellingUsers), PDT::GenericData, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::KnxNetIpBackboneKey), PDT::Generic16, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpMulticastLatencyTolerance), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpSyncLatencyFraction), PDT::UnsignedChar, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::KnxNetIpSecuredServiceFamilies), PDT::GenericData, PA::ReadWrite, 0xFFFFu),
        };
        return regs;
    }

    if (type == InterfaceObjectType::fileServer()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
        };
        return regs;
    }

    if (type == InterfaceObjectType::eModeChannel() || type == InterfaceObjectType::adjustedEModeChannel()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            entry(toPid(ReferencePropertyId::EModeChanNumber), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::EModeChanCode), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::EModeChanFlags), PDT::Generic02, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::EModeChanFbList), PDT::UnsignedInt, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeChanAdjLists), PDT::UnsignedChar, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeGoCcodesList), PDT::Generic08, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeGoCflagsList), PDT::Bitset16, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeObjectLink), PDT::Function, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::EModeGoSubunit), PDT::UnsignedChar, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeGoNameList), PDT::Generic10, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeGoDiagnostics), PDT::Function, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::EModeParamTypes), PDT::Generic10, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeParamFlags), PDT::Bitset16, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeParamNames), PDT::Generic10, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeParamUnits), PDT::Generic10, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::EModeParamValues), PDT::GenericData, PA::ReadWrite, 0xFFFFu),
        };
        return regs;
    }

    if (type == InterfaceObjectType::textCatalogue()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            entry(toPid(ReferencePropertyId::TextCatalogueLocaleList), PDT::Generic04, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::TextCatalogueLocaleSelection), PDT::Generic04, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::TextCatalogueActiveLocale), PDT::Generic04, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::TextCatalogueString001), PDT::VariableLength, PA::ReadWrite, 1),
        };
        return regs;
    }

    if (type == InterfaceObjectType::eModeDevice()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            entry(toPid(ReferencePropertyId::EModeLocalisationMode), PDT::BinaryInfo, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::EModeLocalisationReport), PDT::UnsignedInt, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::EModeLocalisationCommand), PDT::Generic03, PA::ReadWrite, 1),
        };
        return regs;
    }

    if (type == InterfaceObjectType::rfMedium()) {
        static const PropertyManifestEntry regs[] = {
            objectTypeEntry(),
            entry(toPid(ReferencePropertyId::RfMultiType), PDT::Bitset8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfMultiPhysicalFeatures), PDT::Bitset8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfMultiCallChannel), PDT::GenericData, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfMultiObjectLink), PDT::Function, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfMultiExtGaRepeated), PDT::Function, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfRetransmitter), PDT::BinaryInfo, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfBidirTimeout), PDT::Function, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfDiagSaFilterTable), PDT::Generic03, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::RfDiagQualityTable), PDT::Generic04, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::RfDiagProbe), PDT::Function, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfTransmissionMode), PDT::Enum8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfReceptionMode), PDT::Enum8, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfTestSignal), PDT::Generic02, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfFastAck), PDT::Generic02, PA::ReadWrite, 0xFFFFu),
            entry(toPid(ReferencePropertyId::RfFastAckActivate), PDT::BinaryInfo, PA::ReadWrite, 1),
            entry(toPid(ReferencePropertyId::RfTypesSupported), PDT::Bitset8, PA::ReadWrite, 1),
        };
        return regs;
    }

    return std::span<const PropertyManifestEntry>{};
}

} // namespace objects
} // namespace knx
