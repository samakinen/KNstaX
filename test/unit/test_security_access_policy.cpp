// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/application/security_access_policy.hpp"

// ============================================================================
// KNX Access Policies (03/4/1 §6.2) for the services and Resources that can
// undo a device's security.
//
// The rule these tests exist for is 03/05/01 §6.3.5: PID_SECURITY_MODE "shall
// only be writeable using Secure Communication, regardless of its value. […]
// Even if Security Mode is disabled, it shall only be possible to enable it by
// using secure communication."  Everything else here follows the same reading
// applied to the rest of the Security Interface Object (00C/00C) and to the
// management services whose write policy is 00C or 0CC.
// ============================================================================

using namespace knx;
using namespace knx::application;

namespace {

/// PIDs of the Security Interface Object (03/05/01 Table 99).
constexpr uint16_t kPidObjectType = 1;
constexpr uint16_t kPidLoadStateControl = 5;
constexpr uint16_t kPidSecurityMode = 51;
constexpr uint16_t kPidP2PKeyTable = 52;
constexpr uint16_t kPidGroupKeyTable = 53;
constexpr uint16_t kPidSecurityIndividualAddressTable = 54;
constexpr uint16_t kPidToolKey = 56;
constexpr uint16_t kPidSequenceNumberSending = 59;
constexpr uint16_t kPidZoneKeyTable = 60;
constexpr uint16_t kPidGoSecurityFlags = 61;

constexpr RequestSecurity kPlain{};
constexpr RequestSecurity kSecuredNoTool{true, false, true};
constexpr RequestSecurity kToolAuthOnly{true, true, false};
constexpr RequestSecurity kToolSecured{true, true, true};

constexpr bool kWrite = true;
constexpr bool kRead = false;

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- PID_SECURITY_MODE, the reported bug ------------------------------------

void test_security_mode_write_needs_secure_communication(void) {
    TEST_ASSERT_FALSE(securityObjectAccessPermitted(kPidSecurityMode, kWrite, kPlain));
    TEST_ASSERT_TRUE(securityObjectAccessPermitted(kPidSecurityMode, kWrite, kToolSecured));
}

// The Role matters, not merely the fact that something was encrypted: a device
// that has a P2P link with some other device must not let that device rewrite
// the security mode.
void test_security_mode_write_needs_the_tool_role(void) {
    TEST_ASSERT_FALSE(securityObjectAccessPermitted(kPidSecurityMode, kWrite, kSecuredNoTool));
}

// The "A" column of the policy is not the "A+C" column: authentication without
// confidentiality does not satisfy 15F/04C.
void test_security_mode_write_needs_confidentiality(void) {
    TEST_ASSERT_FALSE(securityObjectAccessPermitted(kPidSecurityMode, kWrite, kToolAuthOnly));
}

// ETS reads the mode before it has any secure link to read it over — that is
// how it finds out whether the device is already commissioned securely.
void test_security_mode_read_stays_open(void) {
    TEST_ASSERT_TRUE(securityObjectAccessPermitted(kPidSecurityMode, kRead, kPlain));
}

// --- Key material -----------------------------------------------------------

// 03/05/01 §1.3: "Devices must protect critical security information like keys,
// permission tables etc. from any access." A plain A_PropertyValue_Read of
// PID_TOOL_KEY handed the caller the device's tool credentials outright.
void test_key_material_is_not_readable_in_plain(void) {
    const uint16_t secrets[] = {
        kPidToolKey,
        kPidP2PKeyTable,
        kPidGroupKeyTable,
        kPidZoneKeyTable,
        kPidSecurityIndividualAddressTable,
        kPidSequenceNumberSending,
        kPidGoSecurityFlags,
    };
    for (uint16_t pid : secrets) {
        TEST_ASSERT_FALSE(securityObjectAccessPermitted(pid, kRead, kPlain));
        TEST_ASSERT_FALSE(securityObjectAccessPermitted(pid, kWrite, kPlain));
        TEST_ASSERT_FALSE(securityObjectAccessPermitted(pid, kRead, kSecuredNoTool));
        TEST_ASSERT_TRUE(securityObjectAccessPermitted(pid, kRead, kToolSecured));
        TEST_ASSERT_TRUE(securityObjectAccessPermitted(pid, kWrite, kToolSecured));
    }
}

// A property nobody has classified yet must not become a hole the moment it is
// added to this object.
void test_unknown_security_property_defaults_to_denied(void) {
    constexpr uint16_t kNotYetSpecified = 200;
    TEST_ASSERT_FALSE(securityObjectAccessPermitted(kNotYetSpecified, kRead, kPlain));
    TEST_ASSERT_TRUE(securityObjectAccessPermitted(kNotYetSpecified, kRead, kToolSecured));
}

// --- Object identity and load state -----------------------------------------

// 3FF/0CC: ETS enumerates interface objects before any secure link exists, so
// the object's own type must stay readable.
void test_object_identity_stays_readable(void) {
    TEST_ASSERT_TRUE(securityObjectAccessPermitted(kPidObjectType, kRead, kPlain));
    TEST_ASSERT_FALSE(securityObjectAccessPermitted(kPidObjectType, kWrite, kPlain));
}

// §6.3.4: the Load Control gates the key tables, so only the Role "Tool" drives
// it — but reading the resulting state leaks nothing.
void test_load_state_readable_but_not_drivable_in_plain(void) {
    TEST_ASSERT_TRUE(securityObjectAccessPermitted(kPidLoadStateControl, kRead, kPlain));
    TEST_ASSERT_FALSE(securityObjectAccessPermitted(kPidLoadStateControl, kWrite, kPlain));
    TEST_ASSERT_TRUE(securityObjectAccessPermitted(kPidLoadStateControl, kWrite, kToolSecured));
}

// --- Management services at large --------------------------------------------

// With Security Mode off the device is an ordinary plain device: this is the
// state it is commissioned from, and refusing plain management here would make
// it impossible to ever secure.
void test_management_writes_stay_open_while_security_mode_is_off(void) {
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::PropertyValueWrite, false, kPlain));
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::MemoryWrite, false, kPlain));
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::Restart, false, kPlain));
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::IndividualAddressWrite, false, kPlain));
}

// "Full management access to the device is possible even in device security
// mode if KNX data security is used together with the tool key" — so without
// it, none.
void test_management_writes_need_the_tool_key_once_security_mode_is_on(void) {
    const APCIService services[] = {
        APCIService::PropertyValueWrite,
        APCIService::PropertyExtValueWriteCon,
        APCIService::PropertyExtValueWriteUnCon,
        APCIService::FunctionPropertyCommand,
        APCIService::FunctionPropertyExtCommand,
        APCIService::MemoryWrite,
        APCIService::MemoryExtendedWrite,
        APCIService::IndividualAddressWrite,
        APCIService::IndividualAddressSerialNumberWrite,
        APCIService::NetworkParameterWrite,
        APCIService::SystemNetworkParameterWrite,
        APCIService::AuthorizeRequest,
        APCIService::KeyWrite,
        APCIService::Restart,
    };
    for (APCIService service : services) {
        TEST_ASSERT_FALSE(managementWritePermitted(service, true, kPlain));
        TEST_ASSERT_FALSE(managementWritePermitted(service, true, kSecuredNoTool));
        TEST_ASSERT_TRUE(managementWritePermitted(service, true, kToolSecured));
    }
}

// Group communication is protected by the Group Object Security Flags and the
// group keys, not by this policy: gating it here would silence a secured
// installation's runtime traffic.
void test_group_communication_is_not_gated_as_management(void) {
    TEST_ASSERT_FALSE(isManagementWriteService(APCIService::GroupValueWrite));
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::GroupValueWrite, true, kPlain));
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::GroupValueRead, true, kPlain));
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::GroupValueResponse, true, kPlain));
}

// Reads keep their permissive 3FF policies; only the Security Interface Object
// restricts them, which securityObjectAccessPermitted() handles separately.
void test_reads_are_not_gated_by_the_service_level_policy(void) {
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::PropertyValueRead, true, kPlain));
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::MemoryRead, true, kPlain));
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::DeviceDescriptorRead, true, kPlain));
    TEST_ASSERT_TRUE(managementWritePermitted(APCIService::IndividualAddressRead, true, kPlain));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_security_mode_write_needs_secure_communication);
    RUN_TEST(test_security_mode_write_needs_the_tool_role);
    RUN_TEST(test_security_mode_write_needs_confidentiality);
    RUN_TEST(test_security_mode_read_stays_open);
    RUN_TEST(test_key_material_is_not_readable_in_plain);
    RUN_TEST(test_unknown_security_property_defaults_to_denied);
    RUN_TEST(test_object_identity_stays_readable);
    RUN_TEST(test_load_state_readable_but_not_drivable_in_plain);
    RUN_TEST(test_management_writes_stay_open_while_security_mode_is_off);
    RUN_TEST(test_management_writes_need_the_tool_key_once_security_mode_is_on);
    RUN_TEST(test_group_communication_is_not_gated_as_management);
    RUN_TEST(test_reads_are_not_gated_by_the_service_level_policy);

    return UNITY_END();
}
