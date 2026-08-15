// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file exporter.hpp
 * @brief Minimal serializer for StaticExportDescriptor -> JSON.
 */

#pragma once

#include "knx/product/export_descriptor.hpp"

#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace knx::product {

/// Escape a string for embedding in a JSON string literal.  Product keys and
/// labels are author-supplied text, so quotes and backslashes must not be able
/// to break the document the Python exporter parses.
inline std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

template <typename DescT>
std::string exportDescriptorToJson(const DescT& d)
{
    std::ostringstream o;
    o << std::boolalpha;
    o << "{";
    o << "\"identity\":{\"profileKey\":\"" << d.identity.profileKey << "\",";
    o << "\"productDisplayName\":\"" << d.identity.productDisplayName << "\",";
    o << "\"manufacturerId\":" << static_cast<uint16_t>(d.identity.manufacturerId.value()) << ",";
    o << "\"medium\":" << static_cast<int>(d.identity.medium) << ",";
    o << "\"applicationProgram\":{\"number\":" << d.identity.applicationProgram.applicationNumber
      << ",\"version\":" << d.identity.applicationProgram.applicationVersion << "},";
    o << "\"hardwareSerialNumber\":" << d.identity.hardwareSerialNumber << ",";
    o << "\"hardwareVersion\":" << static_cast<unsigned>(d.identity.hardwareVersion) << ",";
    o << "\"orderNumber\":\"" << d.identity.orderNumber << "\"}";

    o << ",\"features\":{\"persistenceEnabled\":" << d.features.persistenceEnabled
      << ",\"securityCapable\":" << d.features.securityCapable
      << ",\"readResponsesEnabled\":" << d.features.readResponsesEnabled
      << ",\"diagnosticsEnabled\":" << d.features.diagnosticsEnabled << "}";

    o << ",\"security\":{\"dataSecureCapable\":" << d.security.dataSecureCapable
      << ",\"groupObjectRequirement\":\"" << exportSecurityRequirementName(d.security.groupObjectRequirement)
      << "\",\"individualAddressEntries\":" << d.security.individualAddressEntries
      << ",\"groupKeyTableEntries\":" << d.security.groupKeyTableEntries
      << ",\"p2pKeyTableEntries\":" << d.security.p2pKeyTableEntries << "}";

    o << ",\"capacities\":{\"groupObjectCount\":" << d.capacities.datapointCount
      << ",\"addressTableEntries\":" << d.capacities.groupAddressCapacity
      << ",\"associationEntries\":" << d.capacities.datapointLinkCapacity
      << ",\"autoResponseQueueCapacity\":" << d.capacities.autoResponseQueueCapacity
      << ",\"transmissionOutcomeQueueCapacity\":" << d.capacities.transmissionOutcomeQueueCapacity << "}";

    o << ",\"communicationObjects\": [";
    for (size_t i = 0; i < DescT::kCommunicationObjectCount; ++i) {
        const auto& c = d.communicationObjects[i];
        if (i) o << ",";
        o << "{";
        o << "\"exportNumber\":" << c.exportNumber << ",";
        o << "\"logicalId\":" << c.logicalId << ",";
        o << "\"key\":\"" << c.key << "\",";
        o << "\"displayName\":\"" << c.displayName << "\",";
        o << "\"defaultAddress\":" << c.defaultAddress.value() << ",";
        o << "\"dpt_main\":" << c.dpt.mainNumber() << ",";
        o << "\"dpt_sub\":" << c.dpt.sub << ",";
        o << "\"valueType\":" << static_cast<int>(c.valueType) << ",";
        o << "\"readable\":" << c.readable << ",";
        o << "\"writable\":" << c.writable << ",";
        o << "\"transmit\":" << c.transmit << ",";
        o << "\"receivable\":" << c.receivable << ",";
        o << "\"readOnInit\":" << c.readOnInit << ",";
        o << "\"communication\":" << c.communication << ",";
        o << "\"persisted\":" << c.persisted;
        o << "}";
    }
    o << "]";

    o << ",\"parameters\": [";
    for (size_t i = 0; i < DescT::kParameterCount; ++i) {
        const auto& p = d.parameters[i];
        if (i) o << ",";
        o << "{";
        o << "\"id\":" << p.id << ",";
        o << "\"key\":\"" << p.key << "\",";
        o << "\"displayName\":\"" << p.displayName << "\",";
        o << "\"valueKind\":" << static_cast<int>(p.valueKind) << ",";
        o << "\"required\":" << p.required << ",";
        // Enough digits for an exact float round-trip in the ETS default value.
        o << "\"defaultValue\":" << std::setprecision(9) << p.defaultValue << std::setprecision(6);
        if (p.optionCount > 0) {
            o << ",\"options\":[";
            for (size_t j = 0; j < p.optionCount; ++j) {
                if (j) o << ",";
                o << "{\"value\":" << p.options[j].value
                  << ",\"label\":\"" << jsonEscape(p.options[j].label) << "\"}";
            }
            o << "]";
        }
        if (p.minValue != 0.0 || p.maxValue != 0.0) {
            o << ",\"minValue\":" << std::setprecision(9) << p.minValue
              << ",\"maxValue\":" << p.maxValue << std::setprecision(6);
        }
        if (!p.unit.empty()) {
            o << ",\"unit\":\"" << jsonEscape(p.unit) << "\"";
        }
        if (!p.group.empty()) {
            o << ",\"group\":\"" << jsonEscape(p.group) << "\"";
        }
        if (p.visibleWhenParameterId != 0xFFFFu) {
            o << ",\"visibleWhenParameterId\":" << p.visibleWhenParameterId
              << ",\"visibleWhenValue\":" << p.visibleWhenValue;
        }
        if (p.groupVisibleWhenParameterId != 0xFFFFu) {
            o << ",\"groupVisibleWhenParameterId\":" << p.groupVisibleWhenParameterId
              << ",\"groupVisibleWhenValue\":" << p.groupVisibleWhenValue;
        }
        o << "}";
    }
    o << "]";

    o << "}";
    return o.str();
}

inline std::string xmlEscape(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '&': out.append("&amp;"); break;
    case '<': out.append("&lt;"); break;
    case '>': out.append("&gt;"); break;
    case '"': out.append("&quot;"); break;
    case '\'': out.append("&apos;"); break;
    default: out.push_back(c); break;
    }
  }
  return out;
}

template <typename DescT>
std::string exportDescriptorToKaenxXml(const DescT& d)
{
  std::ostringstream o;
  o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  o << "<KNXProductExport>\n";
  o << "  <Product>\n";
  o << "    <ProfileKey>" << xmlEscape(d.identity.profileKey) << "</ProfileKey>\n";
  o << "    <DisplayName>" << xmlEscape(d.identity.productDisplayName) << "</DisplayName>\n";
  o << "    <ManufacturerId>" << static_cast<uint16_t>(d.identity.manufacturerId.value()) << "</ManufacturerId>\n";
  o << "    <Medium>" << static_cast<int>(d.identity.medium) << "</Medium>\n";
  o << "    <ApplicationProgram>\n";
  o << "      <Number>" << d.identity.applicationProgram.applicationNumber << "</Number>\n";
  o << "      <Version>" << d.identity.applicationProgram.applicationVersion << "</Version>\n";
  o << "    </ApplicationProgram>\n";

  o << "    <Features>\n";
  o << "      <Persistence>" << (d.features.persistenceEnabled ? "true" : "false") << "</Persistence>\n";
  o << "      <SecurityCapable>" << (d.features.securityCapable ? "true" : "false") << "</SecurityCapable>\n";
  o << "      <ReadResponses>" << (d.features.readResponsesEnabled ? "true" : "false") << "</ReadResponses>\n";
  o << "      <Diagnostics>" << (d.features.diagnosticsEnabled ? "true" : "false") << "</Diagnostics>\n";
  o << "    </Features>\n";

  o << "    <Capacities>\n";
  o << "      <GroupObjectCount>" << d.capacities.datapointCount << "</GroupObjectCount>\n";
  o << "      <AddressTableEntries>" << d.capacities.groupAddressCapacity << "</AddressTableEntries>\n";
  o << "      <AssociationEntries>" << d.capacities.datapointLinkCapacity << "</AssociationEntries>\n";
  o << "      <AutoResponseQueueCapacity>" << d.capacities.autoResponseQueueCapacity << "</AutoResponseQueueCapacity>\n";
  o << "      <TransmissionOutcomeQueueCapacity>" << d.capacities.transmissionOutcomeQueueCapacity << "</TransmissionOutcomeQueueCapacity>\n";
  o << "    </Capacities>\n";

  o << "    <CommunicationObjects>\n";
  for (size_t i = 0; i < DescT::kCommunicationObjectCount; ++i) {
    const auto& c = d.communicationObjects[i];
    o << "      <CommunicationObject ExportNumber=\"" << c.exportNumber << "\" LogicalId=\"" << c.logicalId << "\">\n";
    o << "        <Key>" << xmlEscape(c.key) << "</Key>\n";
    o << "        <DisplayName>" << xmlEscape(c.displayName) << "</DisplayName>\n";
    // Format group address as main/middle/sub
    o << "        <DefaultAddressEncoded>" << c.defaultAddress.value() << "</DefaultAddressEncoded>\n";
    o << "        <DefaultAddressFormatted>" << static_cast<int>(c.defaultAddress.part0()) << "/" << static_cast<int>(c.defaultAddress.part1()) << "/" << static_cast<int>(c.defaultAddress.part2()) << "</DefaultAddressFormatted>\n";
    o << "        <DPT>" << c.dpt.mainNumber();
    if (c.dpt.hasSubtype()) o << "." << c.dpt.sub;
    o << "</DPT>\n";
    o << "        <ValueType>" << static_cast<int>(c.valueType) << "</ValueType>\n";
    o << "        <Readable>" << (c.readable ? "true" : "false") << "</Readable>\n";
    o << "        <Writable>" << (c.writable ? "true" : "false") << "</Writable>\n";
    o << "        <Transmit>" << (c.transmit ? "true" : "false") << "</Transmit>\n";
    o << "        <Receivable>" << (c.receivable ? "true" : "false") << "</Receivable>\n";
    o << "        <Persisted>" << (c.persisted ? "true" : "false") << "</Persisted>\n";
    o << "      </CommunicationObject>\n";
  }
  o << "    </CommunicationObjects>\n";

  o << "    <Parameters>\n";
  for (size_t i = 0; i < DescT::kParameterCount; ++i) {
    const auto& p = d.parameters[i];
    o << "      <Parameter Id=\"" << p.id << "\">\n";
    o << "        <Key>" << xmlEscape(p.key) << "</Key>\n";
    o << "        <DisplayName>" << xmlEscape(p.displayName) << "</DisplayName>\n";
    o << "        <ValueKind>" << static_cast<int>(p.valueKind) << "</ValueKind>\n";
    o << "        <Required>" << (p.required ? "true" : "false") << "</Required>\n";
    o << "      </Parameter>\n";
  }
  o << "    </Parameters>\n";

  o << "  </Product>\n";
  o << "</KNXProductExport>\n";
  return o.str();
}

} // namespace knx::product
