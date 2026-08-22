// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_netip_secured_service_families.cpp
 * @brief PID_SECURED_SERVICE_FAMILIES decoding (03/08/09 §2.3.1.5, Table 4)
 *
 * Every case below is a Return Code the standard names. Getting one wrong
 * tells a management client something untrue about how the installation is
 * secured, which is the failure that does not announce itself.
 */

#include "unity.h"

#include "knx/netip/secured_service_families.hpp"

#include <vector>

using namespace knx;
using RC = knx::application::FunctionPropertyReturnCode;

namespace {

netip::SecuredServiceFamiliesRequest decodeCommand(std::vector<uint8_t> data)
{
    return netip::decodeSecuredServiceFamiliesRequest(data, true);
}

netip::SecuredServiceFamiliesRequest decodeRead(std::vector<uint8_t> data)
{
    return netip::decodeSecuredServiceFamiliesRequest(data, false);
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// The three commands ETS sends at the start of a secure Download All, verbatim
// from a capture: Routing, Device Management and Tunnelling, each set to
// version 00h — plain allowed. ETS turns IP Secure *off* here; reading these as
// "enable security" is the misreading worth guarding against.
void test_decodes_the_three_commands_ets_sends(void) {
    struct Case { uint8_t family; const char* name; };
    const Case cases[] = {
        {netip::kServiceFamilyRouting, "Routing"},
        {netip::kServiceFamilyDeviceManagement, "Device Management"},
        {netip::kServiceFamilyTunnelling, "Tunnelling"},
    };

    for (const auto& c : cases) {
        const auto decoded = decodeCommand({0x00, 0x00, c.family, 0x00});
        TEST_ASSERT_TRUE(decoded.ok());
        TEST_ASSERT_TRUE(decoded.hasServiceId);
        TEST_ASSERT_EQUAL_HEX8(0x00, decoded.serviceId);
        TEST_ASSERT_EQUAL_HEX8(c.family, decoded.serviceFamilyId);
        TEST_ASSERT_EQUAL_HEX8(netip::kSecurityVersionPlain, decoded.securityVersion);
    }
}

void test_decodes_a_request_to_require_security(void) {
    const auto decoded = decodeCommand({0x00, 0x00, netip::kServiceFamilyRouting, 0x01});
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_HEX8(netip::kSecurityVersionRequired, decoded.securityVersion);
}

// A state read carries no version — three octets, not four.
void test_decodes_a_state_read(void) {
    const auto decoded = decodeRead({0x00, 0x00, netip::kServiceFamilyTunnelling});
    TEST_ASSERT_TRUE(decoded.ok());
    TEST_ASSERT_EQUAL_HEX8(netip::kServiceFamilyTunnelling, decoded.serviceFamilyId);
}

// Table 4: "This Return Code shall be used if octet 10 in the request does not
// equal 00h."
void test_reserved_octet_must_be_zero(void) {
    const auto decoded = decodeCommand({0x01, 0x00, netip::kServiceFamilyRouting, 0x00});
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(RC::DataVoid),
                           static_cast<uint8_t>(decoded.returnCode));
    // The ServiceID is still echoed, so a client can tell which command failed.
    TEST_ASSERT_TRUE(decoded.hasServiceId);
    TEST_ASSERT_EQUAL_HEX8(0x00, decoded.serviceId);
}

// Table 4: an undefined Read-/WriteServiceID is E_COMMAND_INVALID, not
// E_DATA_VOID — the distinction is the whole reason both codes exist.
void test_unknown_service_id_is_command_invalid(void) {
    const auto decoded = decodeCommand({0x00, 0x07, netip::kServiceFamilyRouting, 0x00});
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(RC::CommandInvalid),
                           static_cast<uint8_t>(decoded.returnCode));
    TEST_ASSERT_EQUAL_HEX8(0x07, decoded.serviceId);
}

// Table 4: "not supported/allowed Service Family ID". Core (02h) is the case
// that matters — §2.3.1.5.1 says discovery is intentionally never secured.
void test_unsecurable_service_family_is_data_void(void) {
    for (const uint8_t family : {0x00u, 0x01u, 0x02u, 0x06u, 0x08u, 0xFFu}) {
        const auto decoded = decodeCommand({0x00, 0x00, static_cast<uint8_t>(family), 0x00});
        TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(RC::DataVoid),
                               static_cast<uint8_t>(decoded.returnCode));
    }
}

// "> 01h: Reserved. For future use." A device that accepted one would be
// claiming a security version it cannot possibly implement.
void test_reserved_security_version_is_data_void(void) {
    const auto decoded = decodeCommand({0x00, 0x00, netip::kServiceFamilyRouting, 0x02});
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(RC::DataVoid),
                           static_cast<uint8_t>(decoded.returnCode));
}

// Too short to carry a ServiceID: nothing to echo, so the catch-all code.
void test_truncated_request_is_error_with_nothing_to_echo(void) {
    for (const auto& payload : {std::vector<uint8_t>{}, std::vector<uint8_t>{0x00}}) {
        const auto decoded = netip::decodeSecuredServiceFamiliesRequest(payload, true);
        TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(RC::Error),
                               static_cast<uint8_t>(decoded.returnCode));
        TEST_ASSERT_FALSE(decoded.hasServiceId);
    }
}

// Long enough for a ServiceID but not for what it asks for.
void test_missing_fields_are_data_void(void) {
    // Command without the Security Version.
    const auto command = decodeCommand({0x00, 0x00, netip::kServiceFamilyRouting});
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(RC::DataVoid),
                           static_cast<uint8_t>(command.returnCode));
    // Read without the Service Family ID.
    const auto read = decodeRead({0x00, 0x00});
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(RC::DataVoid),
                           static_cast<uint8_t>(read.returnCode));
}

// The index is what lets a device keep one setting per family; a wrong mapping
// would silently apply Routing's requirement to Tunnelling.
void test_family_index_is_stable_and_distinct(void) {
    TEST_ASSERT_EQUAL_UINT32(0, netip::securableServiceFamilyIndex(netip::kServiceFamilyDeviceManagement));
    TEST_ASSERT_EQUAL_UINT32(1, netip::securableServiceFamilyIndex(netip::kServiceFamilyTunnelling));
    TEST_ASSERT_EQUAL_UINT32(2, netip::securableServiceFamilyIndex(netip::kServiceFamilyRouting));
    TEST_ASSERT_TRUE(netip::isSecurableServiceFamily(netip::kServiceFamilyRouting));
    TEST_ASSERT_FALSE(netip::isSecurableServiceFamily(0x02));  // Core: never secured
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_decodes_the_three_commands_ets_sends);
    RUN_TEST(test_decodes_a_request_to_require_security);
    RUN_TEST(test_decodes_a_state_read);
    RUN_TEST(test_reserved_octet_must_be_zero);
    RUN_TEST(test_unknown_service_id_is_command_invalid);
    RUN_TEST(test_unsecurable_service_family_is_data_void);
    RUN_TEST(test_reserved_security_version_is_data_void);
    RUN_TEST(test_truncated_request_is_error_with_nothing_to_echo);
    RUN_TEST(test_missing_fields_are_data_void);
    RUN_TEST(test_family_index_is_stable_and_distinct);
    return UNITY_END();
}
