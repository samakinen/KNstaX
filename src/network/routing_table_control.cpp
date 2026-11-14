// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file routing_table_control.cpp
 * @brief PID_ROUTETABLE_CONTROL implementation (KNX 03/05/01 §4.5.7).
 */

#include "knx/network/routing_table_control.hpp"

#include "knx/util/log.hpp"

namespace knx {
namespace network {

namespace {

constexpr const char* TAG = "KNX.Net.RouteCtl";

/// Leading 00h octet plus the ServiceID.
constexpr size_t kServiceHeaderBytes = 2u;
/// SRVID 3 and 4 carry a start and an end address, big-endian.
constexpr size_t kRangeServiceInfoBytes = 4u;

/// "All standard Group Addresses": the whole 16-bit space the Filter Table
/// covers (03/05/01 §5.3.1.1.1), GA 0000h excepted.
constexpr uint16_t kFirstGroupAddress = 0x0001u;
constexpr uint16_t kLastGroupAddress = 0xFFFFu;

struct Range {
    uint16_t start;
    uint16_t end;
};

Range decodeRange(std::span<const uint8_t> serviceInfo)
{
    return Range{
        static_cast<uint16_t>((static_cast<uint16_t>(serviceInfo[0]) << 8) | serviceInfo[1]),
        static_cast<uint16_t>((static_cast<uint16_t>(serviceInfo[2]) << 8) | serviceInfo[3]),
    };
}

/// return_code + ServiceID, followed for SRVID 3/4 by the START/END ADDRESS
/// the request carried (§4.5.7.4, §4.5.7.5: the response repeats them).
application::FunctionPropertyResult makeResponse(RouteTableReturnCode code, uint8_t serviceId,
                                                 std::span<const uint8_t> echo = {})
{
    application::FunctionPropertyResult result{};
    result.returnCode = static_cast<application::FunctionPropertyReturnCode>(code);
    (void)result.data.push_back(serviceId);
    for (uint8_t octet : echo) {
        (void)result.data.push_back(octet);
    }
    return result;
}

RouteTableReturnCode codeFor(bool ok)
{
    return ok ? RouteTableReturnCode::Success : RouteTableReturnCode::Error;
}

}  // namespace

util::Result<application::FunctionPropertyResult> RoutingTableControl::invoke(
    application::FunctionPropertyInvocation invocation,
    std::span<const uint8_t> payload)
{
    if (payload.size() < kServiceHeaderBytes) {
        // No ServiceID at all: this is not a well-formed invocation of the
        // property, so the caller answers with the degenerate response.
        return util::ErrorCode::DecodeFailed;
    }

    const uint8_t rawServiceId = payload[1];
    const auto serviceInfo = payload.subspan(kServiceHeaderBytes);
    const bool stateRead = invocation == application::FunctionPropertyInvocation::StateRead;

    switch (static_cast<RouteTableServiceId>(rawServiceId)) {
        case RouteTableServiceId::ClearRoutingTable:
        case RouteTableServiceId::SetRoutingTable: {
            const bool pass = static_cast<RouteTableServiceId>(rawServiceId)
                              == RouteTableServiceId::SetRoutingTable;
            if (stateRead) {
                // §4.5.7.2 b) / §4.5.7.3 b): "check if all standard Group
                // Addresses are cleared / set". A State_Read never modifies:
                // treating it as a write would let a query wipe the table.
                return makeResponse(codeFor(_filterTable.rangeIs(kFirstGroupAddress,
                                                                 kLastGroupAddress, pass)),
                                    rawServiceId);
            }
            _filterTable.setAll(pass);
            _lastAffectedAddressCount = kLastGroupAddress;
            KNX_LOGI(TAG, "Routing table %s", pass ? "set (all pass)" : "cleared (none pass)");
            return makeResponse(RouteTableReturnCode::Success, rawServiceId);
        }

        case RouteTableServiceId::ClearGroupAddress:
        case RouteTableServiceId::SetGroupAddress: {
            const bool pass = static_cast<RouteTableServiceId>(rawServiceId)
                              == RouteTableServiceId::SetGroupAddress;
            if (serviceInfo.size() < kRangeServiceInfoBytes) {
                return makeResponse(RouteTableReturnCode::Error, rawServiceId, serviceInfo);
            }
            const auto echo = serviceInfo.first(kRangeServiceInfoBytes);
            const Range range = decodeRange(serviceInfo);
            if (stateRead) {
                // "If START ADDRESS is greater than END ADDRESS then no
                // standard Group Address shall be checked": nothing fails.
                return makeResponse(codeFor(_filterTable.rangeIs(range.start, range.end, pass)),
                                    rawServiceId, echo);
            }
            // "If START ADDRESS is greater than END ADDRESS no address will be
            // modified (error response!)".
            if (range.start > range.end) {
                KNX_LOGW(TAG, "Range 0x%04X..0x%04X is inverted; rejecting",
                         static_cast<unsigned>(range.start), static_cast<unsigned>(range.end));
                return makeResponse(RouteTableReturnCode::Error, rawServiceId, echo);
            }
            _filterTable.setRange(range.start, range.end, pass);
            _lastAffectedAddressCount = static_cast<size_t>(range.end - range.start) + 1u;
            KNX_LOGI(TAG, "%s group addresses 0x%04X..0x%04X",
                     pass ? "Routing" : "Blocking",
                     static_cast<unsigned>(range.start), static_cast<unsigned>(range.end));
            return makeResponse(RouteTableReturnCode::Success, rawServiceId, echo);
        }
    }

    KNX_LOGW(TAG, "Unknown PID_ROUTETABLE_CONTROL ServiceID 0x%02X",
             static_cast<unsigned>(rawServiceId));
    return makeResponse(RouteTableReturnCode::Error, rawServiceId);
}

} // namespace network
} // namespace knx
