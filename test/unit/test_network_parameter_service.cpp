// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_network_parameter_service.cpp
 * @brief Unit tests for serial-number-addressed commissioning and
 *        A_NetworkParameter_Read/_Write.
 *
 * These are broadcast services: every device on the line receives every
 * request.  Most of what is worth testing is therefore about *not* answering —
 * a device that replies to somebody else's serial number collides with the
 * real target's response.
 */

#include "unity.h"
#include "knx/application/network_parameter_service.hpp"
#include "knx/protocol/tpdu_codec.hpp"

#include <vector>

using namespace knx;
using namespace knx::application;

namespace {

constexpr KnxSerialNumber kOurSerial{0x00, 0xFA, 0x11, 0x22, 0x33, 0x44};
constexpr KnxSerialNumber kOtherSerial{0x00, 0xFA, 0xAA, 0xBB, 0xCC, 0xDD};

struct Captured {
    int serialResponses{0};
    int addressWrites{0};
    int parameterResponses{0};
    IndividualAddress lastAddress{};
    uint16_t lastDomain{0};
    std::vector<uint8_t> lastParameterValue;
};

Captured g_captured;

NetworkParameterService makeService() {
    g_captured = Captured{};
    NetworkParameterService service;
    service.setSerialNumber(kOurSerial);
    service.setSerialNumberResponseCallback(
        [](const KnxSerialNumber&, uint16_t domainAddress) {
            ++g_captured.serialResponses;
            g_captured.lastDomain = domainAddress;
        });
    service.setAddressWriteCallback([](const IndividualAddress& address) -> util::Result<void> {
        ++g_captured.addressWrites;
        g_captured.lastAddress = address;
        return util::Result<void>::ok();
    });
    return service;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- Serial-number read ----------------------------------------------------

void test_serial_read_answers_own_serial(void) {
    auto service = makeService();
    service.setDomainAddress(0xBEEF);

    TEST_ASSERT_TRUE(service.handleSerialNumberRead(kOurSerial).isOk());
    TEST_ASSERT_EQUAL(1, g_captured.serialResponses);
    TEST_ASSERT_EQUAL(0xBEEF, g_captured.lastDomain);
}

void test_serial_read_ignores_other_serial(void) {
    auto service = makeService();

    // Must succeed (this is not an error condition) but stay silent.
    TEST_ASSERT_TRUE(service.handleSerialNumberRead(kOtherSerial).isOk());
    TEST_ASSERT_EQUAL(0, g_captured.serialResponses);
}

// --- Serial-number write ---------------------------------------------------

void test_serial_write_applies_address_for_own_serial(void) {
    auto service = makeService();

    IndividualAddressSerialNumberWrite request{};
    request.serialNumber = kOurSerial;
    request.newAddress = IndividualAddress(1, 2, 3);

    TEST_ASSERT_TRUE(service.handleSerialNumberWrite(request).isOk());
    TEST_ASSERT_EQUAL(1, g_captured.addressWrites);
    TEST_ASSERT_EQUAL(IndividualAddress(1, 2, 3).raw, g_captured.lastAddress.raw);
}

void test_serial_write_ignores_other_serial(void) {
    auto service = makeService();

    IndividualAddressSerialNumberWrite request{};
    request.serialNumber = kOtherSerial;
    request.newAddress = IndividualAddress(1, 2, 3);

    TEST_ASSERT_TRUE(service.handleSerialNumberWrite(request).isOk());
    TEST_ASSERT_EQUAL(0, g_captured.addressWrites);
}

void test_serial_write_rejects_invalid_address(void) {
    // Accepting address 0 would leave the device unreachable, and the write is
    // addressed at us specifically, so silence would be misleading.
    auto service = makeService();

    IndividualAddressSerialNumberWrite request{};
    request.serialNumber = kOurSerial;
    request.newAddress = IndividualAddress(0);

    TEST_ASSERT_TRUE(service.handleSerialNumberWrite(request).isError());
    TEST_ASSERT_EQUAL(0, g_captured.addressWrites);
}

// --- Codecs ----------------------------------------------------------------

void test_serial_read_codec_round_trip(void) {
    std::vector<uint8_t> apdu(16);
    const auto encoded = NetworkParameterService::encodeSerialNumberRead(kOurSerial, apdu);
    TEST_ASSERT_TRUE(encoded.isOk());
    apdu.resize(encoded.value());

    const auto decoded = NetworkParameterService::decodeSerialNumberRead(apdu);
    TEST_ASSERT_TRUE(decoded.isOk());
    for (size_t i = 0; i < kKnxSerialNumberBytes; ++i) {
        TEST_ASSERT_EQUAL_UINT8(kOurSerial[i], decoded.value()[i]);
    }
}

void test_serial_write_codec_round_trip(void) {
    std::vector<uint8_t> apdu(24);
    const auto encoded = NetworkParameterService::encodeSerialNumberWrite(
        kOurSerial, IndividualAddress(1, 1, 5), apdu);
    TEST_ASSERT_TRUE(encoded.isOk());
    // header(2) + serial(6) + address(2) + reserved(4)
    TEST_ASSERT_EQUAL(14u, encoded.value());
    apdu.resize(encoded.value());

    const auto decoded = NetworkParameterService::decodeSerialNumberWrite(apdu);
    TEST_ASSERT_TRUE(decoded.isOk());
    for (size_t i = 0; i < kKnxSerialNumberBytes; ++i) {
        TEST_ASSERT_EQUAL_UINT8(kOurSerial[i], decoded.value().serialNumber[i]);
    }
    TEST_ASSERT_EQUAL(IndividualAddress(1, 1, 5).raw, decoded.value().newAddress.raw);
}

void test_serial_write_decodes_without_reserved_octets(void) {
    // Some tools omit the 4 reserved octets.  The payload is unambiguous
    // without them, so rejecting it would break interop for no benefit.
    std::vector<uint8_t> apdu(24);
    const auto encoded = NetworkParameterService::encodeSerialNumberWrite(
        kOurSerial, IndividualAddress(1, 1, 5), apdu);
    TEST_ASSERT_TRUE(encoded.isOk());
    apdu.resize(encoded.value() - 4);  // drop the reserved tail

    const auto decoded = NetworkParameterService::decodeSerialNumberWrite(apdu);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL(IndividualAddress(1, 1, 5).raw, decoded.value().newAddress.raw);
}

void test_serial_response_carries_domain_address(void) {
    std::vector<uint8_t> apdu(24);
    const auto encoded =
        NetworkParameterService::encodeSerialNumberResponse(kOurSerial, 0x1234, apdu);
    TEST_ASSERT_TRUE(encoded.isOk());
    // header(2) + serial(6) + domain(2) + reserved(2)
    TEST_ASSERT_EQUAL(12u, encoded.value());

    const auto header = knx::protocol::unpackTpduHeader(apdu[0], apdu[1]);
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::IndividualAddressSerialNumberResponse),
                      static_cast<int>(header.apci.service()));
    TEST_ASSERT_EQUAL(0x12, apdu[8]);
    TEST_ASSERT_EQUAL(0x34, apdu[9]);
}

// --- Network parameter -----------------------------------------------------

void test_network_parameter_read_stays_silent_when_unsupported(void) {
    auto service = makeService();
    service.setNetworkParameterResponseCallback(
        [](InterfaceObjectType, PropertyID, std::span<const uint8_t>) {
            ++g_captured.parameterResponses;
        });
    // No read handler installed: this device simply does not expose parameters.

    NetworkParameterRequest request{};
    request.objectType = InterfaceObjectType::device();
    request.propertyId = static_cast<PropertyID>(12);

    TEST_ASSERT_TRUE(service.handleNetworkParameterRead(request).isOk());
    TEST_ASSERT_EQUAL(0, g_captured.parameterResponses);
}

void test_network_parameter_read_answers_when_supported(void) {
    auto service = makeService();
    service.setNetworkParameterReadHandler(
        [](const NetworkParameterRequest&) -> util::Result<NetworkParameterValueBuffer> {
            NetworkParameterValueBuffer value{};
            (void)value.push_back(0x00);
            (void)value.push_back(0xFA);
            return value;
        });
    service.setNetworkParameterResponseCallback(
        [](InterfaceObjectType, PropertyID, std::span<const uint8_t> value) {
            ++g_captured.parameterResponses;
            g_captured.lastParameterValue.assign(value.begin(), value.end());
        });

    NetworkParameterRequest request{};
    request.objectType = InterfaceObjectType::device();
    request.propertyId = static_cast<PropertyID>(12);

    TEST_ASSERT_TRUE(service.handleNetworkParameterRead(request).isOk());
    TEST_ASSERT_EQUAL(1, g_captured.parameterResponses);
    TEST_ASSERT_EQUAL(2u, g_captured.lastParameterValue.size());
    TEST_ASSERT_EQUAL(0xFA, g_captured.lastParameterValue[1]);
}

void test_network_parameter_handler_error_is_silence_not_failure(void) {
    // "We do not have that parameter" is a normal outcome on a broadcast.
    auto service = makeService();
    service.setNetworkParameterReadHandler(
        [](const NetworkParameterRequest&) -> util::Result<NetworkParameterValueBuffer> {
            return util::ErrorCode::InvalidParameter;
        });
    service.setNetworkParameterResponseCallback(
        [](InterfaceObjectType, PropertyID, std::span<const uint8_t>) {
            ++g_captured.parameterResponses;
        });

    NetworkParameterRequest request{};
    TEST_ASSERT_TRUE(service.handleNetworkParameterRead(request).isOk());
    TEST_ASSERT_EQUAL(0, g_captured.parameterResponses);
}

void test_network_parameter_codec_round_trip(void) {
    std::vector<uint8_t> apdu(24);
    const uint8_t value[] = {0xDE, 0xAD};
    const auto encoded = NetworkParameterService::encodeNetworkParameterResponse(
        InterfaceObjectType::device(), static_cast<PropertyID>(12), value, apdu);
    TEST_ASSERT_TRUE(encoded.isOk());
    apdu.resize(encoded.value());

    const auto decoded = NetworkParameterService::decodeNetworkParameter(apdu);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL(InterfaceObjectType::device().value(), decoded.value().objectType.value());
    TEST_ASSERT_EQUAL(12u, static_cast<uint8_t>(decoded.value().propertyId));
    TEST_ASSERT_EQUAL(2u, decoded.value().value.size());
    TEST_ASSERT_EQUAL(0xDE, decoded.value().value[0]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_serial_read_answers_own_serial);
    RUN_TEST(test_serial_read_ignores_other_serial);
    RUN_TEST(test_serial_write_applies_address_for_own_serial);
    RUN_TEST(test_serial_write_ignores_other_serial);
    RUN_TEST(test_serial_write_rejects_invalid_address);
    RUN_TEST(test_serial_read_codec_round_trip);
    RUN_TEST(test_serial_write_codec_round_trip);
    RUN_TEST(test_serial_write_decodes_without_reserved_octets);
    RUN_TEST(test_serial_response_carries_domain_address);
    RUN_TEST(test_network_parameter_read_stays_silent_when_unsupported);
    RUN_TEST(test_network_parameter_read_answers_when_supported);
    RUN_TEST(test_network_parameter_handler_error_is_silence_not_failure);
    RUN_TEST(test_network_parameter_codec_round_trip);
    return UNITY_END();
}
