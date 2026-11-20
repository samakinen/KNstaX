// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen
/**
 * @file parameter_layout_export.hpp
 * @brief A parameter layout as JSON for the knxprod exporter (host tools only)
 *
 * Writes the three sections tools/knxprod_exporter/exporter.py reads for a
 * product with a parameter layout (docs/design/parameter-model.md §3.8):
 *
 *   "parameterImage":  size and fingerprint of the image
 *   "optionTypes":     every option list once, however many parameters use it
 *   "parameterBlocks": the blocks with base, stride and repeat count, their
 *                      parameters with the offset in the block, and their
 *                      sections (pages inside a block)
 *
 * Every offset comes from the layout the firmware compiles. The Python side
 * places a parameter at base + instance × stride + offset and derives nothing.
 *
 * This header takes the layout object by reference and reads its names: it is
 * for the exporter, not for firmware.
 */

#pragma once

#include "knx/product/exporter.hpp"
#include "knx/product/parameter_layout.hpp"

#include <cstdint>
#include <iomanip>
#include <string_view>
#include <sstream>
#include <string>
#include <vector>

namespace knx::product {

namespace detail {

inline void writeConditionJson(std::ostringstream& o, const params::Condition& condition,
                               const std::vector<std::string_view>& blocks)
{
    size_t block = 0;
    while (block < blocks.size() && blocks[block] != condition.block) {
        ++block;
    }
    const char* op = condition.op == params::ConditionOp::NotEqual ? "notEqual"
                     : condition.op == params::ConditionOp::Range  ? "range"
                                                                   : "anyOf";
    o << "{\"block\":" << block << ",\"parameter\":" << condition.parameterId << ",\"op\":\"" << op
      << "\",\"values\":[";
    for (size_t i = 0; i < condition.valueCount; ++i) {
        o << (i ? "," : "") << condition.values[i];
    }
    o << "]}";
}

} // namespace detail

/// The layout as the JSON members "parameterImage", "optionTypes" and
/// "parameterBlocks", without the enclosing braces.
template <typename... Rs>
std::string parameterLayoutToJson(const params::ParameterLayout<Rs...>& layout)
{
    using Layout = params::ParameterLayout<Rs...>;

    std::vector<params::ParameterInfo> parameters;
    params::forEachParameter(layout, [&](const params::ParameterInfo& info) { parameters.push_back(info); });

    // Blocks in image order, and each option list once per width it is used at.
    std::vector<std::string_view> blocks;
    struct OptionType {
        std::span<const params::Option> options;
        size_t width;
    };
    std::vector<OptionType> optionTypes;
    const auto optionTypeOf = [&](const params::ParameterInfo& info) {
        for (size_t i = 0; i < optionTypes.size(); ++i) {
            if (optionTypes[i].options.data() == info.options.data()
                && optionTypes[i].options.size() == info.options.size() && optionTypes[i].width == info.width) {
                return i;
            }
        }
        optionTypes.push_back(OptionType{info.options, info.width});
        return optionTypes.size() - 1u;
    };
    for (const auto& info : parameters) {
        if (blocks.empty() || blocks.back() != info.block) {
            blocks.push_back(info.block);
        }
        if (!info.options.empty()) {
            (void)optionTypeOf(info);
        }
    }

    std::ostringstream o;
    o << "\"parameterImage\":{\"size\":" << Layout::kSize << ",\"fingerprint\":" << Layout::kFingerprint << "}";

    o << ",\"optionTypes\":[";
    for (size_t i = 0; i < optionTypes.size(); ++i) {
        o << (i ? "," : "") << "{\"id\":" << i << ",\"sizeInBit\":" << optionTypes[i].width * 8u << ",\"options\":[";
        for (size_t j = 0; j < optionTypes[i].options.size(); ++j) {
            const auto& option = optionTypes[i].options[j];
            o << (j ? "," : "") << "{\"value\":" << option.value << ",\"label\":\"" << jsonEscape(option.label)
              << "\"}";
        }
        o << "]}";
    }
    o << "]";

    // The sections of the block that is being written, closed with its parameters.
    std::string sections;
    int lastSection = -1;
    const auto closeBlock = [&] {
        o << "],\"sections\":[" << sections << "]}";
        sections.clear();
        lastSection = -1;
    };

    o << ",\"parameterBlocks\":[";
    size_t blockIndex = 0;
    for (size_t i = 0; i < parameters.size(); ++i) {
        const auto& info = parameters[i];
        const bool first = i == 0u || parameters[i - 1u].block != info.block;
        if (first) {
            if (i) {
                closeBlock();
                o << ",";
            }
            o << "{\"index\":" << blockIndex++ << ",\"key\":\"" << jsonEscape(info.blockKey)
              << "\",\"name\":\"" << jsonEscape(info.blockName) << "\",\"count\":" << info.blockCount
              << ",\"base\":" << info.blockBase << ",\"stride\":" << info.blockStride;
            if (info.blockVisible.op != params::ConditionOp::Always) {
                o << ",\"visible\":";
                detail::writeConditionJson(o, info.blockVisible, blocks);
            }
            o << ",\"parameters\":[";
        }
        o << (first ? "" : ",") << "{\"id\":" << info.id << ",\"key\":\"";
        if (info.key.empty()) {
            o << "p" << info.id;
        } else {
            o << jsonEscape(info.key);
        }
        o << "\",\"name\":\"" << jsonEscape(info.name) << "\",\"valueKind\":" << static_cast<int>(info.kind)
          << ",\"sizeInBit\":" << info.width * 8u << ",\"offset\":" << info.offset
          // Enough digits for an exact float round trip in the ETS default value.
          << ",\"default\":" << std::setprecision(9) << info.defaultValue;
        if (info.min != info.max) {
            o << ",\"min\":" << info.min << ",\"max\":" << info.max;
        }
        o << std::setprecision(6);
        if (!info.unit.empty()) {
            o << ",\"unit\":\"" << jsonEscape(info.unit) << "\"";
        }
        if (!info.options.empty()) {
            o << ",\"optionType\":" << optionTypeOf(info);
        }
        if (info.section >= 0) {
            o << ",\"section\":" << info.section;
            if (info.section != lastSection) {
                lastSection = info.section;
                std::ostringstream section;
                section << (sections.empty() ? "" : ",") << "{\"index\":" << info.section << ",\"name\":\""
                        << jsonEscape(info.sectionName) << "\"";
                if (info.sectionVisible.op != params::ConditionOp::Always) {
                    section << ",\"visible\":";
                    detail::writeConditionJson(section, info.sectionVisible, blocks);
                }
                section << "}";
                sections += section.str();
            }
        }
        if (info.visible.op != params::ConditionOp::Always) {
            o << ",\"visible\":";
            detail::writeConditionJson(o, info.visible, blocks);
        }
        o << "}";
    }
    if (!parameters.empty()) {
        closeBlock();
    }
    o << "]";
    return o.str();
}

} // namespace knx::product
