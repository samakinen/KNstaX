// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_routing_table_control.cpp
 * @brief Unit tests for PID_ROUTETABLE_CONTROL (KNX 03/05/01 §4.4.6).
 *
 * This is the service ETS uses to download a coupler's group-address filter
 * table. The behaviours pinned here are the ones where getting it wrong causes
 * a coupler to silently pass or drop traffic the integrator believes is
 * configured the other way.
 */

#include "unity.h"
#include "knx/network/routing_table_control.hpp"

#include <vector>

using namespace knx;
using namespace knx::network;
using knx::application::FunctionPropertyInvocation;

namespace {

/// Build a PID_ROUTETABLE_CONTROL request payload: leading 00h, ServiceID,
/// then service-specific info.
std::vector<uint8_t> request(RouteTableServiceId service,
                             std::initializer_list<uint8_t> serviceInfo = {}) {
    std::vector<uint8_t> payload{0x00, static_cast<uint8_t>(service)};
    payload.insert(payload.end(), serviceInfo);
    return payload;
}

std::vector<uint8_t> rangeRequest(RouteTableServiceId service, uint16_t start, uint16_t end) {
    return request(service, {static_cast<uint8_t>(start >> 8), static_cast<uint8_t>(start & 0xFF),
                             static_cast<uint8_t>(end >> 8), static_cast<uint8_t>(end & 0xFF)});
}

bool succeeded(const util::Result<application::FunctionPropertyResult>& result) {
    return result.isOk()
        && result.value().returnCode
               == static_cast<application::FunctionPropertyReturnCode>(RouteTableReturnCode::Success);
}

bool failed(const util::Result<application::FunctionPropertyResult>& result) {
    return result.isOk()
        && result.value().returnCode
               == static_cast<application::FunctionPropertyReturnCode>(RouteTableReturnCode::Error);
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- Response shape --------------------------------------------------------

void test_response_echoes_service_id(void) {
    // §4.4.6.2: the response carries return_code followed by the ServiceID, so
    // a client can correlate it with the request it sent.
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    request(RouteTableServiceId::ClearRoutingTable));
    TEST_ASSERT_TRUE(succeeded(res));
    TEST_ASSERT_EQUAL(1u, res.value().data.size());
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(RouteTableServiceId::ClearRoutingTable),
                      res.value().data[0]);
}

void test_payload_without_service_id_is_malformed(void) {
    // Too short to contain a ServiceID: the caller must produce the §3.4.5.3
    // degenerate response rather than guessing an operation.
    FilterTable table;
    RoutingTableControl control(table);

    const std::vector<uint8_t> tooShort{0x00};
    TEST_ASSERT_TRUE(control.invoke(FunctionPropertyInvocation::Command, tooShort).isError());
}

void test_unknown_service_id_returns_error_not_silence(void) {
    // §4.4.6.6: an unknown ServiceID is echoed back with an error return code.
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    request(static_cast<RouteTableServiceId>(0x7F)));
    TEST_ASSERT_TRUE(failed(res));
    TEST_ASSERT_EQUAL(0x7F, res.value().data[0]);
}

// --- SRVID 1 / 2: whole-table operations -----------------------------------

void test_clear_routing_table_blocks_by_default(void) {
    // Clearing must leave the coupler blocking. An emptied table that still
    // defaulted to Allow would route everything — the exact opposite of what
    // the management client asked for.
    FilterTable table;
    table.setDefaultAction(FilterAction::Allow);
    (void)table.addEntry(GroupAddress(1, 1, 1), GroupAddress(0xFFFF), FilterAction::Allow,
                         EntryState::Enabled);
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(succeeded(control.invoke(FunctionPropertyInvocation::Command,
                                              request(RouteTableServiceId::ClearRoutingTable))));
    TEST_ASSERT_EQUAL(0u, table.entryCount());
    TEST_ASSERT_EQUAL(static_cast<int>(FilterAction::Block),
                      static_cast<int>(table.defaultAction()));
}

void test_set_routing_table_allows_by_default(void) {
    FilterTable table;
    table.setDefaultAction(FilterAction::Block);
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(succeeded(control.invoke(FunctionPropertyInvocation::Command,
                                              request(RouteTableServiceId::SetRoutingTable))));
    TEST_ASSERT_EQUAL(static_cast<int>(FilterAction::Allow),
                      static_cast<int>(table.defaultAction()));
    // Expressed as a default rather than ~28k individual entries.
    TEST_ASSERT_EQUAL(0u, table.entryCount());
}

// --- SRVID 3 / 4: address ranges -------------------------------------------

void test_set_group_address_range_adds_allow_entries(void) {
    FilterTable table;
    table.setDefaultAction(FilterAction::Block);
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0100, 0x0104));
    TEST_ASSERT_TRUE(succeeded(res));
    TEST_ASSERT_EQUAL(5u, table.entryCount());
    TEST_ASSERT_EQUAL(5u, control.lastAffectedAddressCount());

    const auto* first = table.getEntry(0);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL(0x0100, first->address.raw);
    TEST_ASSERT_EQUAL(static_cast<int>(FilterAction::Allow), static_cast<int>(first->action));
}

void test_clear_group_address_range_adds_block_entries(void) {
    FilterTable table;
    table.setDefaultAction(FilterAction::Allow);
    RoutingTableControl control(table);

    const auto res = control.invoke(
        FunctionPropertyInvocation::Command,
        rangeRequest(RouteTableServiceId::ClearGroupAddress, 0x0200, 0x0201));
    TEST_ASSERT_TRUE(succeeded(res));
    TEST_ASSERT_EQUAL(2u, table.entryCount());

    const auto* first = table.getEntry(0);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL(static_cast<int>(FilterAction::Block), static_cast<int>(first->action));
}

void test_single_address_range_is_start_equals_end(void) {
    // §4.4.6.4: "If only one standard Group Address is to be modified then
    // START ADDRESS and END ADDRESS shall be equal."
    FilterTable table;
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(succeeded(control.invoke(
        FunctionPropertyInvocation::Command,
        rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0801, 0x0801))));
    TEST_ASSERT_EQUAL(1u, table.entryCount());
}

void test_inverted_range_is_rejected(void) {
    // §4.4.6.4: "If START ADDRESS is greater than END ADDRESS no address will
    // be modified (error response!)".  Treating it as an empty range would
    // hide a client bug behind a success code.
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0500, 0x0100));
    TEST_ASSERT_TRUE(failed(res));
    TEST_ASSERT_EQUAL(0u, table.entryCount());
}

void test_range_above_standard_group_addresses_is_rejected(void) {
    // The routing table covers standard group addresses only (<= 0x6FFF).
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    rangeRequest(RouteTableServiceId::SetGroupAddress, 0x7000, 0x7001));
    TEST_ASSERT_TRUE(failed(res));
    TEST_ASSERT_EQUAL(0u, table.entryCount());
}

void test_truncated_range_payload_is_rejected(void) {
    FilterTable table;
    RoutingTableControl control(table);

    // Two octets where four are required.
    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    request(RouteTableServiceId::SetGroupAddress, {0x01, 0x00}));
    TEST_ASSERT_TRUE(failed(res));
    TEST_ASSERT_EQUAL(0u, table.entryCount());
}

void test_oversized_range_is_rejected_not_partially_applied(void) {
    // The filter table is bounded. A partially applied range would silently
    // drop traffic the integrator believes is configured to pass, so the whole
    // operation must fail and leave the table untouched.
    FilterTable table;
    RoutingTableControl control(table);

    const uint16_t start = 0x0100;
    const uint16_t end = static_cast<uint16_t>(start + FilterTable::MAX_ENTRIES);  // one too many
    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    rangeRequest(RouteTableServiceId::SetGroupAddress, start, end));
    TEST_ASSERT_TRUE(failed(res));
    TEST_ASSERT_EQUAL(0u, table.entryCount());
}

// --- State_Read must not mutate --------------------------------------------

void test_state_read_does_not_modify_the_table(void) {
    // A management client issuing State_Read is asking a question. If that
    // wiped the filter table, a diagnostic read would take down a coupler.
    FilterTable table;
    table.setDefaultAction(FilterAction::Allow);
    (void)table.addEntry(GroupAddress(1, 1, 1), GroupAddress(0xFFFF), FilterAction::Allow,
                         EntryState::Enabled);
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(succeeded(control.invoke(FunctionPropertyInvocation::StateRead,
                                              request(RouteTableServiceId::ClearRoutingTable))));
    TEST_ASSERT_EQUAL(1u, table.entryCount());
    TEST_ASSERT_EQUAL(static_cast<int>(FilterAction::Allow),
                      static_cast<int>(table.defaultAction()));

    TEST_ASSERT_TRUE(succeeded(control.invoke(
        FunctionPropertyInvocation::StateRead,
        rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0100, 0x0110))));
    TEST_ASSERT_EQUAL(1u, table.entryCount());
}

void test_state_read_reports_unknown_service_as_error(void) {
    FilterTable table;
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(failed(control.invoke(FunctionPropertyInvocation::StateRead,
                                           request(static_cast<RouteTableServiceId>(0x42)))));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_response_echoes_service_id);
    RUN_TEST(test_payload_without_service_id_is_malformed);
    RUN_TEST(test_unknown_service_id_returns_error_not_silence);
    RUN_TEST(test_clear_routing_table_blocks_by_default);
    RUN_TEST(test_set_routing_table_allows_by_default);
    RUN_TEST(test_set_group_address_range_adds_allow_entries);
    RUN_TEST(test_clear_group_address_range_adds_block_entries);
    RUN_TEST(test_single_address_range_is_start_equals_end);
    RUN_TEST(test_inverted_range_is_rejected);
    RUN_TEST(test_range_above_standard_group_addresses_is_rejected);
    RUN_TEST(test_truncated_range_payload_is_rejected);
    RUN_TEST(test_oversized_range_is_rejected_not_partially_applied);
    RUN_TEST(test_state_read_does_not_modify_the_table);
    RUN_TEST(test_state_read_reports_unknown_service_as_error);
    return UNITY_END();
}
