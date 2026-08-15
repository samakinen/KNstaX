// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_function_property_services.cpp
 * @brief Unit tests for A_FunctionPropertyCommand / State_Read / _Response.
 *
 * The behaviours pinned here are the ones a management client can observe and
 * that the spec is explicit about (03/03/07 §3.4.5), in particular the
 * degenerate "not a function property" response, which is distinguished from a
 * function that ran and failed only by the *absence* of the return code octet.
 */

#include "unity.h"
#include "knx/application/function_property_services.hpp"
#include "knx/protocol/tpdu_codec.hpp"

#include <vector>

using namespace knx;
using namespace knx::application;

namespace {

struct CapturedResponse {
    bool called{false};
    IndividualAddress destination{};
    InterfaceObjectIndex objectIndex{0};
    PropertyID propertyId{};
    bool hasReturnCode{false};
    FunctionPropertyReturnCode returnCode{FunctionPropertyReturnCode::Success};
    std::vector<uint8_t> data;
};

CapturedResponse g_response;

void installCapture(FunctionPropertyServices& services) {
    g_response = CapturedResponse{};
    services.setResponseCallback([](const IndividualAddress& destination,
                                    InterfaceObjectIndex objectIndex,
                                    PropertyID propertyId,
                                    bool hasReturnCode,
                                    FunctionPropertyReturnCode returnCode,
                                    std::span<const uint8_t> data) {
        g_response.called = true;
        g_response.destination = destination;
        g_response.objectIndex = objectIndex;
        g_response.propertyId = propertyId;
        g_response.hasReturnCode = hasReturnCode;
        g_response.returnCode = returnCode;
        g_response.data.assign(data.begin(), data.end());
    });
}

FunctionPropertyRequest makeRequest(FunctionPropertyInvocation invocation,
                                    std::initializer_list<uint8_t> data = {}) {
    FunctionPropertyRequest request{};
    request.objectIndex = InterfaceObjectIndex(5);
    request.propertyId = static_cast<PropertyID>(51);  // PID_SECURITY_MODE
    request.invocation = invocation;
    for (const uint8_t byte : data) {
        (void)request.data.push_back(byte);
    }
    return request;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- APCI decoding ---------------------------------------------------------

void test_function_property_apci_is_full_ten_bits(void) {
    // 0x2C7..0x2C9 live inside the 0x2Cx group, so a decoder that only looks at
    // bits 6-9 would collapse all three onto one service.
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::FunctionPropertyCommand),
                      static_cast<int>(APCIField(0x2C7).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::FunctionPropertyStateRead),
                      static_cast<int>(APCIField(0x2C8).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::FunctionPropertyStateResponse),
                      static_cast<int>(APCIField(0x2C9).service()));
}

void test_function_property_does_not_shadow_neighbouring_apcis(void) {
    // 0x2C0 with data bits set must still decode as its own group, not as a
    // function property.
    TEST_ASSERT_FALSE(APCIField(0x2C0).isFunctionProperty());
    TEST_ASSERT_FALSE(APCIField(0x2C6).isFunctionProperty());
    TEST_ASSERT_FALSE(APCIField(0x2CA).isFunctionProperty());
    TEST_ASSERT_TRUE(APCIField(0x2C7).isFunctionProperty());
}

// --- Codec round trip ------------------------------------------------------

void test_decode_command_request(void) {
    // TPCI/APCI header for A_FunctionPropertyCommand, then obj/prop/data.
    std::vector<uint8_t> apdu(6);
    const auto built = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::FunctionPropertyCommand),
        std::vector<uint8_t>{5, 51, 1},
        apdu);
    TEST_ASSERT_TRUE(built.isOk());
    apdu.resize(built.value());

    const auto decoded = FunctionPropertyServices::decodeRequest(apdu);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL(5u, decoded.value().objectIndex.value());
    TEST_ASSERT_EQUAL(51u, static_cast<uint8_t>(decoded.value().propertyId));
    TEST_ASSERT_EQUAL(static_cast<int>(FunctionPropertyInvocation::Command),
                      static_cast<int>(decoded.value().invocation));
    TEST_ASSERT_EQUAL(1u, decoded.value().data.size());
    TEST_ASSERT_EQUAL(1u, decoded.value().data[0]);
}

void test_decode_state_read_marks_invocation(void) {
    std::vector<uint8_t> apdu(6);
    const auto built = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::FunctionPropertyStateRead),
        std::vector<uint8_t>{5, 51},
        apdu);
    TEST_ASSERT_TRUE(built.isOk());
    apdu.resize(built.value());

    const auto decoded = FunctionPropertyServices::decodeRequest(apdu);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(FunctionPropertyInvocation::StateRead),
                      static_cast<int>(decoded.value().invocation));
    TEST_ASSERT_TRUE(decoded.value().data.empty());
}

void test_decode_rejects_short_apdu(void) {
    const std::vector<uint8_t> tooShort = {0x00, 0xC7, 0x05};
    TEST_ASSERT_TRUE(FunctionPropertyServices::decodeRequest(tooShort).isError());
}

void test_encode_response_with_return_code(void) {
    std::vector<uint8_t> out(16);
    const uint8_t payload[] = {0x01};
    const auto encoded = FunctionPropertyServices::encodeResponse(
        InterfaceObjectIndex(5), static_cast<PropertyID>(51), true,
        FunctionPropertyReturnCode::Success, payload, out);
    TEST_ASSERT_TRUE(encoded.isOk());
    // header(2) + object_index + property_id + return_code + data
    TEST_ASSERT_EQUAL(6u, encoded.value());

    const auto header = knx::protocol::unpackTpduHeader(out[0], out[1]);
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::FunctionPropertyStateResponse),
                      static_cast<int>(header.apci.service()));
    TEST_ASSERT_EQUAL(5u, out[2]);
    TEST_ASSERT_EQUAL(51u, out[3]);
    TEST_ASSERT_EQUAL(0u, out[4]);
    TEST_ASSERT_EQUAL(1u, out[5]);
}

void test_encode_response_without_return_code(void) {
    // §3.4.5.3: the error response omits both return_code and data, so the
    // whole PDU is 4 octets.  A client tells success-with-code-0 apart from
    // "not a function property" purely by this length.
    std::vector<uint8_t> out(16);
    const auto encoded = FunctionPropertyServices::encodeResponse(
        InterfaceObjectIndex(5), static_cast<PropertyID>(51), false,
        FunctionPropertyReturnCode::Success, {}, out);
    TEST_ASSERT_TRUE(encoded.isOk());
    TEST_ASSERT_EQUAL(4u, encoded.value());
}

// --- Dispatch behaviour ----------------------------------------------------

void test_handler_result_is_forwarded(void) {
    FunctionPropertyServices services;
    installCapture(services);
    services.setHandler([](const IndividualAddress&, const FunctionPropertyRequest& request)
                            -> util::Result<FunctionPropertyResult> {
        FunctionPropertyResult result{};
        result.returnCode = FunctionPropertyReturnCode::Success;
        (void)result.data.push_back(request.data.empty() ? 0u : request.data[0]);
        return result;
    });

    const auto res = services.handleRequest(IndividualAddress(1, 1, 1),
                                            makeRequest(FunctionPropertyInvocation::Command, {1}));
    TEST_ASSERT_TRUE(res.isOk());
    TEST_ASSERT_TRUE(g_response.called);
    TEST_ASSERT_TRUE(g_response.hasReturnCode);
    TEST_ASSERT_EQUAL(static_cast<int>(FunctionPropertyReturnCode::Success),
                      static_cast<int>(g_response.returnCode));
    TEST_ASSERT_EQUAL(1u, g_response.data.size());
    TEST_ASSERT_EQUAL(1u, g_response.data[0]);
}

void test_handler_error_produces_degenerate_response(void) {
    FunctionPropertyServices services;
    installCapture(services);
    services.setHandler([](const IndividualAddress&, const FunctionPropertyRequest&)
                            -> util::Result<FunctionPropertyResult> {
        return util::ErrorCode::OperationNotSupported;
    });

    const auto res = services.handleRequest(IndividualAddress(1, 1, 1),
                                            makeRequest(FunctionPropertyInvocation::Command, {1}));
    TEST_ASSERT_TRUE(res.isOk());  // The service itself succeeded: it answered.
    TEST_ASSERT_TRUE(g_response.called);
    TEST_ASSERT_FALSE(g_response.hasReturnCode);
    TEST_ASSERT_TRUE(g_response.data.empty());
}

void test_missing_handler_still_answers(void) {
    // Silence would leave the management client waiting for a timeout; the spec
    // wants the degenerate response instead.
    FunctionPropertyServices services;
    installCapture(services);

    const auto res = services.handleRequest(IndividualAddress(1, 1, 1),
                                            makeRequest(FunctionPropertyInvocation::StateRead));
    TEST_ASSERT_TRUE(res.isOk());
    TEST_ASSERT_TRUE(g_response.called);
    TEST_ASSERT_FALSE(g_response.hasReturnCode);
}

void test_nonzero_return_code_is_a_successful_call(void) {
    FunctionPropertyServices services;
    installCapture(services);
    services.setHandler([](const IndividualAddress&, const FunctionPropertyRequest&)
                            -> util::Result<FunctionPropertyResult> {
        FunctionPropertyResult result{};
        result.returnCode = FunctionPropertyReturnCode::InvalidCommand;
        return result;
    });

    (void)services.handleRequest(IndividualAddress(1, 1, 1),
                                 makeRequest(FunctionPropertyInvocation::Command, {9}));
    // The function ran and rejected the input: the return code must be present.
    TEST_ASSERT_TRUE(g_response.hasReturnCode);
    TEST_ASSERT_EQUAL(static_cast<int>(FunctionPropertyReturnCode::InvalidCommand),
                      static_cast<int>(g_response.returnCode));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_function_property_apci_is_full_ten_bits);
    RUN_TEST(test_function_property_does_not_shadow_neighbouring_apcis);
    RUN_TEST(test_decode_command_request);
    RUN_TEST(test_decode_state_read_marks_invocation);
    RUN_TEST(test_decode_rejects_short_apdu);
    RUN_TEST(test_encode_response_with_return_code);
    RUN_TEST(test_encode_response_without_return_code);
    RUN_TEST(test_handler_result_is_forwarded);
    RUN_TEST(test_handler_error_produces_degenerate_response);
    RUN_TEST(test_missing_handler_still_answers);
    RUN_TEST(test_nonzero_return_code_is_a_successful_call);
    return UNITY_END();
}
