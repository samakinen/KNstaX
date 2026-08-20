// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * The server half of KNXnet/IP Core: what a KNX IP device answers when ETS asks
 * the network who is there.
 *
 * Checked against the layouts of 03/08/02 §7.5.4 and §7.6–7.7 octet by octet,
 * because a DIB that is one field out is not a soft failure — ETS either does
 * not list the device or lists it as something it is not.
 */

#include "unity.h"

#include "knx/netip/core_discovery_codec.hpp"
#include "knx/netip/core_server_responder.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using knx::IndividualAddress;
using knx::IpAddress;
using knx::NetIpPort;
using knx::netip::CoreDeviceDescription;
using knx::netip::CoreDiscoveryCodec;
using knx::netip::CoreServerResponder;
using knx::netip::DatagramBuffer;
using knx::netip::PacketWriter;
using knx::netip::ServiceFamilySupport;
namespace service_family = knx::netip::service_family;
namespace control_packet = knx::netip::control_packet;

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr uint16_t kPort = 3671;

CoreDeviceDescription makeDevice()
{
    CoreDeviceDescription device{};
    device.medium = 0x20;  // KNX IP
    device.programmingMode = false;
    device.individualAddress = IndividualAddress(0x1102);  // 1.1.2
    device.projectInstallationId = 0x0000;
    device.serialNumber = {0x00, 0xFA, 0x11, 0x22, 0x33, 0x44};
    device.routingMulticastAddress = IpAddress::fromOctets(224, 0, 23, 12);
    device.macAddress = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    std::memcpy(device.friendlyName.data(), "Habinari", 8);
    device.mediumStatus = 0x00;
    device.maxLocalApduLength = 254;
    device.deviceDescriptorType0 = 0x07B0;
    return device;
}

std::vector<ServiceFamilySupport> families()
{
    return {
        {service_family::kCore, 2},
        {service_family::kDeviceManagement, 2},
        {service_family::kRouting, 2},
        {service_family::kSecurity, 1},
    };
}

std::vector<uint8_t> searchRequest(IpAddress replyTo, uint16_t replyPort)
{
    uint8_t a = 0, b = 0, c = 0, d = 0;
    replyTo.toOctets(a, b, c, d);
    return {
        0x06, 0x10, 0x02, 0x01, 0x00, 0x0E,
        0x08, 0x01, a, b, c, d,
        static_cast<uint8_t>(replyPort >> 8), static_cast<uint8_t>(replyPort & 0xFF),
    };
}

/// A SEARCH_REQUEST_EXTENDED with the given SRP blocks appended.
std::vector<uint8_t> extendedSearchRequest(IpAddress replyTo,
                                           uint16_t replyPort,
                                           const std::vector<uint8_t>& srps)
{
    uint8_t a = 0, b = 0, c = 0, d = 0;
    replyTo.toOctets(a, b, c, d);
    const size_t total = 6 + 8 + srps.size();
    std::vector<uint8_t> packet = {
        0x06, 0x10, 0x02, 0x0B,
        static_cast<uint8_t>(total >> 8), static_cast<uint8_t>(total & 0xFF),
        0x08, 0x01, a, b, c, d,
        static_cast<uint8_t>(replyPort >> 8), static_cast<uint8_t>(replyPort & 0xFF),
    };
    packet.insert(packet.end(), srps.begin(), srps.end());
    return packet;
}

CoreServerResponder makeResponder(const CoreDeviceDescription& device)
{
    CoreServerResponder responder;
    responder.setDescriptionProvider([device]() { return device; });
    const auto list = families();
    (void)responder.setServiceFamilies(std::span<const ServiceFamilySupport>(list));
    responder.setControlEndpoint(IpAddress::fromOctets(192, 168, 1, 50), NetIpPort(kPort));
    return responder;
}

} // namespace

void test_device_info_dib_matches_the_specified_layout(void)
{
    DatagramBuffer<256> buffer{};
    PacketWriter writer(buffer.span());
    TEST_ASSERT_TRUE(writer.reset(0).isOk());

    auto device = makeDevice();
    device.programmingMode = true;
    TEST_ASSERT_TRUE(CoreDiscoveryCodec::appendDeviceInfoDib(writer, device).isOk());

    const auto dib = writer.span();
    TEST_ASSERT_EQUAL_UINT32(54, dib.size());
    TEST_ASSERT_EQUAL_UINT8(54, dib[0]);    // structure length
    TEST_ASSERT_EQUAL_UINT8(0x01, dib[1]);  // DEVICE_INFO
    TEST_ASSERT_EQUAL_UINT8(0x20, dib[2]);  // KNX IP medium
    TEST_ASSERT_EQUAL_UINT8(0x01, dib[3]);  // Device Status: programming mode

    TEST_ASSERT_EQUAL_UINT8(0x11, dib[4]);  // individual address 1.1.2
    TEST_ASSERT_EQUAL_UINT8(0x02, dib[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00, dib[6]);  // project-installation id
    TEST_ASSERT_EQUAL_UINT8(0x00, dib[7]);

    TEST_ASSERT_EQUAL_UINT8(0x00, dib[8]);  // serial number
    TEST_ASSERT_EQUAL_UINT8(0xFA, dib[9]);
    TEST_ASSERT_EQUAL_UINT8(0x44, dib[13]);

    TEST_ASSERT_EQUAL_UINT8(224, dib[14]);  // routing multicast address
    TEST_ASSERT_EQUAL_UINT8(0, dib[15]);
    TEST_ASSERT_EQUAL_UINT8(23, dib[16]);
    TEST_ASSERT_EQUAL_UINT8(12, dib[17]);

    TEST_ASSERT_EQUAL_UINT8(0xAA, dib[18]);  // MAC address
    TEST_ASSERT_EQUAL_UINT8(0xFF, dib[23]);

    TEST_ASSERT_EQUAL_UINT8('H', dib[24]);  // friendly name, NUL-padded
    TEST_ASSERT_EQUAL_UINT8('i', dib[31]);
    for (size_t i = 32; i < 54; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0x00, dib[i]);
    }
}

void test_device_without_routing_reports_a_zero_multicast_address(void)
{
    DatagramBuffer<256> buffer{};
    PacketWriter writer(buffer.span());
    TEST_ASSERT_TRUE(writer.reset(0).isOk());

    auto device = makeDevice();
    device.routingMulticastAddress = IpAddress(0);
    TEST_ASSERT_TRUE(CoreDiscoveryCodec::appendDeviceInfoDib(writer, device).isOk());

    const auto dib = writer.span();
    for (size_t i = 14; i < 18; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0x00, dib[i]);
    }
}

void test_search_response_carries_control_endpoint_and_two_dibs(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    const auto request = searchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000);
    TEST_ASSERT_TRUE(responder.handle(request, IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    TEST_ASSERT_FALSE(reply.datagram.empty());

    const auto response = reply.datagram;
    TEST_ASSERT_EQUAL_UINT8(0x06, response[0]);
    TEST_ASSERT_EQUAL_UINT8(0x10, response[1]);
    TEST_ASSERT_EQUAL_UINT8(0x02, response[2]);  // SEARCH_RESPONSE
    TEST_ASSERT_EQUAL_UINT8(0x02, response[3]);

    const uint16_t totalLength = static_cast<uint16_t>((response[4] << 8) | response[5]);
    TEST_ASSERT_EQUAL_UINT32(totalLength, response.size());

    // HPAI of the server's own control endpoint.
    TEST_ASSERT_EQUAL_UINT8(0x08, response[6]);
    TEST_ASSERT_EQUAL_UINT8(0x01, response[7]);  // UDP
    TEST_ASSERT_EQUAL_UINT8(192, response[8]);
    TEST_ASSERT_EQUAL_UINT8(168, response[9]);
    TEST_ASSERT_EQUAL_UINT8(1, response[10]);
    TEST_ASSERT_EQUAL_UINT8(50, response[11]);
    TEST_ASSERT_EQUAL_UINT8(0x0E, response[12]);  // port 3671
    TEST_ASSERT_EQUAL_UINT8(0x57, response[13]);

    TEST_ASSERT_EQUAL_UINT8(54, response[14]);    // DEVICE_INFO DIB
    TEST_ASSERT_EQUAL_UINT8(0x01, response[15]);

    // Supported service families, minus Security: Table 3 does not allow it in
    // a plain SEARCH_RESPONSE, so three of the four are announced here.
    const size_t familiesOffset = 14 + 54;
    TEST_ASSERT_EQUAL_UINT8(2 + 3 * 2, response[familiesOffset]);
    TEST_ASSERT_EQUAL_UINT8(0x02, response[familiesOffset + 1]);
    TEST_ASSERT_EQUAL_UINT8(service_family::kCore, response[familiesOffset + 2]);
    TEST_ASSERT_EQUAL_UINT8(2, response[familiesOffset + 3]);
    TEST_ASSERT_EQUAL_UINT8(service_family::kRouting, response[familiesOffset + 6]);
}

void test_search_response_goes_to_the_requested_discovery_endpoint(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    // The HPAI, not the datagram source, decides where the answer goes.
    const auto request = searchRequest(IpAddress::fromOctets(192, 168, 1, 77), 40001);
    TEST_ASSERT_TRUE(responder.handle(request, IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    TEST_ASSERT_EQUAL_UINT32(IpAddress::fromOctets(192, 168, 1, 77).raw, reply.address.raw);
    TEST_ASSERT_EQUAL_UINT16(40001, reply.port.value());
}

void test_zero_hpai_replies_to_the_datagram_source(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    // 0.0.0.0:0 means "answer wherever this came from" — the NAT case.
    const auto request = searchRequest(IpAddress(0), 0);
    TEST_ASSERT_TRUE(responder.handle(request, IpAddress::fromOctets(10, 0, 0, 9), 41234, reply));
    TEST_ASSERT_EQUAL_UINT32(IpAddress::fromOctets(10, 0, 0, 9).raw, reply.address.raw);
    TEST_ASSERT_EQUAL_UINT16(41234, reply.port.value());
}

void test_search_request_with_a_tcp_endpoint_is_discarded(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    auto request = searchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000);
    request[7] = 0x02;  // HPAI protocol: TCP

    // Consumed — it is a Core request — but answered with silence (§7.6.1).
    TEST_ASSERT_TRUE(responder.handle(request, IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    TEST_ASSERT_TRUE(reply.datagram.empty());
}

void test_description_response_carries_dibs_and_no_hpai(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    std::vector<uint8_t> request = searchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000);
    request[3] = 0x03;  // DESCRIPTION_REQUEST

    TEST_ASSERT_TRUE(responder.handle(request, IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    const auto response = reply.datagram;
    TEST_ASSERT_EQUAL_UINT8(0x02, response[2]);  // DESCRIPTION_RESPONSE
    TEST_ASSERT_EQUAL_UINT8(0x04, response[3]);
    // The body starts with the DEVICE_INFO DIB rather than an HPAI.
    TEST_ASSERT_EQUAL_UINT8(54, response[6]);
    TEST_ASSERT_EQUAL_UINT8(0x01, response[7]);
}

void test_select_by_programming_mode_filters_on_the_live_flag(void)
{
    const std::vector<uint8_t> srp = {0x02, 0x81};  // mandatory, type 01h

    auto quiet = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;
    const auto request = extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, srp);
    TEST_ASSERT_TRUE(quiet.handle(request, IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    TEST_ASSERT_TRUE(reply.datagram.empty());

    auto device = makeDevice();
    device.programmingMode = true;
    auto selected = makeResponder(device);
    CoreServerResponder::Reply selectedReply;
    TEST_ASSERT_TRUE(selected.handle(request, IpAddress::fromOctets(192, 168, 1, 10), 55000, selectedReply));
    TEST_ASSERT_FALSE(selectedReply.datagram.empty());
    TEST_ASSERT_EQUAL_UINT8(0x0C, selectedReply.datagram[3]);  // SEARCH_RESPONSE_EXTENDED
}

void test_select_by_mac_address_matches_only_this_device(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    const std::vector<uint8_t> matching = {0x08, 0x82, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    auto request = extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, matching);
    TEST_ASSERT_TRUE(responder.handle(request, IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    TEST_ASSERT_FALSE(reply.datagram.empty());

    const std::vector<uint8_t> other = {0x08, 0x82, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    CoreServerResponder::Reply silent;
    request = extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, other);
    TEST_ASSERT_TRUE(responder.handle(request, IpAddress::fromOctets(192, 168, 1, 10), 55000, silent));
    TEST_ASSERT_TRUE(silent.datagram.empty());
}

void test_select_by_service_honours_the_minimum_version(void)
{
    auto responder = makeResponder(makeDevice());

    // Routing v2 is announced, so v2 matches and v3 does not.
    const std::vector<uint8_t> supported = {0x04, 0x83, service_family::kRouting, 0x02};
    CoreServerResponder::Reply reply;
    TEST_ASSERT_TRUE(responder.handle(
        extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, supported),
        IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    TEST_ASSERT_FALSE(reply.datagram.empty());

    const std::vector<uint8_t> tooNew = {0x04, 0x83, service_family::kRouting, 0x03};
    CoreServerResponder::Reply silent;
    TEST_ASSERT_TRUE(responder.handle(
        extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, tooNew),
        IpAddress::fromOctets(192, 168, 1, 10), 55000, silent));
    TEST_ASSERT_TRUE(silent.datagram.empty());

    // A family this device does not implement at all.
    const std::vector<uint8_t> absent = {0x04, 0x83, service_family::kObjectServer, 0x01};
    CoreServerResponder::Reply absentReply;
    TEST_ASSERT_TRUE(responder.handle(
        extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, absent),
        IpAddress::fromOctets(192, 168, 1, 10), 55000, absentReply));
    TEST_ASSERT_TRUE(absentReply.datagram.empty());
}

void test_unknown_srp_silences_only_when_mandatory(void)
{
    auto responder = makeResponder(makeDevice());

    // Type 00h is defined as invalid precisely so a client can test this.
    const std::vector<uint8_t> mandatory = {0x02, 0x80};
    CoreServerResponder::Reply silent;
    TEST_ASSERT_TRUE(responder.handle(
        extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, mandatory),
        IpAddress::fromOctets(192, 168, 1, 10), 55000, silent));
    TEST_ASSERT_TRUE(silent.datagram.empty());

    const std::vector<uint8_t> optional = {0x02, 0x00};
    CoreServerResponder::Reply reply;
    TEST_ASSERT_TRUE(responder.handle(
        extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, optional),
        IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    TEST_ASSERT_FALSE(reply.datagram.empty());
}

void test_extended_response_announces_security_and_the_extended_dib(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    TEST_ASSERT_TRUE(responder.handle(
        extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, {}),
        IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));

    const auto response = reply.datagram;
    TEST_ASSERT_EQUAL_UINT8(0x02, response[2]);
    TEST_ASSERT_EQUAL_UINT8(0x0C, response[3]);  // SEARCH_RESPONSE_EXTENDED

    // All four families, Security included — allowed only here.
    const size_t familiesOffset = 14 + 54;
    TEST_ASSERT_EQUAL_UINT8(2 + 4 * 2, response[familiesOffset]);
    TEST_ASSERT_EQUAL_UINT8(service_family::kSecurity, response[familiesOffset + 8]);

    // Then the Extended Device Information DIB.
    const size_t extendedOffset = familiesOffset + 2 + 4 * 2;
    TEST_ASSERT_EQUAL_UINT8(8, response[extendedOffset]);
    TEST_ASSERT_EQUAL_UINT8(0x08, response[extendedOffset + 1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, response[extendedOffset + 2]);  // medium status
    TEST_ASSERT_EQUAL_UINT8(0x00, response[extendedOffset + 4]);  // max APDU 254
    TEST_ASSERT_EQUAL_UINT8(0xFE, response[extendedOffset + 5]);
    TEST_ASSERT_EQUAL_UINT8(0x07, response[extendedOffset + 6]);  // mask 07B0
    TEST_ASSERT_EQUAL_UINT8(0xB0, response[extendedOffset + 7]);
}

void test_malformed_srp_block_does_not_loop_or_read_past_the_datagram(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    // A zero-length SRP would never advance the walk.
    const std::vector<uint8_t> zeroLength = {0x00, 0x81};
    TEST_ASSERT_TRUE(responder.handle(
        extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, zeroLength),
        IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    TEST_ASSERT_TRUE(reply.datagram.empty());

    // One that claims more octets than the datagram holds.
    const std::vector<uint8_t> overlong = {0x40, 0x82, 0xAA};
    CoreServerResponder::Reply second;
    TEST_ASSERT_TRUE(responder.handle(
        extendedSearchRequest(IpAddress::fromOctets(192, 168, 1, 10), 55000, overlong),
        IpAddress::fromOctets(192, 168, 1, 10), 55000, second));
    TEST_ASSERT_TRUE(second.datagram.empty());
}

void test_unrelated_service_types_are_left_alone(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    // A ROUTING_INDICATION must fall through to the routing path untouched.
    const std::vector<uint8_t> routing = {0x06, 0x10, 0x05, 0x30, 0x00, 0x08, 0x29, 0x00};
    TEST_ASSERT_FALSE(responder.handle(routing, IpAddress::fromOctets(192, 168, 1, 10), 3671, reply));
    TEST_ASSERT_TRUE(reply.datagram.empty());
}

void test_truncated_datagrams_are_not_answered(void)
{
    auto responder = makeResponder(makeDevice());
    CoreServerResponder::Reply reply;

    // Header claims 14 octets, only 10 arrived. The header decoder rejects the
    // length mismatch before the service type is trusted, so this is not even
    // claimed as a Core request — and above all it is not answered.
    const std::vector<uint8_t> truncated = {0x06, 0x10, 0x02, 0x01, 0x00, 0x0E, 0x08, 0x01, 0xC0, 0xA8};
    TEST_ASSERT_FALSE(responder.handle(truncated, IpAddress::fromOctets(192, 168, 1, 10), 55000, reply));
    TEST_ASSERT_TRUE(reply.datagram.empty());

    // A SEARCH_REQUEST whose HPAI is cut short is well-framed but unusable:
    // claimed, and answered with silence rather than a reply to nowhere.
    const std::vector<uint8_t> shortHpai = {0x06, 0x10, 0x02, 0x01, 0x00, 0x0A, 0x08, 0x01, 0xC0, 0xA8};
    CoreServerResponder::Reply second;
    TEST_ASSERT_TRUE(responder.handle(shortHpai, IpAddress::fromOctets(192, 168, 1, 10), 55000, second));
    TEST_ASSERT_TRUE(second.datagram.empty());
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_device_info_dib_matches_the_specified_layout);
    RUN_TEST(test_device_without_routing_reports_a_zero_multicast_address);
    RUN_TEST(test_search_response_carries_control_endpoint_and_two_dibs);
    RUN_TEST(test_search_response_goes_to_the_requested_discovery_endpoint);
    RUN_TEST(test_zero_hpai_replies_to_the_datagram_source);
    RUN_TEST(test_search_request_with_a_tcp_endpoint_is_discarded);
    RUN_TEST(test_description_response_carries_dibs_and_no_hpai);
    RUN_TEST(test_select_by_programming_mode_filters_on_the_live_flag);
    RUN_TEST(test_select_by_mac_address_matches_only_this_device);
    RUN_TEST(test_select_by_service_honours_the_minimum_version);
    RUN_TEST(test_unknown_srp_silences_only_when_mandatory);
    RUN_TEST(test_extended_response_announces_security_and_the_extended_dib);
    RUN_TEST(test_malformed_srp_block_does_not_loop_or_read_past_the_datagram);
    RUN_TEST(test_unrelated_service_types_are_left_alone);
    RUN_TEST(test_truncated_datagrams_are_not_answered);
    return UNITY_END();
}
