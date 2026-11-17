// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file endpoint_compiler.hpp
 * @brief Compile-time transformation of an endpoint definition into export descriptor and runtime metadata.
 */

#pragma once

#include "knx/product/export_descriptor.hpp"
#include "knx/product/product_api_types.hpp"
#include "knx/product/impl/model/endpoint_definition.hpp"
#include "knx/product/impl/model/port_table.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace knx::product {

struct RuntimeProductIdentity {
    ManufacturerId manufacturerId{};
    /// Not only an export detail: it decides the mask version in the Device
    /// Descriptor that ETS reads before every download.
    Medium medium{Medium::TP1};
    uint16_t applicationNumber{0};
    uint16_t applicationVersion{0};
    uint8_t firmwareRevision{0};
    uint16_t maxApduLength{static_cast<uint16_t>(config::MAX_APDU_LENGTH)};
    uint16_t hardwareSerialNumber{1};
    uint8_t hardwareVersion{1};
    std::string_view orderNumber{};
};

template <typename PortIdEnum>
struct RuntimeCommunicationObjectDescriptor {
    uint16_t slot{0};
    PortIdEnum logicalId{};
    uint16_t exportNumber{0};
    GroupAddress defaultAddress{};
    application::DptId dpt{};
    application::DptValue::Type valueType{application::DptValue::Type::Unsupported};
    PortDirection direction{PortDirection::CommandIn};
    bool readable{false};
    bool writable{false};
    bool transmit{false};
    bool receivable{false};
    bool persisted{false};
    bool readOnInit{false};
    Priority priority{Priority::Low};
};

template <size_t CommunicationObjectCountV, typename PortIdEnum>
struct RuntimeDescriptorSet {
    using port_id_type = PortIdEnum;

    static constexpr size_t kCommunicationObjectCount = CommunicationObjectCountV;

    std::array<RuntimeCommunicationObjectDescriptor<PortIdEnum>, CommunicationObjectCountV> communicationObjects{};

    constexpr std::optional<size_t> slotForLogicalId(PortIdEnum logicalId) const
    {
        for (size_t slot = 0; slot < communicationObjects.size(); ++slot) {
            if (communicationObjects[slot].logicalId == logicalId) {
                return slot;
            }
        }

        return std::nullopt;
    }

    constexpr const RuntimeCommunicationObjectDescriptor<PortIdEnum>* descriptorFor(PortIdEnum logicalId) const
    {
        const auto slot = slotForLogicalId(logicalId);
        if (!slot.has_value()) {
            return nullptr;
        }

        return &communicationObjects[*slot];
    }
};

template <typename DefinitionT>
struct CompiledEndpointDefinition {
    using definition_type = std::remove_cvref_t<DefinitionT>;
    using port_id_type = typename definition_type::port_id_type;

    static constexpr size_t kCommunicationObjectCount = definition_type::kPortCount;

    RuntimeProductIdentity identity{};
    PersistencePolicy persistence{};
    RuntimeDescriptorSet<kCommunicationObjectCount, port_id_type> runtime{};
    StaticExportDescriptor<kCommunicationObjectCount> exportDescriptor{};

    constexpr std::optional<size_t> slotForLogicalId(port_id_type logicalId) const
    {
        return runtime.slotForLogicalId(logicalId);
    }

    constexpr const RuntimeCommunicationObjectDescriptor<port_id_type>* descriptorFor(port_id_type logicalId) const
    {
        return runtime.descriptorFor(logicalId);
    }
};

namespace detail {

/// The runtime value type a port of datapoint type @p dpt carries (see
/// application::dptValueTypeFor, the one definition of it).
constexpr application::DptValue::Type valueTypeForDpt(application::DptId dpt) noexcept
{
    return application::dptValueTypeFor(dpt);
}

/// The runtime descriptor of one port, built on demand from its table entry.
template <typename PortIdEnum>
constexpr auto makeRuntimeCommunicationObjectDescriptor(uint16_t slot, const PortDescriptor& port)
    -> RuntimeCommunicationObjectDescriptor<PortIdEnum>
{
    return RuntimeCommunicationObjectDescriptor<PortIdEnum>{
        .slot = slot,
        .logicalId = static_cast<PortIdEnum>(port.logicalId),
        .exportNumber = slot,
        .defaultAddress = GroupAddress{},
        .dpt = port.dpt,
        .valueType = port.valueType,
        .direction = port.direction,
        .readable = port.readable(),
        .writable = port.writable(),
        .transmit = port.transmit(),
        .receivable = port.receivable(),
        .persisted = port.persisted(),
        .readOnInit = port.readOnInit(),
        .priority = port.priority,
    };
}

/// The export descriptor of one port: its table entry plus its names.
constexpr auto makeExportCommunicationObjectDescriptorForPort(uint16_t exportNumber,
                                                              const PortDescriptor& port,
                                                              const PortNames& names)
    -> ExportCommunicationObjectDescriptor
{
    return makeExportCommunicationObjectDescriptor(
        exportNumber,
        port.logicalId,
        names.key,
        names.displayName,
        GroupAddress{},
        port.dpt,
        port.valueType,
        port.readable(),
        port.writable(),
        port.transmit(),
        port.receivable(),
        port.persisted(),
        port.readOnInit(),
        true /* communication enable: a declared port is always wired up */);
}

template <typename PortIdEnum, size_t Count>
constexpr auto makeRuntimeCommunicationObjects(const std::array<PortDescriptor, Count>& table)
    -> std::array<RuntimeCommunicationObjectDescriptor<PortIdEnum>, Count>
{
    std::array<RuntimeCommunicationObjectDescriptor<PortIdEnum>, Count> objects{};
    for (size_t slot = 0; slot < Count; ++slot) {
        objects[slot] = makeRuntimeCommunicationObjectDescriptor<PortIdEnum>(static_cast<uint16_t>(slot),
                                                                             table[slot]);
    }
    return objects;
}

template <size_t Count>
constexpr auto makeExportCommunicationObjects(const std::array<PortDescriptor, Count>& table,
                                              const std::array<PortNames, Count>& names)
    -> std::array<ExportCommunicationObjectDescriptor, Count>
{
    std::array<ExportCommunicationObjectDescriptor, Count> objects{};
    for (size_t slot = 0; slot < Count; ++slot) {
        objects[slot] = makeExportCommunicationObjectDescriptorForPort(static_cast<uint16_t>(slot),
                                                                       table[slot],
                                                                       names[slot]);
    }
    return objects;
}

template <typename PortIdEnum, size_t Count>
constexpr ExportFeatureFlags makeExportFeatureFlags(
    const PersistencePolicy& persistence,
    const SecurityPolicy& security,
    const std::array<RuntimeCommunicationObjectDescriptor<PortIdEnum>, Count>& communicationObjects)
{
    bool persistedRuntimeState = false;
    bool readableRuntimeState = false;

    for (const auto& communicationObject : communicationObjects) {
        persistedRuntimeState = persistedRuntimeState || communicationObject.persisted;
        readableRuntimeState = readableRuntimeState || communicationObject.readable;
    }

    return ExportFeatureFlags{
        .persistenceEnabled = persistence.persistKnxState && persistedRuntimeState,
        .securityCapable = security.dataSecureCapable,
        .readResponsesEnabled = readableRuntimeState,
        .diagnosticsEnabled = false,
    };
}

constexpr ExportSecurityRequirement toExportSecurityRequirement(SecurityRequirement requirement)
{
    switch (requirement) {
        case SecurityRequirement::Auth:
            return ExportSecurityRequirement::Auth;
        case SecurityRequirement::AuthAndConf:
            return ExportSecurityRequirement::AuthAndConf;
        case SecurityRequirement::None:
            break;
    }

    return ExportSecurityRequirement::None;
}

/// Resolve the declared Data Secure policy against the actual object count.
/// A non-secure product exports an all-zero descriptor so the knxprod carries
/// no Secure markup at all, rather than advertising empty key tables.
constexpr ExportSecurityDescriptor makeExportSecurityDescriptor(const SecurityPolicy& security,
                                                                size_t communicationObjectCount)
{
    if (!security.dataSecureCapable) {
        return ExportSecurityDescriptor{false, ExportSecurityRequirement::None, 0, 0, 0};
    }

    // 0 means "one group-key slot per object" — see SecurityPolicy.
    const uint16_t groupKeys = security.groupKeyTableEntries != 0
                                   ? security.groupKeyTableEntries
                                   : static_cast<uint16_t>(communicationObjectCount);

    return ExportSecurityDescriptor{
        true,
        toExportSecurityRequirement(security.groupObjectRequirement),
        security.individualAddressEntries,
        groupKeys,
        security.p2pKeyTableEntries,
    };
}

template <typename PortIdEnum, size_t Count>
constexpr ExportCapacities makeExportCapacities(
    const std::array<RuntimeCommunicationObjectDescriptor<PortIdEnum>, Count>& communicationObjects)
{
    size_t readableCount = 0u;
    size_t transmittingCount = 0u;

    for (const auto& communicationObject : communicationObjects) {
        if (communicationObject.readable) {
            ++readableCount;
        }
        if (communicationObject.transmit) {
            ++transmittingCount;
        }
    }

    return ::knx::product::makeExportCapacities(
        Count,
        Count,
        Count,
        readableCount,
        transmittingCount == 0u ? Count : transmittingCount);
}

} // namespace detail

template <typename DefinitionT>
constexpr auto compileEndpointDefinition(const DefinitionT& definition)
    -> CompiledEndpointDefinition<DefinitionT>
{
    using cleaned_definition_t = std::remove_cvref_t<DefinitionT>;
    using port_id_type = typename cleaned_definition_t::port_id_type;

    constexpr auto runtimeObjects =
        detail::makeRuntimeCommunicationObjects<port_id_type>(kPortTable<cleaned_definition_t>);
    constexpr auto exportObjects =
        detail::makeExportCommunicationObjects(kPortTable<cleaned_definition_t>, kPortNames<cleaned_definition_t>);

    return CompiledEndpointDefinition<DefinitionT>{
        .identity = RuntimeProductIdentity{
            .manufacturerId = definition.identity.manufacturerId,
            .medium = definition.identity.medium,
            .applicationNumber = definition.identity.applicationNumber,
            .applicationVersion = definition.identity.applicationVersion,
            .firmwareRevision = definition.identity.firmwareRevision,
            .maxApduLength = definition.identity.maxApduLength,
            .hardwareSerialNumber = definition.identity.hardwareSerialNumber,
            .hardwareVersion = definition.identity.hardwareVersion,
            .orderNumber = definition.identity.orderNumber,
        },
        .persistence = definition.persistence,
        .runtime = RuntimeDescriptorSet<cleaned_definition_t::kPortCount, port_id_type>{
            .communicationObjects = runtimeObjects,
        },
        .exportDescriptor = StaticExportDescriptor<cleaned_definition_t::kPortCount>{
            .identity =
                {
                    .profileKey = definition.identity.productKey,
                    .productDisplayName = definition.identity.productDisplayName,
                    .manufacturerId = definition.identity.manufacturerId,
                    .medium = definition.identity.medium,
                    .applicationProgram =
                        {
                            .applicationNumber = definition.identity.applicationNumber,
                            .applicationVersion = definition.identity.applicationVersion,
                        },
                    .hardwareSerialNumber = definition.identity.hardwareSerialNumber,
                    .hardwareVersion = definition.identity.hardwareVersion,
                    .orderNumber = definition.identity.orderNumber,
                    .maxApduLength = definition.identity.maxApduLength,
                },
            .features = detail::makeExportFeatureFlags(definition.persistence,
                                                       definition.security,
                                                       runtimeObjects),
            .security = detail::makeExportSecurityDescriptor(definition.security,
                                                             cleaned_definition_t::kPortCount),
            .capacities = detail::makeExportCapacities(runtimeObjects),
            .communicationObjects = exportObjects,
        },
    };
}

template <typename DefinitionT>
constexpr auto makeExportDescriptor(const DefinitionT& definition)
{
    return compileEndpointDefinition(definition).exportDescriptor;
}

template <typename DefinitionT>
constexpr auto makeExportDescriptor(const CompiledEndpointDefinition<DefinitionT>& compiled)
{
    return compiled.exportDescriptor;
}

} // namespace knx::product