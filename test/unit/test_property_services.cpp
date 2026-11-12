// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_property_services.cpp
 * @brief Unit tests for property services
 */

#include "knx/application/property.hpp"
#include "knx/application/property_services.hpp"
#include <unity.h>
#include <optional>
#include <span>
#include <vector>

using namespace knx::application;
using namespace knx;

void setUp(void) {}
void tearDown(void) {}

// ============================================================================
// Property Tests
// ============================================================================

void test_PropertyDataTypeSize(void) {
    TEST_ASSERT_EQUAL(1, propertyDataTypeSize(PropertyDataType::UnsignedChar));
    TEST_ASSERT_EQUAL(2, propertyDataTypeSize(PropertyDataType::UnsignedInt));
    TEST_ASSERT_EQUAL(4, propertyDataTypeSize(PropertyDataType::UnsignedLong));
    TEST_ASSERT_EQUAL(8, propertyDataTypeSize(PropertyDataType::Double));
    TEST_ASSERT_EQUAL(2, propertyDataTypeSize(PropertyDataType::Generic02));
}

// ============================================================================
// PropertyServices Tests
// ============================================================================

namespace {

PropertyDescriptionInfo describe(PropertyID pid, PropertyDataType type, PropertyWriteAccess writeAccess,
                                 uint8_t writeLevel = 0)
{
    PropertyDescriptionInfo info;
    info.objectIndex = InterfaceObjectIndex(0);
    info.propertyId = pid;
    info.writeAccess = writeAccess;
    info.type = type;
    info.maxElements = 1;
    info.writeLevel = writeLevel;
    return info;
}

} // namespace

void test_PropertyServices_ValueRead(void) {
    PropertyServices service;
    service.setReadProvider([](const IndividualAddress&, const PropertyValueReadRequest& request,
                               std::span<uint8_t> out) -> std::optional<util::Result<size_t>> {
        if (request.propertyId != PropertyID::ManufacturerId) {
            return std::nullopt;
        }
        out[0] = 0x12;
        out[1] = 0x34;
        return util::Result<size_t>(size_t{2});
    });

    bool responseSent = false;
    PropertyValueResponse receivedResponse;
    service.setValueResponseCallback([&](const IndividualAddress&, const PropertyValueResponse& response) {
        responseSent = true;
        receivedResponse = response;
    });

    PropertyValueReadRequest request;
    request.objectIndex = InterfaceObjectIndex(0);
    request.propertyId = PropertyID::ManufacturerId;
    request.startIndex = 1;
    request.elementCount = 1;

    IndividualAddress source(1, 2, 3);
    TEST_ASSERT_TRUE(service.handleValueRead(source, request).isOk());
    TEST_ASSERT_TRUE(responseSent);
    TEST_ASSERT_EQUAL(0, receivedResponse.objectIndex.value());
    TEST_ASSERT_EQUAL(1, receivedResponse.elementCount);
    TEST_ASSERT_EQUAL(2, receivedResponse.data.size());
    TEST_ASSERT_EQUAL(0x1234, (receivedResponse.data[0] << 8) | receivedResponse.data[1]);
}

void test_PropertyServices_UnansweredReadIsNegative(void) {
    // Nothing behind the services knows the property: the answer is the
    // negative response, never silence.
    PropertyServices service;
    bool responseSent = false;
    PropertyValueResponse receivedResponse;
    service.setValueResponseCallback([&](const IndividualAddress&, const PropertyValueResponse& response) {
        responseSent = true;
        receivedResponse = response;
    });

    PropertyValueReadRequest request;
    request.objectIndex = InterfaceObjectIndex(0);
    request.propertyId = static_cast<PropertyID>(250);
    request.startIndex = 1;
    request.elementCount = 1;

    TEST_ASSERT_TRUE(service.handleValueRead(IndividualAddress(1, 2, 3), request).isError());
    TEST_ASSERT_TRUE(responseSent);
    TEST_ASSERT_EQUAL(0, receivedResponse.elementCount);
    TEST_ASSERT_EQUAL(0, receivedResponse.data.size());
}

void test_PropertyServices_ValueWrite(void) {
    uint8_t progMode = 0;
    PropertyServices service;
    service.setWriteProvider([&](const IndividualAddress&, const PropertyValueWriteRequest& request) {
        progMode = request.data[0];
        return util::Result<void>::ok();
    });
    service.setReadProvider([&](const IndividualAddress&, const PropertyValueReadRequest&,
                                std::span<uint8_t> out) -> std::optional<util::Result<size_t>> {
        out[0] = progMode;
        return util::Result<size_t>(size_t{1});
    });

    PropertyValueResponse receivedResponse;
    service.setValueResponseCallback([&](const IndividualAddress&, const PropertyValueResponse& response) {
        receivedResponse = response;
    });

    PropertyValueWriteRequest request;
    request.objectIndex = InterfaceObjectIndex(0);
    request.propertyId = PropertyID::ProgMode;
    request.startIndex = 1;
    request.elementCount = 1;
    request.data = {1};

    TEST_ASSERT_TRUE(service.handleValueWrite(IndividualAddress(1, 2, 3), request).isOk());
    TEST_ASSERT_EQUAL(1, progMode);
    // The confirmation carries the value read back, not an echo.
    TEST_ASSERT_EQUAL(1, receivedResponse.elementCount);
    TEST_ASSERT_EQUAL(1, receivedResponse.data.size());
    TEST_ASSERT_EQUAL(1, receivedResponse.data[0]);
}

void test_PropertyServices_WriteLevelComesFromDescription(void) {
    uint8_t writeLevel = 0;
    bool written = false;
    PropertyServices service;
    service.setWriteProvider([&](const IndividualAddress&, const PropertyValueWriteRequest&) {
        written = true;
        return util::Result<void>::ok();
    });
    service.setDescriptionProvider([&](const IndividualAddress&, InterfaceObjectIndex, PropertyID pid,
                                       PropertyIndex) -> std::optional<PropertyDescriptionInfo> {
        return describe(pid, PropertyDataType::UnsignedChar, PropertyWriteAccess::Allowed, writeLevel);
    });
    service.setAccessLevelQuery([](const IndividualAddress&) { return uint8_t{2}; });

    PropertyValueResponse receivedResponse;
    service.setValueResponseCallback([&](const IndividualAddress&, const PropertyValueResponse& response) {
        receivedResponse = response;
    });

    PropertyValueWriteRequest request;
    request.objectIndex = InterfaceObjectIndex(0);
    request.propertyId = PropertyID::ProgMode;
    request.startIndex = 1;
    request.elementCount = 1;
    request.data = {1};

    // Level 2 is numerically above the described write level 0: refused.
    TEST_ASSERT_TRUE(service.handleValueWrite(IndividualAddress(1, 2, 3), request).isError());
    TEST_ASSERT_FALSE(written);
    TEST_ASSERT_EQUAL(0, receivedResponse.elementCount);

    // Described write level 2: the same partner may write.
    writeLevel = 2;
    TEST_ASSERT_TRUE(service.handleValueWrite(IndividualAddress(1, 2, 3), request).isOk());
    TEST_ASSERT_TRUE(written);
}

void test_PropertyServices_DescriptionNotFoundIsAnswered(void) {
    PropertyServices service;
    service.setDescriptionProvider([](const IndividualAddress&, InterfaceObjectIndex, PropertyID,
                                      PropertyIndex) -> std::optional<PropertyDescriptionInfo> {
        return std::nullopt;
    });

    bool answered = false;
    PropertyID answeredPid{};
    uint16_t answeredMax = 1;
    service.setDescriptionResponseCallback([&](const IndividualAddress&, InterfaceObjectIndex, PropertyID pid,
                                               PropertyIndex, PropertyWriteAccess, PropertyDataType,
                                               uint16_t maxElements, uint8_t, uint8_t) {
        answered = true;
        answeredPid = pid;
        answeredMax = maxElements;
    });

    TEST_ASSERT_TRUE(service.handleDescriptionRead(IndividualAddress(1, 2, 3), InterfaceObjectIndex(7),
                                                   static_cast<PropertyID>(250), PropertyIndex(0)).isOk());
    TEST_ASSERT_TRUE(answered);
    TEST_ASSERT_EQUAL(250, static_cast<int>(answeredPid));
    TEST_ASSERT_EQUAL(0, answeredMax);  // "property does not exist"
}

// ============================================================================
// Test Runner
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();

    // Property Tests
    RUN_TEST(test_PropertyDataTypeSize);

    // PropertyServices Tests
    RUN_TEST(test_PropertyServices_ValueRead);
    RUN_TEST(test_PropertyServices_UnansweredReadIsNegative);
    RUN_TEST(test_PropertyServices_ValueWrite);
    RUN_TEST(test_PropertyServices_WriteLevelComesFromDescription);
    RUN_TEST(test_PropertyServices_DescriptionNotFoundIsAnswered);

    return UNITY_END();
}
