// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

// Reference products for the export pipeline: writes the JSON that
// tools/knxprod_exporter/exporter.py renders into a knxprod.
//
// The two pilots are deliberately small and deliberately different — A is a
// switching actuator with a fully-specified DPT, B a signal indicator that
// names only a main type — so the renderer is exercised on both shapes. They
// are the fixtures tools/knxprod_exporter/test_validate_knxprod.py checks, and
// they live here as source rather than as a committed binary so that a change
// to the descriptor types shows up in the fixtures instead of silently going
// stale.
//
// Pilot C is a whole commissioned product with a parameter layout
// (knx/product/parameter_layout.hpp): a block that repeats, an option list two
// parameters share, and every form of visibility condition. It goes through
// exportCommissionedProductToJson(), the call the export of any product uses.

#include "knx/application/dpt.hpp"
#include "knx/product/commissioned_product_export.hpp"
#include "knx/product/export_descriptor.hpp"
#include "knx/product/exporter.hpp"
#include "knx/types.hpp"

#include <fstream>
#include <iostream>
#include <string>

namespace {

using knx::GroupAddress;
using knx::ManufacturerId;
using knx::application::DptValue;
using knx::application::makeDptId;
using knx::product::ExportCommunicationObjectDescriptor;
using knx::product::Medium;
using knx::product::StaticExportDescriptor;

constexpr StaticExportDescriptor<2> kPilotA{
    .identity =
        {
            .profileKey = "pilot_a_tp1_actuator",
            .productDisplayName = "Pilot A TP1 actuator",
            .manufacturerId = ManufacturerId{250},
            .medium = Medium::TP1,
            .applicationProgram = {.applicationNumber = 0, .applicationVersion = 0},
        },
    .features =
        {
            .persistenceEnabled = true,
            .securityCapable = false,
            .readResponsesEnabled = true,
            .diagnosticsEnabled = false,
        },
    .security = {},
    .capacities =
        {
            .datapointCount = 2,
            .groupAddressCapacity = 2,
            .datapointLinkCapacity = 2,
            .autoResponseQueueCapacity = 2,
            .transmissionOutcomeQueueCapacity = 4,
        },
    .communicationObjects =
        {
            ExportCommunicationObjectDescriptor{
                .exportNumber = 1,
                .logicalId = 0,
                .key = "command",
                .displayName = "Command",
                .defaultAddress = GroupAddress{1, 1, 1},
                .dpt = makeDptId(1, 1),
                .valueType = DptValue::Type::Boolean,
                .readable = true,
                .writable = true,
                .transmit = false,
                .receivable = true,
                .persisted = true,
            },
            ExportCommunicationObjectDescriptor{
                .exportNumber = 2,
                .logicalId = 1,
                .key = "status",
                .displayName = "Status",
                .defaultAddress = GroupAddress{1, 1, 2},
                .dpt = makeDptId(1, 1),
                .valueType = DptValue::Type::Boolean,
                .readable = true,
                .writable = true,
                .transmit = true,
                .receivable = false,
                .persisted = true,
            },
        },
};

// Sub-type 0 on purpose: a product may name only the main type, and the
// renderer has to emit DPT-1 rather than DPST-1-0 for it.
constexpr StaticExportDescriptor<2> kPilotB{
    .identity =
        {
            .profileKey = "pilot_b_tp1_signal_indicator",
            .productDisplayName = "Pilot B TP1 signal indicator",
            .manufacturerId = ManufacturerId{251},
            .medium = Medium::TP1,
            .applicationProgram = {.applicationNumber = 0, .applicationVersion = 0},
        },
    .features =
        {
            .persistenceEnabled = true,
            .securityCapable = false,
            .readResponsesEnabled = true,
            .diagnosticsEnabled = false,
        },
    .security = {},
    .capacities =
        {
            .datapointCount = 2,
            .groupAddressCapacity = 2,
            .datapointLinkCapacity = 2,
            .autoResponseQueueCapacity = 2,
            .transmissionOutcomeQueueCapacity = 4,
        },
    .communicationObjects =
        {
            ExportCommunicationObjectDescriptor{
                .exportNumber = 1,
                .logicalId = 0,
                .key = "command",
                .displayName = "Command",
                .defaultAddress = GroupAddress{1, 2, 10},
                .dpt = makeDptId(1, 0),
                .valueType = DptValue::Type::Boolean,
                .readable = true,
                .writable = true,
                .transmit = false,
                .receivable = true,
                .persisted = true,
            },
            ExportCommunicationObjectDescriptor{
                .exportNumber = 2,
                .logicalId = 1,
                .key = "state",
                .displayName = "State",
                .defaultAddress = GroupAddress{1, 2, 11},
                .dpt = makeDptId(1, 0),
                .valueType = DptValue::Type::Boolean,
                .readable = true,
                .writable = true,
                .transmit = true,
                .receivable = false,
                .persisted = true,
            },
        },
};

// ---------------------------------------------------------------------------
// Pilot C: two binary inputs, parameters as blocks
// ---------------------------------------------------------------------------

namespace pilot_c {

using namespace knx::product;

enum class Port : uint16_t { Input1 = 0, Input2 = 1 };

enum class Function : uint8_t { Disabled = 0, Switch = 1, Dimming = 2, Scene = 3 };
enum class Action : uint8_t { Nothing = 0, On = 1, Off = 2, Toggle = 3 };

inline constexpr auto kFunctionOptions = params::options<Function>(
    {{Function::Disabled, "Disabled"}, {Function::Switch, "Switch"}, {Function::Dimming, "Dimming"},
     {Function::Scene, "Scene"}});
inline constexpr auto kActionOptions = params::options<Action>(
    {{Action::Nothing, "Nothing"}, {Action::On, "On"}, {Action::Off, "Off"}, {Action::Toggle, "Toggle"}});

enum class Dev : uint8_t { StartupDelayS, SendCyclically, CyclicMinutes, Expert };
enum class In : uint8_t { Function, DebounceMs, OnClosing, OnOpening, Inverted, LongPressMs, SceneNumber };
enum class Diag : uint8_t { HeartbeatS };

inline constexpr auto kDeviceBlock = params::parameterBlock<Dev>(
    {.key = "device", .name = "Device"},
    params::param<Dev::StartupDelayS, uint8_t>(
        {.name = "Delay after bus voltage returns", .def = 2, .min = 0, .max = 60, .unit = "s"}),
    params::param<Dev::SendCyclically, bool>({.name = "Send the states cyclically", .def = false}),
    params::param<Dev::CyclicMinutes, uint8_t>({.name = "Cycle time",
                                                .def = 30,
                                                .min = 1,
                                                .max = 120,
                                                .unit = "min",
                                                .visible = params::when<Dev::SendCyclically>(true)}),
    params::param<Dev::Expert, bool>({.name = "Show the diagnostics page", .def = false}));

inline constexpr auto kInputBlock = params::parameterBlock<In>(
    {.key = "input", .name = "Input {n}"},
    params::param<In::Function, Function>(
        {.name = "Function", .def = Function::Switch, .options = kFunctionOptions}),
    params::param<In::DebounceMs, uint8_t>({.name = "Debounce time",
                                            .def = 30,
                                            .min = 10,
                                            .max = 200,
                                            .unit = "ms",
                                            .visible = params::whenNot<In::Function>(Function::Disabled)}),
    params::param<In::OnClosing, Action>({.name = "On closing",
                                          .def = Action::Toggle,
                                          .options = kActionOptions,
                                          .visible = params::when<In::Function>(Function::Switch)}),
    params::param<In::OnOpening, Action>({.name = "On opening",
                                          .def = Action::Nothing,
                                          .options = kActionOptions,
                                          .visible = params::when<In::Function>(Function::Switch)}),
    params::param<In::Inverted, bool>(
        {.name = "Contact is normally closed",
         .def = false,
         .visible = params::whenRange<In::Function>(Function::Switch, Function::Dimming)}),
    params::param<In::LongPressMs, uint16_t>(
        {.name = "Long press after",
         .def = 500,
         .min = 300,
         .max = 3000,
         .unit = "ms",
         .visible = params::whenAny<In::Function>(Function::Dimming, Function::Scene)}),
    params::param<In::SceneNumber, uint8_t>({.name = "Scene number",
                                             .def = 1,
                                             .min = 1,
                                             .max = 64,
                                             .visible = params::when<In::Function>(Function::Scene)}));

inline constexpr auto kDiagnosticsBlock = params::parameterBlock<Diag>(
    {.key = "diagnostics", .name = "Diagnostics", .visible = params::when<Dev::Expert>(true)},
    params::param<Diag::HeartbeatS, uint16_t>(
        {.name = "Heartbeat interval", .def = 600, .min = 10, .max = 3600, .unit = "s"}));

inline constexpr auto kParameters =
    params::parameterLayout(kDeviceBlock, params::repeat<2>(kInputBlock), kDiagnosticsBlock);

inline constexpr auto kProduct = makeCommissionedProduct<kParameters>(
    makeEndpointDefinition<Port,
                           semantics::SwitchState<Port::Input1, "input_1", "Input 1", false>,
                           semantics::SwitchState<Port::Input2, "input_2", "Input 2", false>>(
        ProductIdentity{
            .productKey = "pilot_c_tp1_binary_input",
            .productDisplayName = "Pilot C TP1 binary input",
            .manufacturerId = ManufacturerId{250},
            .medium = endpoint::Medium::TP1,
            .applicationNumber = 3,
            .applicationVersion = 1,
            .firmwareRevision = 1,
        },
        PersistencePolicy{
            .namespacePrefix = "pilot_c",
            .schemaVersion = 1,
            .persistKnxState = true,
        }));

} // namespace pilot_c

void writeJson(const char* label, const std::string& filename, const std::string& json)
{
    std::cout << "--- " << label << " Export Descriptor ---\n" << json << "\n";
    std::ofstream(filename) << json;
}

template <typename DescT>
void writeExport(const char* label, const std::string& filename, const DescT& descriptor)
{
    const auto json = knx::product::exportDescriptorToJson(descriptor);
    std::cout << "--- " << label << " Export Descriptor ---\n" << json << "\n";
    std::ofstream(filename) << json;
}

} // namespace

int main()
{
    writeExport("Pilot A", "pilot_a_export.json", kPilotA);
    writeExport("Pilot B", "pilot_b_export.json", kPilotB);
    writeJson("Pilot C", "pilot_c_export.json", knx::product::exportCommissionedProductToJson(pilot_c::kProduct));
    return 0;
}
