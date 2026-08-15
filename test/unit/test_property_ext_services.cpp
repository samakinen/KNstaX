// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_property_ext_services.cpp
 * @brief Unit tests for the KNX extended property services
 *
 * Pinned against 03/03/07 Application Layer v02.01.01 §3.4.3.2, §3.4.5, §3.4.8
 * (The KNX Standard v3.0.0).
 */

#include "knx/application/property_ext_services.hpp"
#include <unity.h>

#include <optional>
#include <vector>

using namespace knx::application;

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr uint16_t kDeviceObjectType = 0;
constexpr uint16_t kAppProgramObjectType = 3;
constexpr auto kTestPid = static_cast<PropertyID>(51);
constexpr auto kReadOnlyPid = static_cast<PropertyID>(52);

struct Fixture {
    PropertyStoreManager manager;
    PropertyExtServices services{manager};

    bool valueResponseSeen = false;
    APCIService valueService{};
    PropertyExtValueResponse valueResponse;

    bool descriptionSeen = false;
    PropertyExtDescriptionResponse descriptionResponse;

    bool functionSeen = false;
    FunctionPropertyExtResponse functionResponse;

    Fixture()
    {
        auto* device = manager.addObject(knx::InterfaceObjectType(kDeviceObjectType),
                                         knx::InterfaceObjectIndex(0));
        TEST_ASSERT_NOT_NULL(device);
        (void)device->registerProperty(
            PropertyDescriptor{kTestPid, PropertyDataType::UnsignedChar,
                               PropertyAccess::ReadWrite, 4, 0, 0},
            {});
        (void)device->registerProperty(
            PropertyDescriptor{kReadOnlyPid, PropertyDataType::UnsignedChar,
                               PropertyAccess::ReadOnly, 1, 0, 0},
            {});

        // A second object of a different type, so instance resolution has to
        // discriminate rather than always landing on index 0.
        auto* app = manager.addObject(knx::InterfaceObjectType(kAppProgramObjectType),
                                      knx::InterfaceObjectIndex(1));
        TEST_ASSERT_NOT_NULL(app);
        (void)app->registerProperty(
            PropertyDescriptor{kTestPid, PropertyDataType::UnsignedChar,
                               PropertyAccess::ReadWrite, 2, 0, 0},
            {});

        services.setValueResponseCallback(
            [this](const knx::IndividualAddress&, APCIService service,
                   const PropertyExtValueResponse& response) {
                valueResponseSeen = true;
                valueService = service;
                valueResponse = response;
            });
        services.setDescriptionResponseCallback(
            [this](const knx::IndividualAddress&, const PropertyExtDescriptionResponse& response) {
                descriptionSeen = true;
                descriptionResponse = response;
            });
        services.setFunctionResponseCallback(
            [this](const knx::IndividualAddress&, const FunctionPropertyExtResponse& response) {
                functionSeen = true;
                functionResponse = response;
            });
    }
};

knx::IndividualAddress source() { return knx::IndividualAddress(1, 2, 3); }

PropertyExtValueRequest makeValueRequest(uint16_t objectType,
                                         uint16_t objectInstance,
                                         uint16_t propertyId,
                                         uint8_t elementCount,
                                         uint16_t startIndex)
{
    PropertyExtValueRequest request;
    request.header.objectType = objectType;
    request.header.objectInstance = objectInstance;
    request.header.propertyId = propertyId;
    request.elementCount = elementCount;
    request.startIndex = startIndex;
    return request;
}

} // namespace

// ---------------------------------------------------------------------------
// Header codec — the 12-bit packing is the easiest thing to get wrong
// ---------------------------------------------------------------------------

void test_PropertyExtHeader_RoundTripsTwelveBitFields(void) {
    PropertyExtHeader header;
    header.objectType = 0xABCD;
    header.objectInstance = 0x0FFF;  // max 12-bit
    header.propertyId = 0x0ABC;

    uint8_t encoded[PropertyExtHeader::kEncodedLength]{};
    header.encode(encoded);

    // octet 10 = instance high 8, octet 11 = instance low 4 | pid high 4.
    TEST_ASSERT_EQUAL(0xAB, encoded[0]);
    TEST_ASSERT_EQUAL(0xCD, encoded[1]);
    TEST_ASSERT_EQUAL(0xFF, encoded[2]);
    TEST_ASSERT_EQUAL(0xFA, encoded[3]);
    TEST_ASSERT_EQUAL(0xBC, encoded[4]);

    const auto decoded = PropertyExtHeader::decode(encoded);
    TEST_ASSERT_EQUAL(header.objectType, decoded.objectType);
    TEST_ASSERT_EQUAL(header.objectInstance, decoded.objectInstance);
    TEST_ASSERT_EQUAL(header.propertyId, decoded.propertyId);
}

void test_PropertyExtHeader_RejectsOutOfRangeFields(void) {
    PropertyExtHeader header;
    header.objectInstance = 0x1000;  // 13 bits — not representable
    TEST_ASSERT_FALSE(header.fieldsInRange());
}

// ---------------------------------------------------------------------------
// APCI decoding
// ---------------------------------------------------------------------------

void test_PropertyExt_ApciDecodeDoesNotCollideWithAdcResponse(void) {
    // 0x1CC..0x1D6 mask to service group 0x1C0 (A_ADC_Response), so the decoder
    // must match all ten bits or the device answers ETS as if it were an ADC.
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::PropertyExtValueRead),
                      static_cast<int>(APCIField(0x1CC).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::PropertyExtValueWriteCon),
                      static_cast<int>(APCIField(0x1CE).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::PropertyExtValueWriteUnCon),
                      static_cast<int>(APCIField(0x1D0).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::PropertyExtDescriptionRead),
                      static_cast<int>(APCIField(0x1D2).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::FunctionPropertyExtCommand),
                      static_cast<int>(APCIField(0x1D4).service()));
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::FunctionPropertyExtStateResponse),
                      static_cast<int>(APCIField(0x1D6).service()));

    // Still below the extended blocks: a genuine A_ADC_Response.
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::ADCResponse),
                      static_cast<int>(APCIField(0x1C5).service()));
}

void test_PropertyExt_ReturnCodesMatchSpecTable(void) {
    // §3.4.5.5 — generic range E0h..FEh, not small integers.
    TEST_ASSERT_EQUAL(0x00, static_cast<int>(KnxReturnCode::Success));
    TEST_ASSERT_EQUAL(0xF5, static_cast<int>(KnxReturnCode::DataOverflow));
    TEST_ASSERT_EQUAL(0xF6, static_cast<int>(KnxReturnCode::DataMin));
    TEST_ASSERT_EQUAL(0xF7, static_cast<int>(KnxReturnCode::DataMax));
    TEST_ASSERT_EQUAL(0xF8, static_cast<int>(KnxReturnCode::DataVoid));
    TEST_ASSERT_EQUAL(0xFB, static_cast<int>(KnxReturnCode::AccessReadOnly));
    TEST_ASSERT_EQUAL(0xFD, static_cast<int>(KnxReturnCode::AddressVoid));
    TEST_ASSERT_EQUAL(0xFE, static_cast<int>(KnxReturnCode::DataTypeConflict));
}

// ---------------------------------------------------------------------------
// Object resolution by (type, instance)
// ---------------------------------------------------------------------------

void test_PropertyExt_ResolvesObjectByTypeAndInstance(void) {
    Fixture f;
    const auto device = f.services.resolveObject(kDeviceObjectType, 1);
    TEST_ASSERT_TRUE(device.has_value());
    TEST_ASSERT_EQUAL(0, device->value());

    const auto app = f.services.resolveObject(kAppProgramObjectType, 1);
    TEST_ASSERT_TRUE(app.has_value());
    TEST_ASSERT_EQUAL(1, app->value());

    // Instance is 1-based: instance 0 does not exist, nor does a second one.
    TEST_ASSERT_FALSE(f.services.resolveObject(kDeviceObjectType, 0).has_value());
    TEST_ASSERT_FALSE(f.services.resolveObject(kDeviceObjectType, 2).has_value());
    TEST_ASSERT_FALSE(f.services.resolveObject(99, 1).has_value());
}

// ---------------------------------------------------------------------------
// A_PropertyExtValue_Read
// ---------------------------------------------------------------------------

void test_PropertyExtValue_ReadReturnsData(void) {
    Fixture f;
    const std::vector<uint8_t> seed = {0x11, 0x22, 0x33, 0x44};
    TEST_ASSERT_TRUE(f.manager.writeProperty(knx::InterfaceObjectIndex(0), kTestPid, 1, 4, seed).isOk());

    auto request = makeValueRequest(kDeviceObjectType, 1, static_cast<uint16_t>(kTestPid), 4, 1);
    TEST_ASSERT_TRUE(f.services.handleValueRead(source(), request).isOk());

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::PropertyExtValueResponse),
                      static_cast<int>(f.valueService));
    TEST_ASSERT_EQUAL(4, f.valueResponse.elementCount);
    TEST_ASSERT_EQUAL(1, f.valueResponse.startIndex);
    TEST_ASSERT_EQUAL(4, f.valueResponse.data.size());
    TEST_ASSERT_EQUAL(0x11, f.valueResponse.data[0]);
    TEST_ASSERT_EQUAL(0x44, f.valueResponse.data[3]);
}

void test_PropertyExtValue_StartIndexZeroReturnsElementCount(void) {
    // §3.4.5.1: start_index 0 asks how many elements exist; the answer is a
    // 2-octet count with nr_of_elem forced to 1 whatever was requested.
    Fixture f;
    auto request = makeValueRequest(kDeviceObjectType, 1, static_cast<uint16_t>(kTestPid), 7, 0);
    TEST_ASSERT_TRUE(f.services.handleValueRead(source(), request).isOk());

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(1, f.valueResponse.elementCount);
    TEST_ASSERT_EQUAL(0, f.valueResponse.startIndex);
    TEST_ASSERT_EQUAL(2, f.valueResponse.data.size());
    TEST_ASSERT_EQUAL(0, f.valueResponse.data[0]);
    TEST_ASSERT_EQUAL(4, f.valueResponse.data[1]);
}

void test_PropertyExtValue_UnknownObjectAnswersAddressVoid(void) {
    // A negative answer must still be sent: nr_of_elem 0, start_index echoed,
    // one octet of error information.
    Fixture f;
    auto request = makeValueRequest(99, 1, static_cast<uint16_t>(kTestPid), 1, 5);
    TEST_ASSERT_FALSE(f.services.handleValueRead(source(), request).isOk());

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(0, f.valueResponse.elementCount);
    TEST_ASSERT_EQUAL(5, f.valueResponse.startIndex);
    TEST_ASSERT_EQUAL(1, f.valueResponse.data.size());
    TEST_ASSERT_EQUAL(0xFD, f.valueResponse.data[0]);
}

// ---------------------------------------------------------------------------
// A_PropertyExtValue_WriteCon / _WriteUnCon
// ---------------------------------------------------------------------------

void test_PropertyExtValue_WriteConStoresAndConfirms(void) {
    Fixture f;
    auto request = makeValueRequest(kDeviceObjectType, 1, static_cast<uint16_t>(kTestPid), 2, 1);
    (void)request.data.push_back(0xAA);
    (void)request.data.push_back(0xBB);

    TEST_ASSERT_TRUE(f.services.handleValueWrite(source(), request, true).isOk());

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::PropertyExtValueWriteConRes),
                      static_cast<int>(f.valueService));
    TEST_ASSERT_EQUAL(2, f.valueResponse.elementCount);
    TEST_ASSERT_EQUAL(0x00, static_cast<int>(f.valueResponse.returnCode));

    uint8_t readBack[2]{};
    TEST_ASSERT_TRUE(f.manager.readProperty(knx::InterfaceObjectIndex(0), kTestPid, 1, 2, readBack).isOk());
    TEST_ASSERT_EQUAL(0xAA, readBack[0]);
    TEST_ASSERT_EQUAL(0xBB, readBack[1]);
}

void test_PropertyExtValue_WriteUnConNeverAnswers(void) {
    // The unconfirmed flavour must stay silent even when it fails, or it puts
    // an unexpected response on the bus.
    Fixture f;
    auto request = makeValueRequest(99, 1, static_cast<uint16_t>(kTestPid), 1, 1);
    (void)request.data.push_back(0xAA);

    TEST_ASSERT_FALSE(f.services.handleValueWrite(source(), request, false).isOk());
    TEST_ASSERT_FALSE(f.valueResponseSeen);
}

void test_PropertyExtValue_WriteToReadOnlyIsAccessReadOnly(void) {
    Fixture f;
    auto request = makeValueRequest(kDeviceObjectType, 1, static_cast<uint16_t>(kReadOnlyPid), 1, 1);
    (void)request.data.push_back(0xAA);

    TEST_ASSERT_FALSE(f.services.handleValueWrite(source(), request, true).isOk());

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::PropertyExtValueWriteConRes),
                      static_cast<int>(f.valueService));
    // E_ACCESS_READ_ONLY, not the authorisation-flavoured E_ACCESS_DENIED.
    TEST_ASSERT_EQUAL(0xFB, static_cast<int>(f.valueResponse.returnCode));
    TEST_ASSERT_EQUAL(0, f.valueResponse.elementCount);
}

// ETS shortens a table it is about to re-download by writing element 0 — two
// octets, whatever the element size is. Sizing that write like an element write
// is what answered ETS's "clear the Security Individual Address Table"
// (nr_of_elem = 1, start_index = 0, data = 0000) with
// E_LENGTH_EXCEEDS_MAX_APDU_LENGTH instead of performing it.
void test_PropertyExtValue_ElementCountWriteIsNotSizedLikeAnElement(void) {
    Fixture f;
    auto request = makeValueRequest(kDeviceObjectType, 1, static_cast<uint16_t>(kTestPid), 1, 0);
    (void)request.data.push_back(0x00);
    (void)request.data.push_back(0x00);

    // The property store behind this fixture has no resize hook, so the write
    // is refused — but on the grounds of what it does, not of its length: the
    // answer must no longer be the "APDU too short" code.
    (void)f.services.handleValueWrite(source(), request, true);

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_NOT_EQUAL(0xF4, static_cast<int>(f.valueResponse.returnCode));
}

void test_PropertyExtValue_TruncatedWriteIsRejected(void) {
    Fixture f;
    auto request = makeValueRequest(kDeviceObjectType, 1, static_cast<uint16_t>(kTestPid), 4, 1);
    (void)request.data.push_back(0xAA);  // claims 4 elements, sends 1

    TEST_ASSERT_FALSE(f.services.handleValueWrite(source(), request, true).isOk());
    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(0xF4, static_cast<int>(f.valueResponse.returnCode));
}

// ---------------------------------------------------------------------------
// A_PropertyExtDescription_Read
// ---------------------------------------------------------------------------

void test_PropertyExtDescription_ReadByIdReportsAccessAndLimits(void) {
    Fixture f;
    PropertyExtDescriptionRequest request;
    request.header.objectType = kDeviceObjectType;
    request.header.objectInstance = 1;
    request.header.propertyId = static_cast<uint16_t>(kTestPid);
    request.descriptionType = 0;
    request.propertyIndex = 0;

    TEST_ASSERT_TRUE(f.services.handleDescriptionRead(source(), request).isOk());

    TEST_ASSERT_TRUE(f.descriptionSeen);
    TEST_ASSERT_EQUAL(0, f.descriptionResponse.descriptionType);
    TEST_ASSERT_TRUE(f.descriptionResponse.writeEnabled);
    TEST_ASSERT_EQUAL(4, f.descriptionResponse.maxElements);
}

void test_PropertyExtDescription_ReadOnlyPropertyIsNotWriteEnabled(void) {
    Fixture f;
    PropertyExtDescriptionRequest request;
    request.header.objectType = kDeviceObjectType;
    request.header.objectInstance = 1;
    request.header.propertyId = static_cast<uint16_t>(kReadOnlyPid);

    TEST_ASSERT_TRUE(f.services.handleDescriptionRead(source(), request).isOk());
    TEST_ASSERT_TRUE(f.descriptionSeen);
    TEST_ASSERT_FALSE(f.descriptionResponse.writeEnabled);
}

void test_PropertyExtDescription_UnsupportedTypeStillAnswers(void) {
    Fixture f;
    PropertyExtDescriptionRequest request;
    request.header.objectType = kDeviceObjectType;
    request.header.objectInstance = 1;
    request.header.propertyId = static_cast<uint16_t>(kTestPid);
    request.descriptionType = 1;  // only type zero is defined

    TEST_ASSERT_FALSE(f.services.handleDescriptionRead(source(), request).isOk());
    TEST_ASSERT_TRUE(f.descriptionSeen);
}

// ---------------------------------------------------------------------------
// A_FunctionPropertyExtCommand / _State_Read
// ---------------------------------------------------------------------------

void test_FunctionPropertyExt_CommandReachesProviderAndAnswers(void) {
    Fixture f;
    bool invoked = false;
    FunctionPropertyInvocation seenInvocation{};
    f.services.setFunctionProvider(
        [&](const knx::IndividualAddress&, const PropertyExtHeader&,
            FunctionPropertyInvocation invocation, const knx::RequestSecurity&,
            std::span<const uint8_t> input,
            FunctionPropertyExtResponse::DataBuffer& output) -> std::optional<KnxReturnCode> {
            invoked = true;
            seenInvocation = invocation;
            for (uint8_t byte : input) {
                (void)output.push_back(static_cast<uint8_t>(byte + 1));
            }
            return KnxReturnCode::Success;
        });

    FunctionPropertyExtRequest request;
    request.header.objectType = kDeviceObjectType;
    request.header.objectInstance = 1;
    request.header.propertyId = static_cast<uint16_t>(kTestPid);
    (void)request.data.push_back(0x10);

    TEST_ASSERT_TRUE(
        f.services.handleFunctionProperty(source(), request, FunctionPropertyInvocation::Command).isOk());

    TEST_ASSERT_TRUE(invoked);
    TEST_ASSERT_EQUAL(static_cast<int>(FunctionPropertyInvocation::Command),
                      static_cast<int>(seenInvocation));
    TEST_ASSERT_TRUE(f.functionSeen);
    TEST_ASSERT_EQUAL(0x00, static_cast<int>(f.functionResponse.returnCode));
    TEST_ASSERT_EQUAL(1, f.functionResponse.data.size());
    TEST_ASSERT_EQUAL(0x11, f.functionResponse.data[0]);
}

void test_FunctionPropertyExt_NoProviderIsTypeConflict(void) {
    Fixture f;
    FunctionPropertyExtRequest request;
    request.header.objectType = kDeviceObjectType;
    request.header.objectInstance = 1;
    request.header.propertyId = static_cast<uint16_t>(kTestPid);

    TEST_ASSERT_FALSE(
        f.services.handleFunctionProperty(source(), request, FunctionPropertyInvocation::StateRead).isOk());

    TEST_ASSERT_TRUE(f.functionSeen);
    TEST_ASSERT_EQUAL(0xFE, static_cast<int>(f.functionResponse.returnCode));
}

void test_FunctionPropertyExt_UnknownObjectAnswersAddressVoid(void) {
    Fixture f;
    FunctionPropertyExtRequest request;
    request.header.objectType = 99;
    request.header.objectInstance = 1;
    request.header.propertyId = static_cast<uint16_t>(kTestPid);

    TEST_ASSERT_FALSE(
        f.services.handleFunctionProperty(source(), request, FunctionPropertyInvocation::Command).isOk());

    TEST_ASSERT_TRUE(f.functionSeen);
    TEST_ASSERT_EQUAL(0xFD, static_cast<int>(f.functionResponse.returnCode));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_PropertyExtHeader_RoundTripsTwelveBitFields);
    RUN_TEST(test_PropertyExtHeader_RejectsOutOfRangeFields);
    RUN_TEST(test_PropertyExt_ApciDecodeDoesNotCollideWithAdcResponse);
    RUN_TEST(test_PropertyExt_ReturnCodesMatchSpecTable);
    RUN_TEST(test_PropertyExt_ResolvesObjectByTypeAndInstance);

    RUN_TEST(test_PropertyExtValue_ReadReturnsData);
    RUN_TEST(test_PropertyExtValue_StartIndexZeroReturnsElementCount);
    RUN_TEST(test_PropertyExtValue_UnknownObjectAnswersAddressVoid);
    RUN_TEST(test_PropertyExtValue_WriteConStoresAndConfirms);
    RUN_TEST(test_PropertyExtValue_WriteUnConNeverAnswers);
    RUN_TEST(test_PropertyExtValue_WriteToReadOnlyIsAccessReadOnly);
    RUN_TEST(test_PropertyExtValue_TruncatedWriteIsRejected);
    RUN_TEST(test_PropertyExtValue_ElementCountWriteIsNotSizedLikeAnElement);

    RUN_TEST(test_PropertyExtDescription_ReadByIdReportsAccessAndLimits);
    RUN_TEST(test_PropertyExtDescription_ReadOnlyPropertyIsNotWriteEnabled);
    RUN_TEST(test_PropertyExtDescription_UnsupportedTypeStillAnswers);

    RUN_TEST(test_FunctionPropertyExt_CommandReachesProviderAndAnswers);
    RUN_TEST(test_FunctionPropertyExt_NoProviderIsTypeConflict);
    RUN_TEST(test_FunctionPropertyExt_UnknownObjectAnswersAddressVoid);

    return UNITY_END();
}
