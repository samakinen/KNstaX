// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file routing_table_control.hpp
 * @brief PID_ROUTETABLE_CONTROL — the Router Object's routing-table Function
 *        Property (KNX 03/05/01 §4.4.6).
 *
 * A coupler's group-address filter table is not configured with property
 * writes.  ETS drives it through A_FunctionPropertyCommand on PID 56 of the
 * Router Object, selecting an operation with a ServiceID octet.  This is what
 * makes a Router Object useful rather than decorative: without it, a device can
 * advertise the object but ETS has no way to tell it which group addresses to
 * pass.
 *
 * The PDU payload (after object_index and property_id) is:
 *
 *     octet 0: 00h on request / return_code on response
 *     octet 1: ServiceID
 *     octet 2..: ServiceInfo, service specific
 */

#pragma once

#include "knx/application/function_property_services.hpp"
#include "knx/network/filter_table.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <cstdint>
#include <span>

namespace knx {
namespace network {

/// ServiceIDs of PID_ROUTETABLE_CONTROL (§4.4.6.2 … §4.4.6.5).
enum class RouteTableServiceId : uint8_t {
    ClearRoutingTable = 0x01,   ///< SRVID_CLEAR_ROUTINGTABLE
    SetRoutingTable = 0x02,     ///< SRVID_SET_ROUTINGTABLE (pass every address)
    ClearGroupAddress = 0x03,   ///< SRVID_CLEAR_GROUPADDRESS (range)
    SetGroupAddress = 0x04,     ///< SRVID_SET_GROUPADDRESS (range)
};

/**
 * @brief Return codes used by PID_ROUTETABLE_CONTROL.
 *
 * §4.4.6 defines only success and a generic failure; the error value is 0xFF
 * for every service.
 */
enum class RouteTableReturnCode : uint8_t {
    Success = 0x00,
    Error = 0xFF,
};

/**
 * @brief Applies PID_ROUTETABLE_CONTROL operations to a filter table.
 *
 * Stateless apart from the table it is given, so it can be unit-tested against
 * a bare FilterTable without a coupler or a bus.
 */
class RoutingTableControl {
public:
    explicit RoutingTableControl(FilterTable& filterTable) : _filterTable(filterTable) {}

    /**
     * @brief Execute one Function Property invocation.
     *
     * @param invocation Command (may modify the table) or State_Read (must not).
     * @param payload    Request payload, starting at the leading 00h octet.
     * @return The response payload (return_code + ServiceID), or an error when
     *         the payload is too short to contain a ServiceID at all — which
     *         the caller turns into the §3.4.5.3 degenerate response.
     */
    util::Result<application::FunctionPropertyResult> invoke(
        application::FunctionPropertyInvocation invocation,
        std::span<const uint8_t> payload);

    /// Number of addresses the last successful range operation touched.
    /// Exposed for diagnostics and tests, not part of the KNX service.
    size_t lastAffectedAddressCount() const { return _lastAffectedAddressCount; }

private:
    util::Result<void> clearRoutingTable();
    util::Result<void> setRoutingTable();
    util::Result<void> applyRange(RouteTableServiceId service, std::span<const uint8_t> serviceInfo);

    FilterTable& _filterTable;
    size_t _lastAffectedAddressCount{0};
};

} // namespace network
} // namespace knx
