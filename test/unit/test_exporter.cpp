// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_exporter.cpp
 * @brief Unit tests for the product exporter (JSON and XML).
 */

#include "unity.h"

#include "knx/application/dpt.hpp"
#include "knx/product/export_descriptor.hpp"
#include "knx/product/exporter.hpp"
#include "knx/types.hpp"

#include <sstream>
#include <string>

using namespace knx;
using namespace knx::product;

// ---------------------------------------------------------------------------
// Canonical test fixture – owns its own data, no dependency on any product
// ---------------------------------------------------------------------------
namespace {

constexpr StaticExportDescriptor<2> kTestDescriptor{
    .identity =
        {
            .profileKey = "test.fixture.device",
            .productDisplayName = "Test Fixture Device",
            .manufacturerId = ManufacturerId{0x00FA},
            .medium = Medium::TP1,
            .applicationProgram = {.applicationNumber = 42, .applicationVersion = 1},
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
            .datapointCount = 2,
            .groupAddressCapacity = 10,
            .datapointLinkCapacity = 10,
            .autoResponseQueueCapacity = 4,
            .transmissionOutcomeQueueCapacity = 4,
        },
    .communicationObjects =
        {
            ExportCommunicationObjectDescriptor{
                .exportNumber = 0,
                .logicalId = 0,
                .key = "switch.output",
                .displayName = "Switch Output",
                .defaultAddress = GroupAddress{1, 0, 1},
                .dpt = application::makeDptId(1, 1),
                .valueType = application::DptValue::Type::Boolean,
                .readable = false,
                .writable = true,
                .transmit = true,
                .receivable = true,
                .persisted = true,
            },
            ExportCommunicationObjectDescriptor{
                .exportNumber = 1,
                .logicalId = 1,
                .key = "dimmer.output",
                .displayName = "Dimmer Output",
                .defaultAddress = GroupAddress{1, 0, 2},
                .dpt = application::makeDptId(5, 1),
                .valueType = application::DptValue::Type::Unsigned8,
                .readable = true,
                .writable = true,
                .transmit = true,
                .receivable = true,
                .persisted = false,
            },
        },
    .parameters = {},
};

// Fixture that includes parameters, for testing parameter serialisation.
constexpr StaticExportDescriptor<1, 2> kParameterDescriptor{
    .identity =
        {
            .profileKey = "test.param.device",
            .productDisplayName = "Test Parameter Device",
            .manufacturerId = ManufacturerId{0x00FB},
            .medium = Medium::TP1,
            .applicationProgram = {.applicationNumber = 77, .applicationVersion = 2},
        },
    .features = {.persistenceEnabled = false, .securityCapable = false,
                 .readResponsesEnabled = false, .diagnosticsEnabled = false},
    .capacities = {.datapointCount = 1, .groupAddressCapacity = 1,
                   .datapointLinkCapacity = 1,
                   .autoResponseQueueCapacity = 0, .transmissionOutcomeQueueCapacity = 1},
    .communicationObjects = {
        ExportCommunicationObjectDescriptor{
            .exportNumber = 0,
            .logicalId = 0,
            .key = "relay.command",
            .displayName = "Relay Command",
            .defaultAddress = GroupAddress{2, 0, 1},
            .dpt = application::makeDptId(1, 1),
            .valueType = application::DptValue::Type::Boolean,
            .readable = false,
            .writable = true,
            .transmit = false,
            .receivable = true,
            .persisted = false,
        },
    },
    .parameters = {
        ExportParameterDescriptor{
            .id = 0,
            .key = "default_relay_state",
            .displayName = "Default Relay State",
            .propType = static_cast<application::PropertyDataType>(0),
            .valueKind = ExportParameterValueKind::Boolean,
            .required = false,
        },
        ExportParameterDescriptor{
            .id = 1,
            .key = "power_limit",
            .displayName = "Power Limit",
            .propType = static_cast<application::PropertyDataType>(0),
            .valueKind = ExportParameterValueKind::Unsigned16,
            .required = true,
        },
    },
};

} // namespace

void setUp(void) {}
void tearDown(void) {}

namespace {

size_t countOccurrences(const std::string& text, const std::string& needle)
{
    size_t count = 0;
    size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }

    return count;
}

std::string boolText(bool value)
{
    return value ? "true" : "false";
}

std::string formattedDpt(const application::DptId& dpt)
{
    std::ostringstream stream;
    stream << dpt.mainNumber();
    if (dpt.hasSubtype()) {
        stream << "." << dpt.sub;
    }
    return stream.str();
}

std::string formattedGroupAddress(const GroupAddress& address)
{
    std::ostringstream stream;
    stream << static_cast<int>(address.part0()) << "/" << static_cast<int>(address.part1()) << "/"
           << static_cast<int>(address.part2());
    return stream.str();
}

template <typename DescT>
void assertDescriptorExportMapping(const DescT& descriptor)
{
    const auto json = exportDescriptorToJson(descriptor);
    const auto xml = exportDescriptorToKaenxXml(descriptor);

    TEST_ASSERT_NOT_NULL(json.c_str());
    TEST_ASSERT_NOT_NULL(xml.c_str());

    TEST_ASSERT_TRUE(json.find(std::string(descriptor.identity.profileKey)) != std::string::npos);
    TEST_ASSERT_TRUE(xml.find(std::string(descriptor.identity.profileKey)) != std::string::npos);
    TEST_ASSERT_TRUE(json.find(std::string(descriptor.identity.productDisplayName)) != std::string::npos);
    TEST_ASSERT_TRUE(xml.find(std::string(descriptor.identity.productDisplayName)) != std::string::npos);

    const auto manufacturerId = std::to_string(static_cast<uint16_t>(descriptor.identity.manufacturerId.value()));
    TEST_ASSERT_TRUE(json.find(manufacturerId) != std::string::npos);
    TEST_ASSERT_TRUE(xml.find(manufacturerId) != std::string::npos);

    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(DescT::kCommunicationObjectCount),
                           static_cast<unsigned int>(countOccurrences(json, "\"exportNumber\":")));
    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned int>(DescT::kCommunicationObjectCount),
                           static_cast<unsigned int>(countOccurrences(xml, "<CommunicationObject ")));

    TEST_ASSERT_TRUE(json.find("\"groupObjectCount\":" + std::to_string(descriptor.capacities.datapointCount))
                     != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<GroupObjectCount>" + std::to_string(descriptor.capacities.datapointCount)
                              + "</GroupObjectCount>")
                     != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"addressTableEntries\":"
                               + std::to_string(descriptor.capacities.groupAddressCapacity))
                     != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<AddressTableEntries>"
                              + std::to_string(descriptor.capacities.groupAddressCapacity)
                              + "</AddressTableEntries>")
                     != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"associationEntries\":"
                               + std::to_string(descriptor.capacities.datapointLinkCapacity))
                     != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<AssociationEntries>"
                              + std::to_string(descriptor.capacities.datapointLinkCapacity)
                              + "</AssociationEntries>")
                     != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"autoResponseQueueCapacity\":"
                               + std::to_string(descriptor.capacities.autoResponseQueueCapacity))
                     != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<AutoResponseQueueCapacity>"
                              + std::to_string(descriptor.capacities.autoResponseQueueCapacity)
                              + "</AutoResponseQueueCapacity>")
                     != std::string::npos);

    TEST_ASSERT_TRUE(json.find("\"persistenceEnabled\":" + boolText(descriptor.features.persistenceEnabled))
                     != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"securityCapable\":" + boolText(descriptor.features.securityCapable))
                     != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<Persistence>" + boolText(descriptor.features.persistenceEnabled)
                              + "</Persistence>")
                     != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<SecurityCapable>" + boolText(descriptor.features.securityCapable)
                              + "</SecurityCapable>")
                     != std::string::npos);

    for (size_t i = 0; i < descriptor.communicationObjects.size(); ++i) {
        const auto& c = descriptor.communicationObjects[i];
        const auto encodedAddress = std::to_string(c.defaultAddress.value());
        const auto formattedAddress = formattedGroupAddress(c.defaultAddress);
        const auto dpt = formattedDpt(c.dpt);

        TEST_ASSERT_TRUE(json.find(std::string(c.key)) != std::string::npos);
        TEST_ASSERT_TRUE(xml.find(std::string(c.key)) != std::string::npos);
        TEST_ASSERT_TRUE(json.find(std::string(c.displayName)) != std::string::npos);
        TEST_ASSERT_TRUE(xml.find(std::string(c.displayName)) != std::string::npos);
        TEST_ASSERT_TRUE(json.find("\"defaultAddress\":" + encodedAddress) != std::string::npos);
        TEST_ASSERT_TRUE(xml.find("<DefaultAddressEncoded>" + encodedAddress + "</DefaultAddressEncoded>")
                         != std::string::npos);
        TEST_ASSERT_TRUE(xml.find("<DefaultAddressFormatted>" + formattedAddress + "</DefaultAddressFormatted>")
                         != std::string::npos);
        TEST_ASSERT_TRUE(xml.find("<DPT>" + dpt + "</DPT>") != std::string::npos);
        TEST_ASSERT_TRUE(xml.find(std::string(c.key)) != std::string::npos);
    }
}

} // namespace

void test_exporter_preserves_descriptor_identity_in_json_and_xml(void)
{
    TEST_ASSERT_EQUAL_STRING("Test Fixture Device", kTestDescriptor.identity.productDisplayName.data());
    assertDescriptorExportMapping(kTestDescriptor);
}

void test_exporter_json_contains_all_communication_object_keys(void)
{
    const auto json = exportDescriptorToJson(kTestDescriptor);
    TEST_ASSERT_TRUE(json.find("\"switch.output\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"dimmer.output\"") != std::string::npos);
}

void test_exporter_xml_contains_all_communication_object_keys(void)
{
    const auto xml = exportDescriptorToKaenxXml(kTestDescriptor);
    TEST_ASSERT_TRUE(xml.find("switch.output") != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("dimmer.output") != std::string::npos);
}

void test_exporter_json_includes_parameter_keys_and_value_kinds(void)
{
    const auto json = exportDescriptorToJson(kParameterDescriptor);

    // "parameters" section must be present
    TEST_ASSERT_TRUE(json.find("\"parameters\"") != std::string::npos);

    // Both parameter keys must appear
    TEST_ASSERT_TRUE(json.find("\"default_relay_state\"") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"power_limit\"") != std::string::npos);

    // valueKind integers: Boolean=1, Unsigned16=3
    TEST_ASSERT_TRUE(json.find("\"valueKind\":1") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"valueKind\":3") != std::string::npos);

    // required flag for the second parameter
    TEST_ASSERT_TRUE(json.find("\"required\":true") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("\"required\":false") != std::string::npos);
}

void test_exporter_xml_includes_parameter_keys_and_value_kinds(void)
{
    const auto xml = exportDescriptorToKaenxXml(kParameterDescriptor);

    // <Parameters> section must be present
    TEST_ASSERT_TRUE(xml.find("<Parameters>") != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("</Parameters>") != std::string::npos);

    // Both parameter keys must appear inside <Key> elements
    TEST_ASSERT_TRUE(xml.find("<Key>default_relay_state</Key>") != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<Key>power_limit</Key>") != std::string::npos);

    // Display names
    TEST_ASSERT_TRUE(xml.find("<DisplayName>Default Relay State</DisplayName>") != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<DisplayName>Power Limit</DisplayName>") != std::string::npos);

    // ValueKind integers
    TEST_ASSERT_TRUE(xml.find("<ValueKind>1</ValueKind>") != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<ValueKind>3</ValueKind>") != std::string::npos);

    // Required flags
    TEST_ASSERT_TRUE(xml.find("<Required>true</Required>") != std::string::npos);
    TEST_ASSERT_TRUE(xml.find("<Required>false</Required>") != std::string::npos);
}

int run_all_exporter_tests(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exporter_preserves_descriptor_identity_in_json_and_xml);
    RUN_TEST(test_exporter_json_contains_all_communication_object_keys);
    RUN_TEST(test_exporter_xml_contains_all_communication_object_keys);
    RUN_TEST(test_exporter_json_includes_parameter_keys_and_value_kinds);
    RUN_TEST(test_exporter_xml_includes_parameter_keys_and_value_kinds);
    return UNITY_END();
}

int main() { return run_all_exporter_tests(); }
