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
    uint16_t applicationNumber{0};
    uint16_t applicationVersion{0};
    uint8_t firmwareRevision{0};
    uint16_t maxApduLength{254};
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
    StaticExportDescriptor<kCommunicationObjectCount, 0> exportDescriptor{};

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

constexpr application::DptValue::Type valueTypeForDpt(application::DptId dpt) noexcept
{
    switch (dpt.main) {
        case application::DptMainType::Boolean:
            return application::DptValue::Type::Boolean;
        case application::DptMainType::Controlled1Bit:
            return application::DptValue::Type::Controlled1Bit;
        case application::DptMainType::Controlled3Bit:
            return application::DptValue::Type::Controlled3Bit;
        case application::DptMainType::Character:
            return application::DptValue::Type::Character;
        case application::DptMainType::Unsigned8:
            return application::DptValue::Type::Unsigned8;
        case application::DptMainType::Signed8:
            return application::DptValue::Type::Signed8;
        case application::DptMainType::Unsigned16:
            return application::DptValue::Type::Unsigned16;
        case application::DptMainType::Signed16:
            return application::DptValue::Type::Signed16;
        case application::DptMainType::Float2Byte:
            return application::DptValue::Type::Float2Byte;
        case application::DptMainType::TimeOfDay:
            return application::DptValue::Type::TimeOfDay;
        case application::DptMainType::Date:
            return application::DptValue::Type::Date;
        case application::DptMainType::Unsigned32:
            return application::DptValue::Type::Unsigned32;
        case application::DptMainType::Signed32:
            return application::DptValue::Type::Signed32;
        case application::DptMainType::Float4Byte:
            return application::DptValue::Type::Float4Byte;
        case application::DptMainType::AccessControl:
            return application::DptValue::Type::AccessControl;
        case application::DptMainType::String:
            return application::DptValue::Type::String;
        case application::DptMainType::SceneNumber:
            return application::DptValue::Type::SceneNumber;
        case application::DptMainType::SceneControl:
            return application::DptValue::Type::SceneControl;
        case application::DptMainType::DateTime:
            return application::DptValue::Type::DateTime;
        case application::DptMainType::HvacMode:
            // Only DPT-20.102 (and the bare main type) carry the typed
            // Dpt20Mode enum; every other DPT-20 sub-type is transported as a
            // raw byte (see dpt_catalog.inc), so its runtime value type is
            // Unsigned8 to match the port's uint8_t storage.
            return (dpt.sub == 0 || dpt.sub == 102)
                       ? application::DptValue::Type::HvacMode
                       : application::DptValue::Type::Unsigned8;
        case application::DptMainType::Bitfield8:
            return application::DptValue::Type::Unsigned8;
        case application::DptMainType::Bitfield16:
            return application::DptValue::Type::Unsigned16;
        case application::DptMainType::RgbColor:
            return application::DptValue::Type::RgbColor;
        case application::DptMainType::XyYColor:
            return application::DptValue::Type::XyYColor;
        case application::DptMainType::HsvColor:
            return application::DptValue::Type::HsvColor;
        case application::DptMainType::ColorTransition:
            return application::DptValue::Type::ColorTransition;
        case application::DptMainType::Unsupported:
            break;
    }

    return application::DptValue::Type::Unsupported;
}

template <typename PortIdEnum, typename PortSpec>
constexpr auto makeRuntimeCommunicationObjectDescriptor(uint16_t slot)
    -> RuntimeCommunicationObjectDescriptor<PortIdEnum>
{
    return RuntimeCommunicationObjectDescriptor<PortIdEnum>{
        .slot = slot,
        .logicalId = PortSpec::logicalId,
        .exportNumber = slot,
        .defaultAddress = GroupAddress{},
        .dpt = PortSpec::dpt,
        .valueType = valueTypeForDpt(PortSpec::dpt),
        .direction = PortSpec::direction,
        .readable = PortSpec::readable,
        .writable = PortSpec::writable,
        .transmit = PortSpec::transmit,
        .receivable = PortSpec::receivable,
        .persisted = PortSpec::persisted,
        .readOnInit = PortSpec::readOnInit,
        .priority = PortSpec::priority,
    };
}

template <typename PortSpec>
constexpr auto makeExportCommunicationObjectDescriptorForPort(uint16_t exportNumber)
    -> ExportCommunicationObjectDescriptor
{
    return makeExportCommunicationObjectDescriptor(
        exportNumber,
        PortSpec::logicalId,
        PortSpec::key.view(),
        PortSpec::displayName.view(),
        GroupAddress{},
        PortSpec::dpt,
        valueTypeForDpt(PortSpec::dpt),
        PortSpec::readable,
        PortSpec::writable,
        PortSpec::transmit,
        PortSpec::receivable,
        PortSpec::persisted,
        PortSpec::readOnInit,
        true /* communication enable: a declared port is always wired up */);
}

template <typename PortIdEnum, typename PortsTuple, size_t... Indices>
constexpr auto makeRuntimeCommunicationObjects(std::index_sequence<Indices...>)
    -> std::array<RuntimeCommunicationObjectDescriptor<PortIdEnum>, sizeof...(Indices)>
{
    return {makeRuntimeCommunicationObjectDescriptor<PortIdEnum, std::tuple_element_t<Indices, PortsTuple>>(
        static_cast<uint16_t>(Indices))...};
}

template <typename PortsTuple, size_t... Indices>
constexpr auto makeExportCommunicationObjects(std::index_sequence<Indices...>)
    -> std::array<ExportCommunicationObjectDescriptor, sizeof...(Indices)>
{
    return {makeExportCommunicationObjectDescriptorForPort<std::tuple_element_t<Indices, PortsTuple>>(
        static_cast<uint16_t>(Indices))...};
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

// ---------------------------------------------------------------------------
// Parameter export helpers
// ---------------------------------------------------------------------------

template <typename ValueT>
constexpr ExportParameterValueKind exportParameterValueKind() noexcept
{
    if constexpr (std::is_same_v<ValueT, bool>) {
        return ExportParameterValueKind::Boolean;
    } else if constexpr (std::is_same_v<ValueT, uint8_t>) {
        return ExportParameterValueKind::Unsigned8;
    } else if constexpr (std::is_same_v<ValueT, uint16_t>) {
        return ExportParameterValueKind::Unsigned16;
    } else if constexpr (std::is_same_v<ValueT, int16_t>) {
        return ExportParameterValueKind::Signed16;
    } else if constexpr (std::is_enum_v<ValueT>) {
        return ExportParameterValueKind::Enum;
    } else if constexpr (std::is_same_v<ValueT, std::string_view>) {
        return ExportParameterValueKind::Text;
    } else if constexpr (std::is_same_v<ValueT, float>) {
        return ExportParameterValueKind::Float;
    } else if constexpr (std::is_same_v<ValueT, Dpt9Float>) {
        return ExportParameterValueKind::FloatDpt9;
    } else {
        return ExportParameterValueKind::None;
    }
}

/// Returns the number of bytes occupied by one parameter value of the given kind
/// in the KNX ProgramData serialisation block.  Returns 0 for kinds that are
/// not serialisable (None, Text).
constexpr size_t exportParameterValueByteWidth(ExportParameterValueKind kind) noexcept
{
    switch (kind) {
        case ExportParameterValueKind::Boolean:   return 1;
        case ExportParameterValueKind::Unsigned8: return 1;
        case ExportParameterValueKind::Unsigned16: return 2;
        case ExportParameterValueKind::Signed16:  return 2;
        case ExportParameterValueKind::Enum:      return 2;
        case ExportParameterValueKind::Float:     return 4;
        case ExportParameterValueKind::FloatDpt9: return 2;
        case ExportParameterValueKind::Text:      return 0;
        case ExportParameterValueKind::None:      return 0;
    }
    return 0;
}

template <typename DescriptorT>
constexpr ExportParameterDescriptor makeExportParameterDescriptorFrom(const DescriptorT& desc)
{
    using ValueT = typename DescriptorT::value_type;
    double defaultValue = 0.0;
    if constexpr (std::is_arithmetic_v<ValueT>) {
        defaultValue = static_cast<double>(desc.defaultValue);
    } else if constexpr (std::is_enum_v<ValueT>) {
        defaultValue = static_cast<double>(static_cast<int64_t>(desc.defaultValue));
    } else if constexpr (std::is_same_v<ValueT, Dpt9Float>) {
        defaultValue = static_cast<double>(desc.defaultValue.value);
    }
    auto exported = makeExportParameterDescriptor(
        static_cast<uint16_t>(DescriptorT::id),
        desc.key,
        desc.displayName.empty() ? desc.key : desc.displayName,
        exportParameterValueKind<ValueT>(),
        false, // not required by default
        static_cast<application::PropertyDataType>(0),
        defaultValue);

    // Carry the authoring-side presentation metadata through to the exporter.
    // Options are what turn an enum into an ETS drop-down; bounds and unit
    // turn a bare spin box into a field an integrator can trust.
    exported.optionCount = desc.options.count;
    for (size_t i = 0; i < desc.options.count && i < kMaxExportParameterOptions; ++i) {
        exported.options[i] =
            ExportParameterOption{desc.options.entries[i].value, desc.options.entries[i].label};
    }
    exported.minValue = desc.minValue;
    exported.maxValue = desc.maxValue;
    exported.unit = desc.unit;
    exported.group = desc.group;
    exported.visibleWhenParameterId = desc.visibleWhenParameterId;
    exported.visibleWhenValue = desc.visibleWhenValue;
    exported.groupVisibleWhenParameterId = desc.groupVisibleWhenParameterId;
    exported.groupVisibleWhenValue = desc.groupVisibleWhenValue;

    return exported;
}

template <typename DescriptorsTupleT, size_t... Indices>
constexpr auto makeExportParameterDescriptorsFromTuple(const DescriptorsTupleT& descriptors,
                                                       std::index_sequence<Indices...>)
    -> std::array<ExportParameterDescriptor, sizeof...(Indices)>
{
    return {makeExportParameterDescriptorFrom(std::get<Indices>(descriptors))...};
}

template <typename SchemaT>
constexpr auto makeExportParameterDescriptors(const SchemaT& schema)
    -> std::array<ExportParameterDescriptor, SchemaT::kParameterCount>
{
    return makeExportParameterDescriptorsFromTuple(
        schema.descriptors,
        std::make_index_sequence<SchemaT::kParameterCount>{});
}

} // namespace detail

template <typename DefinitionT>
constexpr auto compileEndpointDefinition(const DefinitionT& definition)
    -> CompiledEndpointDefinition<DefinitionT>
{
    using cleaned_definition_t = std::remove_cvref_t<DefinitionT>;
    using port_id_type = typename cleaned_definition_t::port_id_type;
    using ports_tuple = typename cleaned_definition_t::ports_tuple;

    constexpr auto indices = std::make_index_sequence<cleaned_definition_t::kPortCount>{};
    constexpr auto runtimeObjects = detail::makeRuntimeCommunicationObjects<port_id_type, ports_tuple>(indices);
    constexpr auto exportObjects = detail::makeExportCommunicationObjects<ports_tuple>(indices);

    return CompiledEndpointDefinition<DefinitionT>{
        .identity = RuntimeProductIdentity{
            .manufacturerId = definition.identity.manufacturerId,
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
        .exportDescriptor = StaticExportDescriptor<cleaned_definition_t::kPortCount, 0>{
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
                },
            .features = detail::makeExportFeatureFlags(definition.persistence,
                                                       definition.security,
                                                       runtimeObjects),
            .security = detail::makeExportSecurityDescriptor(definition.security,
                                                             cleaned_definition_t::kPortCount),
            .capacities = detail::makeExportCapacities(runtimeObjects),
            .communicationObjects = exportObjects,
            .parameters = {},
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