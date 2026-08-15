// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file routing_table_control.cpp
 * @brief PID_ROUTETABLE_CONTROL implementation (KNX 03/05/01 §4.4.6).
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

/// Standard group addresses are the range the routing table covers; addresses
/// above 0x6FFF are "extended" and handled by PID_MAIN_LCGRPCONFIG's
/// GROUP_7000 field rather than by the routing table (§4.4.5).
constexpr uint16_t kMaxStandardGroupAddress = 0x6FFF;

/// A single-address filter entry: mask of all ones matches exactly one address.
constexpr GroupAddress exactMask() { return GroupAddress(0xFFFFu); }

application::FunctionPropertyResult makeResponse(RouteTableReturnCode code, uint8_t serviceId)
{
    application::FunctionPropertyResult result{};
    result.returnCode = static_cast<application::FunctionPropertyReturnCode>(code);
    (void)result.data.push_back(serviceId);
    return result;
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

    // §4.4.6: a State_Read reports whether the service *would* work without
    // performing it.  Treating a read as a write would let a management client
    // wipe a coupler's filter table with what it believes is a query.
    if (invocation == application::FunctionPropertyInvocation::StateRead) {
        switch (static_cast<RouteTableServiceId>(rawServiceId)) {
            case RouteTableServiceId::ClearRoutingTable:
            case RouteTableServiceId::SetRoutingTable:
            case RouteTableServiceId::ClearGroupAddress:
            case RouteTableServiceId::SetGroupAddress:
                return makeResponse(RouteTableReturnCode::Success, rawServiceId);
        }
        // §4.4.6.6: an unknown ServiceID is echoed back with an error code.
        return makeResponse(RouteTableReturnCode::Error, rawServiceId);
    }

    util::Result<void> outcome = util::ErrorCode::OperationNotSupported;
    switch (static_cast<RouteTableServiceId>(rawServiceId)) {
        case RouteTableServiceId::ClearRoutingTable:
            outcome = clearRoutingTable();
            break;
        case RouteTableServiceId::SetRoutingTable:
            outcome = setRoutingTable();
            break;
        case RouteTableServiceId::ClearGroupAddress:
        case RouteTableServiceId::SetGroupAddress:
            outcome = applyRange(static_cast<RouteTableServiceId>(rawServiceId), serviceInfo);
            break;
        default:
            KNX_LOGW(TAG, "Unknown PID_ROUTETABLE_CONTROL ServiceID 0x%02X",
                     static_cast<unsigned>(rawServiceId));
            return makeResponse(RouteTableReturnCode::Error, rawServiceId);
    }

    return makeResponse(outcome.isOk() ? RouteTableReturnCode::Success : RouteTableReturnCode::Error,
                        rawServiceId);
}

util::Result<void> RoutingTableControl::clearRoutingTable()
{
    // "Clear" means no standard group address is routed.  The table is emptied
    // and the default action set to Block, so the absence of an entry denies
    // rather than permits — an empty table that defaulted to Forward would
    // route everything, the opposite of what the client asked for.
    _filterTable.clear();
    _filterTable.setDefaultAction(FilterAction::Block);
    _lastAffectedAddressCount = 0;
    KNX_LOGI(TAG, "Routing table cleared (default action: block)");
    return util::Result<void>::ok();
}

util::Result<void> RoutingTableControl::setRoutingTable()
{
    // The inverse: every standard group address is routed.  Expressed as a
    // default action rather than 28k individual entries.
    _filterTable.clear();
    _filterTable.setDefaultAction(FilterAction::Allow);
    _lastAffectedAddressCount = 0;
    KNX_LOGI(TAG, "Routing table set to pass all standard group addresses");
    return util::Result<void>::ok();
}

util::Result<void> RoutingTableControl::applyRange(RouteTableServiceId service,
                                                   std::span<const uint8_t> serviceInfo)
{
    if (serviceInfo.size() < kRangeServiceInfoBytes) {
        return util::ErrorCode::DecodeFailed;
    }

    const uint16_t start = static_cast<uint16_t>((static_cast<uint16_t>(serviceInfo[0]) << 8)
                                                 | serviceInfo[1]);
    const uint16_t end = static_cast<uint16_t>((static_cast<uint16_t>(serviceInfo[2]) << 8)
                                               | serviceInfo[3]);

    // §4.4.6.4: "If START ADDRESS is greater than END ADDRESS no address will
    // be modified (error response!)".  An inverted range is a client bug, and
    // silently treating it as empty would hide it.
    if (start > end) {
        KNX_LOGW(TAG, "Range 0x%04X..0x%04X is inverted; rejecting",
                 static_cast<unsigned>(start), static_cast<unsigned>(end));
        return util::ErrorCode::InvalidParameter;
    }

    if (start > kMaxStandardGroupAddress) {
        KNX_LOGW(TAG, "Range starts above the standard group address space (0x%04X)",
                 static_cast<unsigned>(start));
        return util::ErrorCode::OutOfRange;
    }

    const uint16_t clampedEnd = end > kMaxStandardGroupAddress ? kMaxStandardGroupAddress : end;
    const auto action = (service == RouteTableServiceId::SetGroupAddress) ? FilterAction::Allow
                                                                         : FilterAction::Block;

    // The filter table is a bounded resource (MAX_ENTRIES).  Refuse a range
    // that cannot be represented rather than applying a partial one: a
    // half-applied routing table silently drops traffic the integrator
    // believes is configured to pass.
    const size_t requested = static_cast<size_t>(clampedEnd - start) + 1u;
    const size_t free = FilterTable::MAX_ENTRIES - _filterTable.entryCount();
    if (requested > free) {
        KNX_LOGW(TAG,
                 "Range 0x%04X..0x%04X needs %zu entries, %zu free; rejecting",
                 static_cast<unsigned>(start), static_cast<unsigned>(clampedEnd), requested, free);
        return util::ErrorCode::QueueFull;
    }

    for (uint32_t address = start; address <= clampedEnd; ++address) {
        const GroupAddress groupAddress(static_cast<uint16_t>(address));
        const auto res = _filterTable.addEntry(groupAddress, exactMask(), action, EntryState::Enabled);
        if (res.isError()) {
            return res;
        }
    }

    _lastAffectedAddressCount = requested;
    KNX_LOGI(TAG, "%s group addresses 0x%04X..0x%04X (%zu entries)",
             action == FilterAction::Allow ? "Routing" : "Blocking",
             static_cast<unsigned>(start), static_cast<unsigned>(clampedEnd), requested);
    return util::Result<void>::ok();
}

} // namespace network
} // namespace knx
