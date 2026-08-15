// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

// Minimal CLI to demonstrate StaticExportDescriptor serialization to JSON and XML.
// Define your own StaticExportDescriptor using the canonical export_descriptor.hpp types.

#include "knx/application/dpt.hpp"
#include "knx/product/export_descriptor.hpp"
#include "knx/product/exporter.hpp"
#include "knx/types.hpp"

#include <fstream>
#include <iostream>

namespace {

constexpr knx::product::StaticExportDescriptor<1> kExampleDescriptor{
    .identity =
        {
            .profileKey = "example.device",
            .productDisplayName = "Example Device",
            .manufacturerId = knx::ManufacturerId{0x00FA},
            .medium = knx::product::Medium::TP1,
            .applicationProgram = {.applicationNumber = 1, .applicationVersion = 1},
        },
    .features =
        {
            .persistenceEnabled = true,
            .securityCapable = false,
            .readResponsesEnabled = true,
            .diagnosticsEnabled = false,
        },
    .capacities =
        {
            .datapointCount = 1,
            .groupAddressCapacity = 10,
            .datapointLinkCapacity = 10,
            .autoResponseQueueCapacity = 4,
            .transmissionOutcomeQueueCapacity = 4,
        },
    .communicationObjects =
        {
            knx::product::ExportCommunicationObjectDescriptor{
                .exportNumber = 0,
                .logicalId = 0,
                .key = "output.switch",
                .displayName = "Switch Output",
                .defaultAddress = knx::GroupAddress{1, 0, 1},
                .dpt = knx::application::makeDptId(1, 1),
                .valueType = knx::application::DptValue::Type::Boolean,
                .readable = false,
                .writable = true,
                .transmit = true,
                .receivable = true,
                .persisted = true,
            },
        },
    .parameters = {},
};

} // namespace

int main()
{
    using namespace knx::product;

    const auto json = exportDescriptorToJson(kExampleDescriptor);
    std::cout << "--- Example Export Descriptor (JSON) ---\n" << json << "\n";
    std::ofstream("example_export.json") << json;

    const auto xml = exportDescriptorToKaenxXml(kExampleDescriptor);
    std::cout << "--- Example Export Descriptor (XML) ---\n" << xml << "\n";
    std::ofstream("example_export.xml") << xml;

    return 0;
}
