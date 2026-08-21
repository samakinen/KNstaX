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

#include "knx/application/dpt.hpp"
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
    .parameters = {},
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
    .parameters = {},
};

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
    return 0;
}
