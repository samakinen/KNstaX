// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_routing_table_control.cpp
 * @brief Unit tests for PID_ROUTETABLE_CONTROL (KNX 03/05/01 §4.5.7).
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
    // §4.5.7.2: the response carries return_code followed by the ServiceID.
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    request(RouteTableServiceId::ClearRoutingTable));
    TEST_ASSERT_TRUE(succeeded(res));
    TEST_ASSERT_EQUAL(1u, res.value().data.size());
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(RouteTableServiceId::ClearRoutingTable),
                      res.value().data[0]);
}

void test_range_response_echoes_start_and_end(void) {
    // §4.5.7.4/§4.5.7.5: the response is return_code, ServiceID, START ADDRESS,
    // END ADDRESS.
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0801, 0x08FF));
    TEST_ASSERT_TRUE(succeeded(res));
    TEST_ASSERT_EQUAL(5u, res.value().data.size());
    TEST_ASSERT_EQUAL_HEX8(0x04, res.value().data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x08, res.value().data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, res.value().data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x08, res.value().data[3]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, res.value().data[4]);
}

void test_payload_without_service_id_is_malformed(void) {
    FilterTable table;
    RoutingTableControl control(table);

    const std::vector<uint8_t> tooShort{0x00};
    TEST_ASSERT_TRUE(control.invoke(FunctionPropertyInvocation::Command, tooShort).isError());
}

void test_unknown_service_id_returns_error_not_silence(void) {
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    request(static_cast<RouteTableServiceId>(0x7F)));
    TEST_ASSERT_TRUE(failed(res));
    TEST_ASSERT_EQUAL(0x7F, res.value().data[0]);
}

// --- SRVID 1 / 2: whole-table operations -----------------------------------

void test_clear_filter_table_clears_every_address(void) {
    FilterTable table;
    table.setAll(true);
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(succeeded(control.invoke(FunctionPropertyInvocation::Command,
                                              request(RouteTableServiceId::ClearRoutingTable))));
    TEST_ASSERT_EQUAL_UINT32(0u, table.passCount());
}

void test_set_filter_table_sets_every_address(void) {
    FilterTable table;
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(succeeded(control.invoke(FunctionPropertyInvocation::Command,
                                              request(RouteTableServiceId::SetRoutingTable))));
    TEST_ASSERT_EQUAL_UINT32(65535u, table.passCount());
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0x0001)));
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0xFFFF)));
}

// --- SRVID 3 / 4: address ranges -------------------------------------------

void test_set_group_address_range(void) {
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0100, 0x0104));
    TEST_ASSERT_TRUE(succeeded(res));
    TEST_ASSERT_EQUAL_UINT32(5u, table.passCount());
    TEST_ASSERT_EQUAL(5u, control.lastAffectedAddressCount());
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0x0100)));
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0x0104)));
    TEST_ASSERT_FALSE(table.passes(GroupAddress(0x0105)));
}

void test_clear_group_address_range(void) {
    FilterTable table;
    table.setAll(true);
    RoutingTableControl control(table);

    const auto res = control.invoke(
        FunctionPropertyInvocation::Command,
        rangeRequest(RouteTableServiceId::ClearGroupAddress, 0x0200, 0x0201));
    TEST_ASSERT_TRUE(succeeded(res));
    TEST_ASSERT_EQUAL_UINT32(65533u, table.passCount());
    TEST_ASSERT_FALSE(table.passes(GroupAddress(0x0200)));
    TEST_ASSERT_FALSE(table.passes(GroupAddress(0x0201)));
}

void test_whole_main_groups_like_ets_writes(void) {
    // A real line-coupler download is a handful of large ranges; the old
    // 64-entry list refused anything over 64 addresses.
    FilterTable table;
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(succeeded(control.invoke(FunctionPropertyInvocation::Command,
                                              request(RouteTableServiceId::ClearRoutingTable))));
    TEST_ASSERT_TRUE(succeeded(control.invoke(
        FunctionPropertyInvocation::Command,
        rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0000, 0x37FF))));  // main groups 0..6
    TEST_ASSERT_TRUE(succeeded(control.invoke(
        FunctionPropertyInvocation::Command,
        rangeRequest(RouteTableServiceId::ClearGroupAddress, 0x1000, 0x17FF))));  // except 2
    TEST_ASSERT_TRUE(succeeded(control.invoke(
        FunctionPropertyInvocation::Command,
        rangeRequest(RouteTableServiceId::SetGroupAddress, 0x1005, 0x1005))));  // but 2/0/5
    TEST_ASSERT_EQUAL_UINT32(0x37FFu - 0x0800u + 1u, table.passCount());
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0x0FFF)));
    TEST_ASSERT_FALSE(table.passes(GroupAddress(0x1004)));
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0x1005)));
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0x37FF)));
    TEST_ASSERT_FALSE(table.passes(GroupAddress(0x3800)));
}

void test_range_above_6fff_is_part_of_the_table(void) {
    // The Filter Table covers the full 16-bit space (§5.3.1.1.1), so
    // GROUP_ROUT7000 has entries to consult.
    FilterTable table;
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(succeeded(control.invoke(
        FunctionPropertyInvocation::Command,
        rangeRequest(RouteTableServiceId::SetGroupAddress, 0x7000, 0xFFFF))));
    TEST_ASSERT_EQUAL_UINT32(0x9000u, table.passCount());
    TEST_ASSERT_TRUE(table.passes(GroupAddress(0xFFFF)));
    TEST_ASSERT_FALSE(table.passes(GroupAddress(0x6FFF)));
}

void test_single_address_range_is_start_equals_end(void) {
    FilterTable table;
    RoutingTableControl control(table);

    TEST_ASSERT_TRUE(succeeded(control.invoke(
        FunctionPropertyInvocation::Command,
        rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0801, 0x0801))));
    TEST_ASSERT_EQUAL_UINT32(1u, table.passCount());
}

void test_inverted_range_is_rejected(void) {
    // "If START ADDRESS is greater than END ADDRESS no address will be
    // modified (error response!)".
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0500, 0x0100));
    TEST_ASSERT_TRUE(failed(res));
    TEST_ASSERT_EQUAL_UINT32(0u, table.passCount());
}

void test_truncated_range_payload_is_rejected(void) {
    FilterTable table;
    RoutingTableControl control(table);

    const auto res = control.invoke(FunctionPropertyInvocation::Command,
                                    request(RouteTableServiceId::SetGroupAddress, {0x01, 0x00}));
    TEST_ASSERT_TRUE(failed(res));
    TEST_ASSERT_EQUAL_UINT32(0u, table.passCount());
}

// --- State_Read: a real check, never a modification -------------------------

void test_state_read_does_not_modify_the_table(void) {
    FilterTable table;
    table.set(GroupAddress(1, 1, 1), true);
    RoutingTableControl control(table);

    // Not all cleared: error code, and the address is still there.
    TEST_ASSERT_TRUE(failed(control.invoke(FunctionPropertyInvocation::StateRead,
                                           request(RouteTableServiceId::ClearRoutingTable))));
    TEST_ASSERT_EQUAL_UINT32(1u, table.passCount());
    TEST_ASSERT_TRUE(failed(control.invoke(FunctionPropertyInvocation::StateRead,
                                           rangeRequest(RouteTableServiceId::SetGroupAddress, 0x0100, 0x0110))));
    TEST_ASSERT_EQUAL_UINT32(1u, table.passCount());
}

void test_state_read_reports_whether_the_range_has_the_state(void) {
    // §4.5.7.2 b) … §4.5.7.5 b): 00h if every checked address is cleared/set,
    // FFh otherwise.
    FilterTable table;
    RoutingTableControl control(table);
    const auto read = [&](RouteTableServiceId id) {
        return control.invoke(FunctionPropertyInvocation::StateRead, request(id));
    };
    const auto readRange = [&](RouteTableServiceId id, uint16_t s, uint16_t e) {
        return control.invoke(FunctionPropertyInvocation::StateRead, rangeRequest(id, s, e));
    };

    TEST_ASSERT_TRUE(succeeded(read(RouteTableServiceId::ClearRoutingTable)));
    TEST_ASSERT_TRUE(failed(read(RouteTableServiceId::SetRoutingTable)));

    table.setRange(0x0800, 0x0FFF, true);
    TEST_ASSERT_TRUE(succeeded(readRange(RouteTableServiceId::SetGroupAddress, 0x0800, 0x0FFF)));
    TEST_ASSERT_TRUE(failed(readRange(RouteTableServiceId::SetGroupAddress, 0x07FF, 0x0FFF)));
    TEST_ASSERT_TRUE(succeeded(readRange(RouteTableServiceId::ClearGroupAddress, 0x1000, 0x6FFF)));
    TEST_ASSERT_TRUE(failed(readRange(RouteTableServiceId::ClearGroupAddress, 0x0FFF, 0x1000)));
    // "If START ADDRESS is greater than END ADDRESS then no standard Group
    // Address shall be checked."
    TEST_ASSERT_TRUE(succeeded(readRange(RouteTableServiceId::SetGroupAddress, 0x2000, 0x1000)));

    table.setAll(true);
    TEST_ASSERT_TRUE(succeeded(read(RouteTableServiceId::SetRoutingTable)));
    const auto echoed = readRange(RouteTableServiceId::SetGroupAddress, 0x0001, 0xFFFF);
    TEST_ASSERT_TRUE(succeeded(echoed));
    TEST_ASSERT_EQUAL(5u, echoed.value().data.size());
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
    RUN_TEST(test_range_response_echoes_start_and_end);
    RUN_TEST(test_payload_without_service_id_is_malformed);
    RUN_TEST(test_unknown_service_id_returns_error_not_silence);
    RUN_TEST(test_clear_filter_table_clears_every_address);
    RUN_TEST(test_set_filter_table_sets_every_address);
    RUN_TEST(test_set_group_address_range);
    RUN_TEST(test_clear_group_address_range);
    RUN_TEST(test_whole_main_groups_like_ets_writes);
    RUN_TEST(test_range_above_6fff_is_part_of_the_table);
    RUN_TEST(test_single_address_range_is_start_equals_end);
    RUN_TEST(test_inverted_range_is_rejected);
    RUN_TEST(test_truncated_range_payload_is_rejected);
    RUN_TEST(test_state_read_does_not_modify_the_table);
    RUN_TEST(test_state_read_reports_whether_the_range_has_the_state);
    RUN_TEST(test_state_read_reports_unknown_service_as_error);
    return UNITY_END();
}
