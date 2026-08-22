// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * Which broadcasts go out as ROUTING_SYSTEM_BROADCAST (03/02/06 §4.1.3).
 *
 * The two failure modes are opposite and both break commissioning: classify too
 * little and ETS never receives the S-A_Sync_Response that ends a secure
 * download; classify too much and A_IndividualAddress_Write lands in the system
 * domain, where plain commissioning cannot find it.
 */

#include "unity.h"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/system_broadcast.hpp"
#include "knx/protocol/tpci.hpp"

#include <cstdint>
#include <vector>

using knx::AddressType;
using knx::GroupAddress;
using knx::IndividualAddress;
using knx::Priority;
using knx::datalink::LDataFrame;
using knx::netip::isSystemBroadcast;
using knx::netip::isSystemBroadcastApdu;

void setUp(void) {}
void tearDown(void) {}

namespace {

LDataFrame broadcastFrame(const std::vector<uint8_t>& tpdu)
{
    LDataFrame frame;
    frame.standardFrame = true;
    frame.priority = Priority::System;
    frame.source = IndividualAddress(0x1101);
    frame.destination = GroupAddress(0x0000);
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;
    for (const uint8_t byte : tpdu) {
        frame.tpdu.push_back(byte);
    }
    return frame;
}

/// A Data Secure APDU: APCI 0x03F1, then the Security Control Field.
std::vector<uint8_t> secureApdu(uint8_t scf)
{
    return {0x03, 0xF1, scf, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
}

} // namespace

void test_secure_apdu_with_sbc_set_is_a_system_broadcast(void)
{
    // SCF bit 3 is the system-broadcast flag, and ETS sets it on the sync
    // exchange that closes a secure download.
    TEST_ASSERT_TRUE(isSystemBroadcast(broadcastFrame(secureApdu(0x08))));
    TEST_ASSERT_TRUE(isSystemBroadcast(broadcastFrame(secureApdu(0x18))));
}

void test_secure_apdu_without_sbc_is_an_ordinary_broadcast(void)
{
    TEST_ASSERT_FALSE(isSystemBroadcast(broadcastFrame(secureApdu(0x00))));
    TEST_ASSERT_FALSE(isSystemBroadcast(broadcastFrame(secureApdu(0x10))));
}

void test_individual_address_write_stays_an_ordinary_broadcast(void)
{
    // A_IndividualAddress_Write, APCI 0x0C0, carrying 1.1.2. This is how a
    // device in programming mode gets its address, and it is a plain broadcast.
    const auto frame = broadcastFrame({0x00, 0xC0, 0x11, 0x02});
    TEST_ASSERT_FALSE(isSystemBroadcast(frame));
}

void test_domain_address_serial_number_services_are_system_broadcasts(void)
{
    // 03/03/07 §3.3.6/§3.3.7 pass all three to T_Data_SystemBroadcast. On
    // KNXnet/IP the two domains are different service types, so a response sent
    // as an ordinary routing indication never reaches the client that asked.
    TEST_ASSERT_TRUE(isSystemBroadcast(broadcastFrame({0x03, 0xEC, 0x00, 0x00})));  // Read
    TEST_ASSERT_TRUE(isSystemBroadcast(broadcastFrame({0x03, 0xED, 0x00, 0x00})));  // Response
    TEST_ASSERT_TRUE(isSystemBroadcast(broadcastFrame({0x03, 0xEE, 0x00, 0x00})));  // Write
}

void test_individual_address_serial_number_services_stay_ordinary_broadcasts(void)
{
    // The neighbouring trio at 0x3DC..0x3DE is the contrast that matters: same
    // escape block, same serial-number addressing, but §3.2.4/§3.2.5 send them
    // with T_Data_Broadcast. Sweeping the whole block into the system domain
    // would move these with it.
    TEST_ASSERT_FALSE(isSystemBroadcast(broadcastFrame({0x03, 0xDC, 0x00, 0x00})));
    TEST_ASSERT_FALSE(isSystemBroadcast(broadcastFrame({0x03, 0xDD, 0x00, 0x00})));
    TEST_ASSERT_FALSE(isSystemBroadcast(broadcastFrame({0x03, 0xDE, 0x00, 0x00})));
}

void test_system_network_parameter_services_are_system_broadcasts(void)
{
    // A_SystemNetworkParameter_Read / _Response / _Write, 0x1C8..0x1CA.
    TEST_ASSERT_TRUE(isSystemBroadcast(broadcastFrame({0x01, 0xC8, 0x00, 0x00})));
    TEST_ASSERT_TRUE(isSystemBroadcast(broadcastFrame({0x01, 0xC9, 0x00, 0x00})));
    TEST_ASSERT_TRUE(isSystemBroadcast(broadcastFrame({0x01, 0xCA, 0x00, 0x00})));
}

void test_adc_response_is_not_mistaken_for_a_system_network_parameter(void)
{
    // A_ADC_Response is 0x1C0 and shares the service group with the system
    // network parameter block; only all ten APCI bits tell them apart.
    TEST_ASSERT_FALSE(isSystemBroadcast(broadcastFrame({0x01, 0xC0, 0x00, 0x00})));
}

void test_group_and_point_to_point_frames_are_never_system_broadcasts(void)
{
    // Group traffic to a real address, even carrying a secured APDU with the
    // SBC bit set: the domain distinction only exists for broadcasts.
    LDataFrame group = broadcastFrame(secureApdu(0x08));
    group.destination = GroupAddress(0x2301);
    TEST_ASSERT_FALSE(isSystemBroadcast(group));

    LDataFrame p2p = broadcastFrame(secureApdu(0x08));
    p2p.destinationType = AddressType::Individual;
    p2p.destination = GroupAddress(0x1102);
    TEST_ASSERT_FALSE(isSystemBroadcast(p2p));
}

void test_short_apdus_are_rejected_rather_than_read_past(void)
{
    TEST_ASSERT_FALSE(isSystemBroadcastApdu({}));
    const std::vector<uint8_t> oneByte = {0x03};
    TEST_ASSERT_FALSE(isSystemBroadcastApdu(oneByte));
    // A secure APCI with no SCF octet behind it.
    const std::vector<uint8_t> truncated = {0x03, 0xF1};
    TEST_ASSERT_FALSE(isSystemBroadcastApdu(truncated));
}

void test_tpci_bits_do_not_disturb_the_apci_match(void)
{
    // The first octet's top six bits are TPCI. A connected-mode secure APDU
    // must still be recognised by its APCI.
    std::vector<uint8_t> connected = secureApdu(0x08);
    connected[0] = static_cast<uint8_t>(0x40 | 0x03);  // numbered data packet
    TEST_ASSERT_TRUE(isSystemBroadcastApdu(connected));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_secure_apdu_with_sbc_set_is_a_system_broadcast);
    RUN_TEST(test_secure_apdu_without_sbc_is_an_ordinary_broadcast);
    RUN_TEST(test_individual_address_write_stays_an_ordinary_broadcast);
    RUN_TEST(test_domain_address_serial_number_services_are_system_broadcasts);
    RUN_TEST(test_individual_address_serial_number_services_stay_ordinary_broadcasts);
    RUN_TEST(test_system_network_parameter_services_are_system_broadcasts);
    RUN_TEST(test_adc_response_is_not_mistaken_for_a_system_network_parameter);
    RUN_TEST(test_group_and_point_to_point_frames_are_never_system_broadcasts);
    RUN_TEST(test_short_apdus_are_rejected_rather_than_read_past);
    RUN_TEST(test_tpci_bits_do_not_disturb_the_apci_match);
    return UNITY_END();
}
