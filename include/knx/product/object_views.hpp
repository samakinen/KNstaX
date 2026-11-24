// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen
/**
 * @file object_views.hpp
 * @brief When ETS shows a communication object, and as what (export data only)
 *
 * A product whose channels have a function chosen by a parameter uses some of
 * its objects for one function and others for another, and one object can
 * mean different things: object A of an input is a switch (1.001) for one
 * function and a shutter move (1.008) for the next. The ports stay what they
 * are at run time (one datapoint size, one value type); a view tells ETS when
 * to show a port and with which text and datapoint type:
 *
 * @code
 *   inline constexpr auto kObjectViews = std::array{
 *       views::view<Port::In1A>({.instance = 0,
 *                                .visible = params::when<In::Function>(InputFunction::Switch),
 *                                .function = "Switch", .dpt = {1, 1}}),
 *       views::view<Port::In1A>({.instance = 0,
 *                                .visible = params::when<In::Function>(InputFunction::Shutter),
 *                                .function = "Move", .dpt = {1, 8}}),
 *   };
 *   // tools: exportCommissionedProductToJson(kProduct, kObjectViews)
 *   // CMake: knx_commissioned_product(... VIEWS kObjectViews)
 * @endcode
 *
 * - A port with views is shown only through them: one ComObjectRef per view,
 *   inside a <choose> on the condition's parameter. Several views of one port
 *   are alternatives on the same object number.
 * - A port without views is exported as before, always visible.
 * - `instance` is the instance of the repeated parameter block the condition
 *   names (0 for a block that appears once).
 * - The datapoint type must be of the port's size; the exporter refuses
 *   another, and two views of one port that can be shown at once.
 *
 * Nothing here is read by the firmware: the list is passed to the export only,
 * so it costs a device no flash.
 */

#pragma once

#include "knx/product/parameter_layout.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace knx::product::views {

/// A datapoint type as ETS names it. main 0 means "the port's own".
struct DatapointType {
    uint16_t main{0};
    uint16_t sub{0};
};

struct ViewSpec {
    /// Instance of the repeated block that holds the condition's parameter.
    uint16_t instance{0};
    /// When ETS shows the object; Always for a view without a condition.
    params::Condition visible{};
    /// ComObjectRef/@Text. Empty: the port's display name.
    std::string_view text{};
    /// ComObjectRef/@FunctionText. Empty: the text.
    std::string_view function{};
    /// ComObjectRef/@DatapointType. main 0: the port's.
    DatapointType dpt{};
};

struct ObjectView {
    /// The port's logical id.
    uint16_t port{0};
    ViewSpec spec{};
};

template <auto Port>
constexpr ObjectView view(ViewSpec spec)
{
    static_assert(std::is_enum_v<decltype(Port)> || std::is_integral_v<decltype(Port)>,
                  "a view names a port by its logical id");
    return ObjectView{static_cast<uint16_t>(Port), spec};
}

} // namespace knx::product::views
