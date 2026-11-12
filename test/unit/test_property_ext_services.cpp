// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_property_ext_services.cpp
 * @brief Unit tests for the KNX extended property services
 *
 * Pinned against 03/03/07 Application Layer v02.01.01 §3.4.3.2, §3.4.5, §3.4.8
 * (The KNX Standard v3.0.0).
 *
 * The services run on the real property model: an InterfaceObjectManager with
 * the core interface objects, reached through the same kind of providers the
 * BAU installs. The classic PropertyServices sit on the same providers, so the
 * two families can be compared directly — they describe the same objects and
 * must agree.
 */

#include "knx/application/property_ext_services.hpp"
#include "knx/application/property_services.hpp"
#include "knx/application/group_object.hpp"
#include "knx/objects/interface_object_manager.hpp"
#include <unity.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

using namespace knx::application;
using knx::InterfaceObjectIndex;
using knx::InterfaceObjectInstance;
using knx::InterfaceObjectType;
using knx::objects::InterfaceObjectManager;
using knx::objects::PropertyAccessResult;

void setUp(void) {}
void tearDown(void) {}

namespace {

constexpr uint16_t kDeviceObjectType = 0;
constexpr uint16_t kGroupObjectTableType = 9;
constexpr uint16_t kSecurityObjectType = 17;

constexpr auto kProgMode = PropertyID::ProgMode;               // Device, RW, PDT_BITSET8
constexpr auto kManufacturerId = PropertyID::ManufacturerId;   // Device, RO
constexpr auto kGotTable = PropertyID::Table;                  // Group Object Table PID_TABLE
constexpr auto kSecurityIaTable = static_cast<PropertyID>(54); // PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE
constexpr auto kParameterStart = static_cast<PropertyID>(18);  // Application Program, RW, PDT_UNSIGNED_INT

/// The core objects at the indices the BAU gives them (bau.cpp).
const std::array<InterfaceObjectType, 6> kCoreObjects = {
    InterfaceObjectType::device(),
    InterfaceObjectType::addressTable(),
    InterfaceObjectType::associationTable(),
    InterfaceObjectType::groupObjectTable(),
    InterfaceObjectType::applicationProgram(),
    InterfaceObjectType::security(),
};

struct ClassicDescription {
    bool seen{false};
    PropertyID propertyId{};
    knx::PropertyIndex propertyIndex{};
    knx::PropertyWriteAccess writeAccess{};
    PropertyDataType type{};
    uint16_t maxElements{0};
    uint8_t readLevel{0};
    uint8_t writeLevel{0};
};

struct Fixture {
    InterfaceObjectManager manager;
    PropertyServices classic;
    PropertyExtServices services;

    bool valueResponseSeen = false;
    APCIService valueService{};
    PropertyExtValueResponse valueResponse;

    bool descriptionSeen = false;
    PropertyExtDescriptionResponse descriptionResponse;

    bool functionSeen = false;
    FunctionPropertyExtResponse functionResponse;

    ClassicDescription classicDescription;
    PropertyValueResponse classicValue;

    std::optional<InterfaceObjectType> typeAt(InterfaceObjectIndex index) const
    {
        if (index.value() >= kCoreObjects.size()) {
            return std::nullopt;
        }
        return kCoreObjects[index.value()];
    }

    Fixture()
    {
        TEST_ASSERT_TRUE(manager.init(false).isOk());

        // The providers below are the BAU's (bau.cpp), on the core objects.
        const PropertyReadProvider read =
            [this](const knx::IndividualAddress&, const PropertyValueReadRequest& request,
                   std::span<uint8_t> out) -> std::optional<knx::util::Result<size_t>> {
            const auto type = typeAt(request.objectIndex);
            if (!type) {
                return std::nullopt;
            }
            PropertyServiceDataBuffer data;
            if (manager.readProperty(*type, InterfaceObjectInstance(1), request.propertyId,
                                     request.startIndex, request.elementCount, data)
                    != PropertyAccessResult::Success) {
                return std::nullopt;
            }
            std::copy(data.begin(), data.end(), out.begin());
            return knx::util::Result<size_t>(data.size());
        };
        const PropertyWriteProvider write =
            [this](const knx::IndividualAddress&, const PropertyValueWriteRequest& request)
            -> knx::util::Result<void> {
            const auto type = typeAt(request.objectIndex);
            if (!type || manager.writeProperty(*type, InterfaceObjectInstance(1), request.propertyId,
                                               request.startIndex, request.data.span())
                             != PropertyAccessResult::Success) {
                return knx::util::Result<void>::err(knx::util::ErrorCode::OperationFailed);
            }
            return knx::util::Result<void>::ok();
        };
        const PropertyDescriptionProvider describe =
            [this](const knx::IndividualAddress&, InterfaceObjectIndex objectIndex, PropertyID propertyId,
                   knx::PropertyIndex propertyIndex) -> std::optional<PropertyDescriptionInfo> {
            const auto type = typeAt(objectIndex);
            if (!type) {
                return std::nullopt;
            }
            knx::objects::PropertyDescription description;
            if (manager.describeProperty(*type, InterfaceObjectInstance(1), propertyId,
                                         propertyIndex.value(), description)
                    != PropertyAccessResult::Success) {
                return std::nullopt;
            }
            PropertyDescriptionInfo info;
            info.objectIndex = objectIndex;
            info.propertyId = description.id;
            info.propertyIndex = knx::PropertyIndex(static_cast<uint8_t>(description.index));
            info.type = description.type;
            info.maxElements = description.maxElements;
            info.writeAccess = knx::objects::canWrite(description.capability) ? knx::PropertyWriteAccess::Allowed
                                                                              : knx::PropertyWriteAccess::Denied;
            info.readable = knx::objects::canRead(description.capability);
            info.readLevel = description.readLevel;
            info.writeLevel = description.writeLevel;
            return info;
        };

        services.setReadProvider(read);
        services.setWriteProvider(write);
        services.setDescriptionProvider(describe);
        services.setObjectResolver(
            [](uint16_t objectType, uint16_t objectInstance) -> std::optional<InterfaceObjectIndex> {
                // One object of each core type, so only instance 1 exists.
                for (size_t i = 0; i < kCoreObjects.size(); ++i) {
                    if (kCoreObjects[i].value() == objectType && objectInstance == 1) {
                        return InterfaceObjectIndex(static_cast<uint8_t>(i));
                    }
                }
                return std::nullopt;
            });
        classic.setReadProvider(read);
        classic.setWriteProvider(write);
        classic.setDescriptionProvider(describe);

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
        classic.setDescriptionResponseCallback(
            [this](const knx::IndividualAddress&, InterfaceObjectIndex, PropertyID propertyId,
                   knx::PropertyIndex propertyIndex, knx::PropertyWriteAccess writeAccess,
                   PropertyDataType type, uint16_t maxElements, uint8_t readLevel, uint8_t writeLevel) {
                classicDescription = {true, propertyId, propertyIndex, writeAccess, type,
                                      maxElements, readLevel, writeLevel};
            });
        classic.setValueResponseCallback(
            [this](const knx::IndividualAddress&, const PropertyValueResponse& response) {
                classicValue = response;
            });
    }

    PropertyExtDescriptionResponse extDescribe(uint16_t objectType, uint16_t propertyId, uint16_t propertyIndex)
    {
        PropertyExtDescriptionRequest request;
        request.header.objectType = objectType;
        request.header.objectInstance = 1;
        request.header.propertyId = propertyId;
        request.descriptionType = 0;
        request.propertyIndex = propertyIndex;
        descriptionSeen = false;
        (void)services.handleDescriptionRead(knx::IndividualAddress(1, 2, 3), request);
        TEST_ASSERT_TRUE(descriptionSeen);
        return descriptionResponse;
    }

    ClassicDescription classicDescribe(uint8_t objectIndex, uint8_t propertyId, uint8_t propertyIndex)
    {
        classicDescription = {};
        (void)classic.handleDescriptionRead(knx::IndividualAddress(1, 2, 3), InterfaceObjectIndex(objectIndex),
                                            static_cast<PropertyID>(propertyId),
                                            knx::PropertyIndex(propertyIndex));
        TEST_ASSERT_TRUE(classicDescription.seen);
        return classicDescription;
    }
};

knx::IndividualAddress source() { return knx::IndividualAddress(1, 2, 3); }

PropertyExtValueRequest makeValueRequest(uint16_t objectType,
                                         uint16_t objectInstance,
                                         PropertyID propertyId,
                                         uint8_t elementCount,
                                         uint16_t startIndex)
{
    PropertyExtValueRequest request;
    request.header.objectType = objectType;
    request.header.objectInstance = objectInstance;
    request.header.propertyId = static_cast<uint16_t>(propertyId);
    request.elementCount = elementCount;
    request.startIndex = startIndex;
    return request;
}

void addGroupObject(Fixture& f)
{
    GroupObjectConfig config{};
    config.address = knx::GroupAddress(1, 2, 0);
    config.dpt = dptids::Bool;
    config.flags.communication = true;
    config.flags.write = true;
    (void)f.manager.groupObjectTable().addGroupObject(std::make_unique<GroupObject>(config));
}

uint16_t descriptorFor(bool readable, bool transmit)
{
    GroupObjectFlags flags{};
    flags.communication = true;
    flags.read = readable;
    flags.transmit = transmit;
    flags.priority = knx::Priority::Low;
    return encodeGroupObjectDescriptor(flags, 0u);
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
// Object addressing by (type, instance)
// ---------------------------------------------------------------------------

void test_PropertyExt_InstanceZeroAndMissingInstancesAreAddressVoid(void) {
    // Instance is 1-based (03/05/01 §4.18.5.2.5): instance 0 does not exist,
    // nor does a second Device Object.
    for (const uint16_t instance : {uint16_t{0}, uint16_t{2}}) {
        Fixture f;
        auto request = makeValueRequest(kDeviceObjectType, instance, kProgMode, 1, 1);
        TEST_ASSERT_FALSE(f.services.handleValueRead(source(), request).isOk());
        TEST_ASSERT_TRUE(f.valueResponseSeen);
        TEST_ASSERT_EQUAL(0xFD, f.valueResponse.data[0]);
    }
}

// ---------------------------------------------------------------------------
// A_PropertyExtValue_Read
// ---------------------------------------------------------------------------

void test_PropertyExtValue_ReadReturnsLiveData(void) {
    Fixture f;
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      f.manager.writeProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                              kProgMode, 1, {0x01}));

    auto request = makeValueRequest(kDeviceObjectType, 1, kProgMode, 1, 1);
    TEST_ASSERT_TRUE(f.services.handleValueRead(source(), request).isOk());

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::PropertyExtValueResponse),
                      static_cast<int>(f.valueService));
    TEST_ASSERT_EQUAL(1, f.valueResponse.elementCount);
    TEST_ASSERT_EQUAL(1, f.valueResponse.startIndex);
    TEST_ASSERT_EQUAL(1, f.valueResponse.data.size());
    TEST_ASSERT_EQUAL(0x01, f.valueResponse.data[0]);
}

void test_PropertyExtValue_StartIndexZeroReturnsElementCount(void) {
    // §3.4.5.1: start_index 0 asks how many elements exist; the answer is a
    // 2-octet count with nr_of_elem forced to 1 whatever was requested.
    Fixture f;
    auto request = makeValueRequest(kDeviceObjectType, 1, kProgMode, 7, 0);
    TEST_ASSERT_TRUE(f.services.handleValueRead(source(), request).isOk());

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(1, f.valueResponse.elementCount);
    TEST_ASSERT_EQUAL(0, f.valueResponse.startIndex);
    TEST_ASSERT_EQUAL(2, f.valueResponse.data.size());
    TEST_ASSERT_EQUAL(0, f.valueResponse.data[0]);
    TEST_ASSERT_EQUAL(1, f.valueResponse.data[1]);
}

void test_PropertyExtValue_UnknownObjectAnswersAddressVoid(void) {
    // A negative answer must still be sent: nr_of_elem 0, start_index echoed,
    // one octet of error information.
    Fixture f;
    auto request = makeValueRequest(99, 1, kProgMode, 1, 5);
    TEST_ASSERT_FALSE(f.services.handleValueRead(source(), request).isOk());

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(0, f.valueResponse.elementCount);
    TEST_ASSERT_EQUAL(5, f.valueResponse.startIndex);
    TEST_ASSERT_EQUAL(1, f.valueResponse.data.size());
    TEST_ASSERT_EQUAL(0xFD, f.valueResponse.data[0]);
}

void test_PropertyExtValue_UnknownPropertyAnswersAddressVoid(void) {
    Fixture f;
    // Above 255 the 12-bit PID cannot name anything here; 250 is simply absent.
    for (const uint16_t pid : {uint16_t{250}, uint16_t{0x0ABC}}) {
        auto request = makeValueRequest(kDeviceObjectType, 1, static_cast<PropertyID>(0), 1, 1);
        request.header.propertyId = pid;
        f.valueResponseSeen = false;
        TEST_ASSERT_FALSE(f.services.handleValueRead(source(), request).isOk());
        TEST_ASSERT_TRUE(f.valueResponseSeen);
        TEST_ASSERT_EQUAL(0xFD, f.valueResponse.data[0]);
    }
}

// ---------------------------------------------------------------------------
// A_PropertyExtValue_WriteCon / _WriteUnCon
// ---------------------------------------------------------------------------

void test_PropertyExtValue_WriteConReachesTheObjectAndConfirms(void) {
    Fixture f;
    auto request = makeValueRequest(kDeviceObjectType, 1, kProgMode, 1, 1);
    (void)request.data.push_back(0x01);

    TEST_ASSERT_TRUE(f.services.handleValueWrite(source(), request, true).isOk());

    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(static_cast<int>(APCIService::PropertyExtValueWriteConRes),
                      static_cast<int>(f.valueService));
    TEST_ASSERT_EQUAL(1, f.valueResponse.elementCount);
    TEST_ASSERT_EQUAL(0x00, static_cast<int>(f.valueResponse.returnCode));

    PropertyServiceDataBuffer readBack;
    TEST_ASSERT_EQUAL(PropertyAccessResult::Success,
                      f.manager.readProperty(InterfaceObjectType::device(), InterfaceObjectInstance(1),
                                             kProgMode, 1, 1, readBack));
    TEST_ASSERT_EQUAL(1, readBack.size());
    TEST_ASSERT_EQUAL(0x01, readBack[0]);
}

void test_PropertyExtValue_WriteUnConNeverAnswers(void) {
    // The unconfirmed flavour must stay silent even when it fails, or it puts
    // an unexpected response on the bus.
    Fixture f;
    auto request = makeValueRequest(99, 1, kProgMode, 1, 1);
    (void)request.data.push_back(0xAA);

    TEST_ASSERT_FALSE(f.services.handleValueWrite(source(), request, false).isOk());
    TEST_ASSERT_FALSE(f.valueResponseSeen);
}

void test_PropertyExtValue_WriteToReadOnlyIsAccessReadOnly(void) {
    Fixture f;
    auto request = makeValueRequest(kDeviceObjectType, 1, kManufacturerId, 1, 1);
    (void)request.data.push_back(0x00);
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
    auto request = makeValueRequest(kSecurityObjectType, 1, kSecurityIaTable, 1, 0);
    (void)request.data.push_back(0x00);
    (void)request.data.push_back(0x00);

    TEST_ASSERT_TRUE(f.services.handleValueWrite(source(), request, true).isOk());
    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(0x00, static_cast<int>(f.valueResponse.returnCode));
}

void test_PropertyExtValue_TruncatedWriteIsRejected(void) {
    Fixture f;
    // PID_PARAMETER_START is PDT_UNSIGNED_INT: one element is two octets.
    auto request = makeValueRequest(3 /* Application Program */, 1, kParameterStart, 1, 1);
    (void)request.data.push_back(0xAA);

    TEST_ASSERT_FALSE(f.services.handleValueWrite(source(), request, true).isOk());
    TEST_ASSERT_TRUE(f.valueResponseSeen);
    TEST_ASSERT_EQUAL(0xF4, static_cast<int>(f.valueResponse.returnCode));
}

// ---------------------------------------------------------------------------
// A_PropertyExtDescription_Read
// ---------------------------------------------------------------------------

void test_PropertyExtDescription_ReadByIdReportsAccessLimitsAndIndex(void) {
    Fixture f;
    const auto response = f.extDescribe(kDeviceObjectType, static_cast<uint16_t>(kProgMode), 0);

    TEST_ASSERT_EQUAL(0, response.descriptionType);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(kProgMode), response.header.propertyId);
    TEST_ASSERT_TRUE(response.writeEnabled);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(PropertyDataType::Bitset8), response.propertyDataType);
    TEST_ASSERT_EQUAL(1, response.maxElements);
    // "The Property Index […] shall be the correct value of the Property Index
    // of the addressed Property": the request's index (0) is not echoed.
    const auto byIndex = f.extDescribe(kDeviceObjectType, 0, response.propertyIndex);
    TEST_ASSERT_NOT_EQUAL(0, response.propertyIndex);
    TEST_ASSERT_EQUAL(static_cast<uint16_t>(kProgMode), byIndex.header.propertyId);
}

void test_PropertyExtDescription_ReadOnlyPropertyIsNotWriteEnabled(void) {
    Fixture f;
    const auto response = f.extDescribe(kDeviceObjectType, static_cast<uint16_t>(kManufacturerId), 0);
    TEST_ASSERT_FALSE(response.writeEnabled);
    TEST_ASSERT_EQUAL(1, response.maxElements);
}

void test_PropertyExtDescription_IndexZeroIsObjectType(void) {
    // §3.4.3.2: "Property Index = 0 shall mean the first Property".
    Fixture f;
    for (const auto type : kCoreObjects) {
        const auto response = f.extDescribe(type.value(), 0, 0);
        TEST_ASSERT_EQUAL(static_cast<uint16_t>(PropertyID::ObjectType), response.header.propertyId);
        TEST_ASSERT_EQUAL(0, response.propertyIndex);
        TEST_ASSERT_EQUAL(static_cast<uint8_t>(PropertyDataType::UnsignedInt), response.propertyDataType);
    }
}

void test_PropertyExtDescription_OtherTypeIsAnsweredAsTypeZero(void) {
    // §3.4.3.2: "If another Property Description Type than zero is requested,
    // then the MaS shall answer with the […] Response-PDU of the Property
    // Description Type zero."
    Fixture f;
    PropertyExtDescriptionRequest request;
    request.header.objectType = kDeviceObjectType;
    request.header.objectInstance = 1;
    request.header.propertyId = static_cast<uint16_t>(kProgMode);
    request.descriptionType = 1;

    TEST_ASSERT_TRUE(f.services.handleDescriptionRead(source(), request).isOk());
    TEST_ASSERT_TRUE(f.descriptionSeen);
    TEST_ASSERT_EQUAL(0, f.descriptionResponse.descriptionType);
    TEST_ASSERT_EQUAL(1, f.descriptionResponse.maxElements);
}

void test_PropertyExtDescription_MissingPropertyIsAllZero(void) {
    Fixture f;
    const auto response = f.extDescribe(kDeviceObjectType, 250, 0);
    TEST_ASSERT_EQUAL(0, response.descriptionType);
    TEST_ASSERT_FALSE(response.writeEnabled);
    TEST_ASSERT_EQUAL(0, response.propertyDataType);
    TEST_ASSERT_EQUAL(0, response.maxElements);
    TEST_ASSERT_EQUAL(0, response.readLevel);
    TEST_ASSERT_EQUAL(0, response.writeLevel);
}

// ---------------------------------------------------------------------------
// Classic and extended services describe the same objects
// ---------------------------------------------------------------------------

// For every index of every core object, A_PropertyDescription_Read and
// A_PropertyExtDescription_Read name the same property with the same
// description, the index after the last property is absent in both, and a
// by-PID request in either family reports the property's own index.
void test_ClassicAndExtendedDescriptionsAgreeOnEveryIndex(void) {
    Fixture f;
    for (size_t object = 0; object < kCoreObjects.size(); ++object) {
        const uint16_t type = kCoreObjects[object].value();
        uint8_t previousPid = 0;
        uint8_t index = 0;
        for (;; ++index) {
            TEST_ASSERT_TRUE(index < 64);
            const auto classic = f.classicDescribe(static_cast<uint8_t>(object), 0, index);
            const auto ext = f.extDescribe(type, 0, index);

            if (classic.maxElements == 0) {
                // Past the last property: both say "does not exist".
                TEST_ASSERT_EQUAL(0, ext.maxElements);
                TEST_ASSERT_EQUAL(0, ext.propertyDataType);
                break;
            }

            const auto pid = static_cast<uint8_t>(classic.propertyId);
            TEST_ASSERT_EQUAL(pid, ext.header.propertyId);
            TEST_ASSERT_EQUAL(index, classic.propertyIndex.value());
            TEST_ASSERT_EQUAL(index, ext.propertyIndex);
            TEST_ASSERT_EQUAL(static_cast<uint8_t>(classic.type), ext.propertyDataType);
            TEST_ASSERT_EQUAL(classic.maxElements, ext.maxElements);
            TEST_ASSERT_EQUAL(classic.writeAccess == knx::PropertyWriteAccess::Allowed, ext.writeEnabled);
            TEST_ASSERT_EQUAL(classic.readLevel, ext.readLevel);
            TEST_ASSERT_EQUAL(classic.writeLevel, ext.writeLevel);

            // One stable order: PID_OBJECT_TYPE first, then ascending PIDs.
            if (index == 0) {
                TEST_ASSERT_EQUAL(static_cast<uint8_t>(PropertyID::ObjectType), pid);
            } else {
                TEST_ASSERT_TRUE(pid > previousPid);
            }
            previousPid = pid;

            const auto classicById = f.classicDescribe(static_cast<uint8_t>(object), pid, 0);
            const auto extById = f.extDescribe(type, pid, 0);
            TEST_ASSERT_EQUAL(index, classicById.propertyIndex.value());
            TEST_ASSERT_EQUAL(index, extById.propertyIndex);
        }
        TEST_ASSERT_TRUE(index > 1);
    }
}

// 03/05/01 §4.18.6.2.4: the Group Object Table's PID_TABLE is
// PDT_GENERIC_02[], writeable, element n = descriptor of Group Object n,
// element 0 = length. Both families must describe and treat it alike.
void test_GroupObjectTableBehavesTheSameThroughBothFamilies(void) {
    Fixture f;
    addGroupObject(f);
    constexpr uint8_t kGotIndex = 3;

    // Description, by PID.
    const auto classic = f.classicDescribe(kGotIndex, static_cast<uint8_t>(kGotTable), 0);
    const auto ext = f.extDescribe(kGroupObjectTableType, static_cast<uint16_t>(kGotTable), 0);
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(PropertyDataType::Generic02), static_cast<uint8_t>(classic.type));
    TEST_ASSERT_EQUAL(static_cast<uint8_t>(PropertyDataType::Generic02), ext.propertyDataType);
    TEST_ASSERT_TRUE(classic.writeAccess == knx::PropertyWriteAccess::Allowed);
    TEST_ASSERT_TRUE(ext.writeEnabled);
    TEST_ASSERT_EQUAL(knx::objects::GroupObjectTableDomain::kMaxObjects, classic.maxElements);
    TEST_ASSERT_EQUAL(classic.maxElements, ext.maxElements);
    TEST_ASSERT_EQUAL(classic.propertyIndex.value(), ext.propertyIndex);

    // Element 0: the number of group objects, the same either way.
    PropertyValueReadRequest classicRead;
    classicRead.objectIndex = InterfaceObjectIndex(kGotIndex);
    classicRead.propertyId = kGotTable;
    classicRead.startIndex = 0;
    classicRead.elementCount = 1;
    TEST_ASSERT_TRUE(f.classic.handleValueRead(source(), classicRead).isOk());
    TEST_ASSERT_EQUAL(2, f.classicValue.data.size());
    TEST_ASSERT_EQUAL(1, f.classicValue.data[1]);
    TEST_ASSERT_TRUE(f.services.handleValueRead(source(),
                                                makeValueRequest(kGroupObjectTableType, 1, kGotTable, 1, 0))
                         .isOk());
    TEST_ASSERT_EQUAL(2, f.valueResponse.data.size());
    TEST_ASSERT_EQUAL(1, f.valueResponse.data[1]);

    // A classic write of element 1 is applied and read back by both.
    const uint16_t first = descriptorFor(true, false);
    PropertyValueWriteRequest classicWrite;
    classicWrite.objectIndex = InterfaceObjectIndex(kGotIndex);
    classicWrite.propertyId = kGotTable;
    classicWrite.startIndex = 1;
    classicWrite.elementCount = 1;
    classicWrite.data = {static_cast<uint8_t>(first >> 8), static_cast<uint8_t>(first & 0xFF)};
    TEST_ASSERT_TRUE(f.classic.handleValueWrite(source(), classicWrite).isOk());
    TEST_ASSERT_EQUAL(1, f.classicValue.elementCount);  // not the negative answer
    TEST_ASSERT_EQUAL(first, static_cast<uint16_t>((f.classicValue.data[0] << 8) | f.classicValue.data[1]));
    TEST_ASSERT_TRUE(f.services.handleValueRead(source(),
                                                makeValueRequest(kGroupObjectTableType, 1, kGotTable, 1, 1))
                         .isOk());
    TEST_ASSERT_EQUAL(first, static_cast<uint16_t>((f.valueResponse.data[0] << 8) | f.valueResponse.data[1]));

    // An extended write of element 1 is applied too.
    const uint16_t second = descriptorFor(false, true);
    auto extWrite = makeValueRequest(kGroupObjectTableType, 1, kGotTable, 1, 1);
    (void)extWrite.data.push_back(static_cast<uint8_t>(second >> 8));
    (void)extWrite.data.push_back(static_cast<uint8_t>(second & 0xFF));
    TEST_ASSERT_TRUE(f.services.handleValueWrite(source(), extWrite, true).isOk());
    TEST_ASSERT_EQUAL(0x00, static_cast<int>(f.valueResponse.returnCode));
    const auto* object = f.manager.groupObjectTable().getGroupObject(knx::GroupObjectIndex(0));
    TEST_ASSERT_NOT_NULL(object);
    TEST_ASSERT_FALSE(object->readable());
    TEST_ASSERT_TRUE(object->transmit());

    // The length is the firmware's: a write to element 0 (ETS clearing the
    // table) is accepted by both and not applied.
    const size_t objectsBefore = f.manager.groupObjectTable().objectCount();
    classicWrite.startIndex = 0;
    classicWrite.data = {0x00, 0x00};
    TEST_ASSERT_TRUE(f.classic.handleValueWrite(source(), classicWrite).isOk());
    TEST_ASSERT_EQUAL(objectsBefore, f.manager.groupObjectTable().objectCount());
    auto extClear = makeValueRequest(kGroupObjectTableType, 1, kGotTable, 1, 0);
    (void)extClear.data.push_back(0x00);
    (void)extClear.data.push_back(0x00);
    TEST_ASSERT_TRUE(f.services.handleValueWrite(source(), extClear, true).isOk());
    TEST_ASSERT_EQUAL(0x00, static_cast<int>(f.valueResponse.returnCode));
    TEST_ASSERT_EQUAL(objectsBefore, f.manager.groupObjectTable().objectCount());
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
    request.header.objectType = kGroupObjectTableType;
    request.header.objectInstance = 1;
    request.header.propertyId = 66;  // PID_GO_DIAGNOSTICS
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
    request.header.objectType = kGroupObjectTableType;
    request.header.objectInstance = 1;
    request.header.propertyId = 66;

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
    request.header.propertyId = 66;

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
    RUN_TEST(test_PropertyExt_InstanceZeroAndMissingInstancesAreAddressVoid);

    RUN_TEST(test_PropertyExtValue_ReadReturnsLiveData);
    RUN_TEST(test_PropertyExtValue_StartIndexZeroReturnsElementCount);
    RUN_TEST(test_PropertyExtValue_UnknownObjectAnswersAddressVoid);
    RUN_TEST(test_PropertyExtValue_UnknownPropertyAnswersAddressVoid);
    RUN_TEST(test_PropertyExtValue_WriteConReachesTheObjectAndConfirms);
    RUN_TEST(test_PropertyExtValue_WriteUnConNeverAnswers);
    RUN_TEST(test_PropertyExtValue_WriteToReadOnlyIsAccessReadOnly);
    RUN_TEST(test_PropertyExtValue_TruncatedWriteIsRejected);
    RUN_TEST(test_PropertyExtValue_ElementCountWriteIsNotSizedLikeAnElement);

    RUN_TEST(test_PropertyExtDescription_ReadByIdReportsAccessLimitsAndIndex);
    RUN_TEST(test_PropertyExtDescription_ReadOnlyPropertyIsNotWriteEnabled);
    RUN_TEST(test_PropertyExtDescription_IndexZeroIsObjectType);
    RUN_TEST(test_PropertyExtDescription_OtherTypeIsAnsweredAsTypeZero);
    RUN_TEST(test_PropertyExtDescription_MissingPropertyIsAllZero);

    RUN_TEST(test_ClassicAndExtendedDescriptionsAgreeOnEveryIndex);
    RUN_TEST(test_GroupObjectTableBehavesTheSameThroughBothFamilies);

    RUN_TEST(test_FunctionPropertyExt_CommandReachesProviderAndAnswers);
    RUN_TEST(test_FunctionPropertyExt_NoProviderIsTypeConflict);
    RUN_TEST(test_FunctionPropertyExt_UnknownObjectAnswersAddressVoid);

    return UNITY_END();
}
