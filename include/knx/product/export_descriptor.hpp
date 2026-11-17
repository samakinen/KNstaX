// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file export_descriptor.hpp
 * @brief Internal product-to-ETS export descriptor model.
 *
 * This header defines the small neutral descriptor model that sits between
 * KNstaX product profiles and future ETS/Kaenx-facing export tooling.
 *
 * The intent is to keep product authoring in KNstaX terms while providing one
 * stable, explicit schema for export/import adapters.
 */

#pragma once

#include "knx/application/dpt.hpp"
#include "knx/config.hpp"
#include "knx/application/property.hpp"
#include "knx/product/product_api_types.hpp"
#include "knx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace knx::product {

struct ExportFeatureFlags {
    bool persistenceEnabled;
    bool securityCapable;
    bool readResponsesEnabled;
    bool diagnosticsEnabled;
};

/// Mirrors the KNX schema's ComObjectSecurityRequirements_t.  Serialised by
/// name, so the strings must match the schema enumeration exactly.
enum class ExportSecurityRequirement : uint8_t {
    None = 0,
    Auth,
    AuthAndConf,
};

constexpr std::string_view exportSecurityRequirementName(ExportSecurityRequirement requirement)
{
    switch (requirement) {
        case ExportSecurityRequirement::Auth:
            return "Auth";
        case ExportSecurityRequirement::AuthAndConf:
            return "AuthAndConf";
        case ExportSecurityRequirement::None:
            break;
    }

    return "None";
}

/// Data Secure declaration carried into ApplicationProgram/@IsSecureEnabled and
/// the MaxSecurity*Entries table-sizing attributes.
struct ExportSecurityDescriptor {
    bool dataSecureCapable;
    ExportSecurityRequirement groupObjectRequirement;
    uint16_t individualAddressEntries;
    uint16_t groupKeyTableEntries;
    uint16_t p2pKeyTableEntries;
};

struct ExportCapacities {
    size_t datapointCount;
    size_t groupAddressCapacity;
    size_t datapointLinkCapacity;
    size_t autoResponseQueueCapacity;
    size_t transmissionOutcomeQueueCapacity;
};

struct ExportApplicationProgramIdentity {
    uint16_t applicationNumber;
    uint16_t applicationVersion;
};

struct ExportProductIdentity {
    std::string_view profileKey;
    std::string_view productDisplayName;
    ManufacturerId manufacturerId;
    product::Medium medium;
    ExportApplicationProgramIdentity applicationProgram;
    // Mirrors the device's PID_HARDWARE_TYPE / PID_ORDER_INFO / PID_VERSION so
    // the exported catalogue entry describes the hardware the device actually
    // reports; ETS compares the two.
    uint16_t hardwareSerialNumber{1};
    uint8_t hardwareVersion{1};
    std::string_view orderNumber{};
    /// What the device reports in PID_MAX_APDU_LENGTH: the product's value, which
    /// never exceeds the firmware's configured KNX_MAX_APDU_LENGTH.
    uint16_t maxApduLength{static_cast<uint16_t>(config::MAX_APDU_LENGTH)};
};

struct ExportCommunicationObjectDescriptor {
    uint16_t exportNumber;
    uint16_t logicalId;
    std::string_view key;
    std::string_view displayName;
    GroupAddress defaultAddress;
    application::DptId dpt;
    application::DptValue::Type valueType;
    bool readable;
    bool writable;
    bool transmit;
    bool receivable;
    bool persisted;
    /// Value-Read-on-Initialisation (KNX flag I).
    bool readOnInit{false};
    /// Communication enable (KNX flag C).  A declared port is enabled by
    /// default; ETS can still clear it at commissioning time.
    bool communication{true};
};

enum class ExportParameterValueKind : uint8_t {
    None = 0,
    Boolean,
    Unsigned8,
    Unsigned16,
    Signed16,
    Enum,
    Text,
    /// 32-bit IEEE 754 float, stored big-endian in the KNX ProgramData block.
    /// CAUTION: knxprod TypeFloat support is patchy in third-party tooling
    /// (e.g. Kaenx-Creator ships no float validation, mis-imports the size as
    /// 16 bit, and parses Value with the system locale). For fractional
    /// quantities prefer FloatDpt9 (with integer-valued defaults) or scaled
    /// integer parameters.
    Float,
    /// KNX DPT9 2-byte half-float (0.01 resolution, ±670760.96) — the native
    /// KNX float parameter encoding (knxprod TypeFloat Encoding="DPT 9").
    /// Declared in a schema via the Dpt9Float value type. Tooling handles this
    /// encoding best; keep exported default values integer-valued so they
    /// parse identically under every locale (see Float caveat above).
    FloatDpt9,
};

/// Schema value type marking an ETS parameter as KNX DPT9 (2-byte half-float)
/// in the parameter memory, as opposed to plain `float` = 4-byte IEEE-754.
/// Behaves as a float in firmware code.
struct Dpt9Float {
    float value{0.0f};
    constexpr Dpt9Float() = default;
    constexpr Dpt9Float(float v) : value(v) {}
    constexpr operator float() const { return value; }
};

/// The identity and the communication objects of a product, for the knxprod
/// exporter. Parameters are exported from the product's parameter layout
/// (parameter_layout_export.hpp).
template <size_t CommunicationObjectCountV>
struct StaticExportDescriptor {
    static constexpr size_t kCommunicationObjectCount = CommunicationObjectCountV;

    ExportProductIdentity identity;
    ExportFeatureFlags features;
    ExportSecurityDescriptor security;
    ExportCapacities capacities;
    std::array<ExportCommunicationObjectDescriptor, CommunicationObjectCountV> communicationObjects;
};

template <typename ObjectIdT>
constexpr uint16_t exportLogicalId(ObjectIdT id)
{
    return static_cast<uint16_t>(id);
}

constexpr ExportCapacities makeExportCapacities(size_t datapointCount,
                                                size_t groupAddressCapacity,
                                                size_t datapointLinkCapacity,
                                                size_t autoResponseQueueCapacity,
                                                size_t transmissionOutcomeQueueCapacity)
{
    return ExportCapacities{
        datapointCount,
        groupAddressCapacity,
        datapointLinkCapacity,
        autoResponseQueueCapacity,
        transmissionOutcomeQueueCapacity,
    };
}

template <typename ObjectIdT>
constexpr ExportCommunicationObjectDescriptor makeExportCommunicationObjectDescriptor(
    uint16_t exportNumber,
    ObjectIdT logicalId,
    std::string_view key,
    std::string_view displayName,
    GroupAddress defaultAddress,
    application::DptId dpt,
    application::DptValue::Type valueType,
    bool readable,
    bool writable,
    bool transmit,
    bool receivable,
    bool persisted,
    bool readOnInit = false,
    bool communication = true)
{
    return ExportCommunicationObjectDescriptor{
        exportNumber,
        exportLogicalId(logicalId),
        key,
        displayName,
        defaultAddress,
        dpt,
        valueType,
        readable,
        writable,
        transmit,
        receivable,
        persisted,
        readOnInit,
        communication,
    };
}

template <typename ProductDefinitionT>
constexpr auto makeStaticExportDescriptorFromDefinition()
{
    constexpr const auto& definition = ProductDefinitionT::kDefinition;
    StaticExportDescriptor<ProductDefinitionT::kDefinition.kDatapointCount> descriptor{
        ExportProductIdentity{
            definition.identity.profileKey,
            definition.identity.productDisplayName,
            definition.identity.manufacturerId,
            definition.identity.medium,
            ExportApplicationProgramIdentity{
                definition.applicationProgram.applicationNumber,
                definition.applicationProgram.applicationVersion,
            },
        },
        ExportFeatureFlags{
            definition.features.persistenceEnabled,
            definition.features.securityEnabled,
            definition.features.readResponsesEnabled,
            definition.features.verboseDiagnostics,
        },
        // Legacy profile path: it only carries a single securityEnabled bool,
        // so the Data Secure tables fall back to the same defaults as
        // SecurityPolicy (one key slot per datapoint, one tool key).
        ExportSecurityDescriptor{
            definition.features.securityEnabled,
            ExportSecurityRequirement::None,
            1,
            static_cast<uint16_t>(definition.capacities.datapointCount),
            1,
        },
        makeExportCapacities(definition.capacities.datapointCount,
                             definition.capacities.groupAddressCapacity,
                             definition.capacities.datapointLinkCapacity,
                             definition.capacities.autoResponseQueueCapacity,
                             definition.capacities.transmissionOutcomeQueueCapacity),
        {},
    };

    for (size_t index = 0; index < ProductDefinitionT::kDefinition.kDatapointCount; ++index) {
        const auto& datapoint = definition.datapoints[index];
        descriptor.communicationObjects[index] = makeExportCommunicationObjectDescriptor(
            datapoint.metadata.exportNumber,
            datapoint.id,
            datapoint.metadata.key,
            datapoint.metadata.displayName,
            datapoint.primaryAddress,
            datapoint.dpt,
            datapoint.valueType,
            datapoint.readable,
            datapoint.writable,
            datapoint.transmit,
            datapoint.receivable,
            datapoint.persisted);
    }

    return descriptor;
}

} // namespace knx::product