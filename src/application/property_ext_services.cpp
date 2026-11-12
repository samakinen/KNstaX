// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property_ext_services.cpp
 * @brief KNX extended property services implementation
 *
 * 03/03/07 Application Layer v02.01.01 §3.4.3.2, §3.4.5, §3.4.8.
 */

#include "knx/application/property_ext_services.hpp"
#include "knx/config.hpp"
#include "knx/util/log.hpp"

#include <array>

namespace knx {
namespace application {

namespace {
constexpr const char* TAG = "KNX.App.PropExt";

/// The classic PropertyID is one octet; the extended services carry 12 bits.
/// No interface object here has a PID or a property index above 255, so
/// anything larger does not exist.
constexpr uint16_t kMaxPropertyId = 0xFFu;
constexpr uint16_t kMaxPropertyIndex = 0xFFu;
} // namespace

std::optional<InterfaceObjectIndex> PropertyExtServices::resolveObject(uint16_t objectType,
                                                                      uint16_t objectInstance) const
{
    // Object instance is 1-based (03/05/01 §4.18.5.2.5); 0 does not exist.
    if (objectInstance == 0 || !_objectResolver) {
        return std::nullopt;
    }
    return _objectResolver(objectType, objectInstance);
}

std::optional<PropertyDescriptionInfo> PropertyExtServices::describe(const IndividualAddress& source,
                                                                     InterfaceObjectIndex objectIndex,
                                                                     uint16_t propertyId,
                                                                     uint16_t propertyIndex) const
{
    if (!_descriptionProvider || propertyId > kMaxPropertyId
            || (propertyId == 0 && propertyIndex > kMaxPropertyIndex)) {
        return std::nullopt;
    }
    return _descriptionProvider(source,
                                objectIndex,
                                static_cast<PropertyID>(propertyId),
                                PropertyIndex(static_cast<uint8_t>(propertyId == 0 ? propertyIndex : 0)));
}

void PropertyExtServices::sendValueFailure(const IndividualAddress& destination,
                                           APCIService service,
                                           const PropertyExtValueRequest& request,
                                           KnxReturnCode returnCode)
{
    // §3.4.5.1 / §3.4.5.2: a negative answer sets nr_of_elem to zero, echoes
    // the requested start_index, and carries the error as a single octet.
    PropertyExtValueResponse response;
    response.header = request.header;
    response.elementCount = 0;
    response.startIndex = request.startIndex;
    response.returnCode = returnCode;

    if (service == APCIService::PropertyExtValueResponse) {
        (void)response.data.push_back(static_cast<uint8_t>(returnCode));
    }

    if (_valueResponseCallback) {
        _valueResponseCallback(destination, service, response);
    }
}

util::Result<void> PropertyExtServices::handleValueRead(const IndividualAddress& source,
                                                        const PropertyExtValueRequest& request)
{
    constexpr auto kService = APCIService::PropertyExtValueResponse;

    if (!request.header.fieldsInRange()) {
        sendValueFailure(source, kService, request, KnxReturnCode::AddressVoid);
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const auto objectIndex = resolveObject(request.header.objectType, request.header.objectInstance);
    if (!objectIndex.has_value()) {
        sendValueFailure(source, kService, request, KnxReturnCode::AddressVoid);
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    if (_accessCheck && !_accessCheck(request.header.objectType, request.header.propertyId, false)) {
        KNX_LOGW(TAG, "Ext property read denied by access policy: src=0x%04X ot=%u pid=%u",
                 source.raw, request.header.objectType, request.header.propertyId);
        sendValueFailure(source, kService, request, KnxReturnCode::AccessDenied);
        return util::Result<void>::err(util::ErrorCode::AccessDenied);
    }

    // PID 0 is not a property; it only selects "by index" in a description.
    const auto description = request.header.propertyId != 0
                                 ? describe(source, *objectIndex, request.header.propertyId, 0)
                                 : std::nullopt;
    if (!description.has_value()) {
        sendValueFailure(source, kService, request, KnxReturnCode::AddressVoid);
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    // §3.4.5.5: E_ACCESS_WRITE_ONLY for a read of a write-only property.
    if (!description->readable) {
        sendValueFailure(source, kService, request, KnxReturnCode::AccessWriteOnly);
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }

    // §3.4.5.1: start_index 0 asks for the *current* element count, answered
    // as a 2-octet value with nr_of_elem forced to 1 however many were
    // requested. Otherwise nr_of_elem 0 reads nothing and is not a request.
    if (request.startIndex != 0 && request.elementCount == 0) {
        sendValueFailure(source, kService, request, KnxReturnCode::DataVoid);
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    PropertyValueReadRequest classicRequest;
    classicRequest.objectIndex = *objectIndex;
    classicRequest.propertyId = description->propertyId;
    classicRequest.startIndex = request.startIndex;
    classicRequest.elementCount = request.startIndex == 0 ? uint8_t{1} : request.elementCount;

    std::array<uint8_t, config::MAX_APDU_LENGTH> buffer{};
    const auto provided = _readProvider ? _readProvider(source, classicRequest, buffer)
                                        : std::optional<util::Result<size_t>>{};
    if (!provided.has_value()) {
        // The property exists, so what is missing is the addressed element
        // range: E_ADDRESS_VOID covers "index out of range".
        sendValueFailure(source, kService, request, KnxReturnCode::AddressVoid);
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }
    if (provided->isError()) {
        sendValueFailure(source, kService, request, KnxReturnCode::DataVoid);
        return provided->error();
    }
    if (provided->value() > PropertyExtValueResponse::DataBuffer::capacity()) {
        sendValueFailure(source, kService, request, KnxReturnCode::ExceedsMaxApduLength);
        return util::Result<void>::err(util::ErrorCode::BufferTooSmall);
    }

    PropertyExtValueResponse response;
    response.header = request.header;
    response.startIndex = request.startIndex;
    response.elementCount = classicRequest.elementCount;
    for (size_t i = 0; i < provided->value(); ++i) {
        (void)response.data.push_back(buffer[i]);
    }
    if (_valueResponseCallback) {
        _valueResponseCallback(source, kService, response);
    }

    KNX_LOGD(TAG, "Ext read OT=%u OI=%u PID=%u -> %zu bytes",
             request.header.objectType, request.header.objectInstance,
             request.header.propertyId, provided->value());
    return util::Result<void>::ok();
}

util::Result<void> PropertyExtServices::handleValueWrite(const IndividualAddress& source,
                                                         const PropertyExtValueRequest& request,
                                                         bool confirmed)
{
    constexpr auto kService = APCIService::PropertyExtValueWriteConRes;

    // _WriteUnCon is unconfirmed: perform the write but never answer, not even
    // on failure.  Routing failures through sendValueFailure would put an
    // unexpected response on the bus.
    const auto fail = [&](KnxReturnCode code, util::ErrorCode error) {
        if (confirmed) {
            sendValueFailure(source, kService, request, code);
        }
        return util::Result<void>::err(error);
    };

    if (!request.header.fieldsInRange()) {
        return fail(KnxReturnCode::AddressVoid, util::ErrorCode::InvalidParameter);
    }

    const auto objectIndex = resolveObject(request.header.objectType, request.header.objectInstance);
    if (!objectIndex.has_value()) {
        return fail(KnxReturnCode::AddressVoid, util::ErrorCode::InvalidAddress);
    }

    if (_accessCheck && !_accessCheck(request.header.objectType, request.header.propertyId, true)) {
        KNX_LOGW(TAG, "Ext property write denied by access policy: src=0x%04X ot=%u pid=%u",
                 source.raw, request.header.objectType, request.header.propertyId);
        return fail(KnxReturnCode::AccessDenied, util::ErrorCode::AccessDenied);
    }

    const auto description = request.header.propertyId != 0
                                 ? describe(source, *objectIndex, request.header.propertyId, 0)
                                 : std::nullopt;
    if (!description.has_value()) {
        return fail(KnxReturnCode::AddressVoid, util::ErrorCode::InvalidAddress);
    }

    // §3.4.5.5 maps refusals onto the shared Error Code Set. A write to a
    // read-only property is E_ACCESS_READ_ONLY, not the authorisation-flavoured
    // E_ACCESS_DENIED — the two mean different things to the tool.
    if (description->writeAccess != PropertyWriteAccess::Allowed) {
        return fail(KnxReturnCode::AccessReadOnly, util::ErrorCode::OperationNotSupported);
    }

    // 03/03/07 §3.5.7: refused when the current access level is below the
    // description's write level. E_ACCESS_DENIED is the A_Authorize refusal.
    if (_accessLevel && _accessLevel(source) > description->writeLevel) {
        KNX_LOGW(TAG, "Ext property write refused: src=0x%04X ot=%u pid=%u needs level %u",
                 source.raw, request.header.objectType, request.header.propertyId,
                 static_cast<unsigned>(description->writeLevel));
        return fail(KnxReturnCode::AccessDenied, util::ErrorCode::AccessDenied);
    }

    if (request.elementCount == 0) {
        return fail(KnxReturnCode::DataVoid, util::ErrorCode::InvalidParameter);
    }

    // A write to array element 0 sets the current number of elements, so its
    // payload is two octets whatever the element size is — sizing it like an
    // element write is what made ETS's "clear this table" write
    // (nr_of_elem = 1, start_index = 0, data = 0000) come back as
    // E_LENGTH_EXCEEDS_MAX_APDU_LENGTH.
    if (request.startIndex == 0) {
        if (request.data.size() != 2u) {
            return fail(KnxReturnCode::DataVoid, util::ErrorCode::InvalidParameter);
        }
    } else {
        // A declared element count that outruns the octets actually sent is a
        // truncated APDU, not a data-content problem.
        const size_t elementSize = propertyDataTypeSize(description->type);
        if (elementSize != 0 && request.data.size() < elementSize * request.elementCount) {
            return fail(KnxReturnCode::ExceedsMaxApduLength, util::ErrorCode::InvalidParameter);
        }
    }

    if (!_writeProvider) {
        return fail(KnxReturnCode::TemporarilyNotAvailable, util::ErrorCode::OperationNotReady);
    }

    PropertyValueWriteRequest classicRequest;
    classicRequest.objectIndex = *objectIndex;
    classicRequest.propertyId = description->propertyId;
    classicRequest.elementCount = request.elementCount;
    classicRequest.startIndex = request.startIndex;
    if (!classicRequest.data.assign(request.data.span())) {
        return fail(KnxReturnCode::ExceedsMaxApduLength, util::ErrorCode::BufferTooSmall);
    }

    const auto written = _writeProvider(source, classicRequest);
    if (written.isError()) {
        return fail(KnxReturnCode::DataVoid, written.error());
    }

    if (confirmed) {
        PropertyExtValueResponse response;
        response.header = request.header;
        response.elementCount = request.elementCount;
        response.startIndex = request.startIndex;
        response.returnCode = KnxReturnCode::Success;
        if (_valueResponseCallback) {
            _valueResponseCallback(source, kService, response);
        }
    }

    KNX_LOGD(TAG, "Ext write OT=%u OI=%u PID=%u (%u elems, confirmed=%d)",
             request.header.objectType, request.header.objectInstance,
             request.header.propertyId, request.elementCount, confirmed ? 1 : 0);
    return util::Result<void>::ok();
}

util::Result<void> PropertyExtServices::handleDescriptionRead(
    const IndividualAddress& source,
    const PropertyExtDescriptionRequest& request)
{
    // §3.4.3.2: "If another Property Description Type than zero is requested,
    // then the MaS shall answer with the A_PropertyExtDescription_Response-PDU
    // of the Property Description Type zero." So the type is always zero, and
    // on a problem "the fields Property Description Type, DPT, writeable, PDT,
    // max_nr_of_elem, and access […] shall be zero" — the defaults.
    PropertyExtDescriptionResponse response;
    response.header = request.header;
    response.descriptionType = 0;
    response.propertyIndex = request.propertyIndex;

    const auto respond = [&]() {
        if (_descriptionResponseCallback) {
            _descriptionResponseCallback(source, response);
        }
    };

    if (!request.header.fieldsInRange()) {
        respond();
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    const auto objectIndex = resolveObject(request.header.objectType, request.header.objectInstance);
    if (!objectIndex.has_value()) {
        respond();
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    // Same reading as the classic service: a description is metadata, so only
    // the form that names a PID outright is gated (03/4/1 §6.2.6.3.4).
    if (request.header.propertyId != 0 && _accessCheck
            && !_accessCheck(request.header.objectType, request.header.propertyId, false)) {
        KNX_LOGW(TAG, "Ext property description denied by access policy: src=0x%04X ot=%u pid=%u",
                 source.raw, request.header.objectType, request.header.propertyId);
        respond();
        return util::Result<void>::err(util::ErrorCode::AccessDenied);
    }

    // §3.4.3.2: a zero Property Identifier selects by Property Index ("Property
    // Index = 0 shall mean the first Property"), otherwise the index "shall be
    // ignored". The index order is the interface objects' own, which is also
    // what the classic A_PropertyDescription_Read reports.
    const auto description = describe(source, *objectIndex, request.header.propertyId,
                                       request.propertyIndex);
    if (!description.has_value()) {
        respond();
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    // "The Property Index in the A_PropertyExtDescription_Response-PDU shall
    // be the correct value of the Property Index of the addressed Property."
    response.header.propertyId = static_cast<uint16_t>(description->propertyId);
    response.propertyIndex = description->propertyIndex.value();
    response.writeEnabled = description->writeAccess == PropertyWriteAccess::Allowed;
    response.propertyDataType = static_cast<uint8_t>(description->type) & 0x3Fu;
    response.maxElements = description->maxElements;
    response.readLevel = description->readLevel & 0x0Fu;
    response.writeLevel = description->writeLevel & 0x0Fu;
    // "If no DPT is available, both DPT fields shall be set to zero."
    response.dptMain = 0;
    response.dptSub = 0;

    respond();
    return util::Result<void>::ok();
}

util::Result<void> PropertyExtServices::handleFunctionProperty(
    const IndividualAddress& source,
    const FunctionPropertyExtRequest& request,
    FunctionPropertyInvocation invocation)
{
    FunctionPropertyExtResponse response;
    response.header = request.header;

    const auto respond = [&](KnxReturnCode code) {
        response.returnCode = code;
        if (_functionResponseCallback) {
            _functionResponseCallback(source, response);
        }
    };

    if (!request.header.fieldsInRange()) {
        respond(KnxReturnCode::AddressVoid);
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    if (!resolveObject(request.header.objectType, request.header.objectInstance).has_value()) {
        respond(KnxReturnCode::AddressVoid);
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    if (!_functionProvider) {
        respond(KnxReturnCode::DataTypeConflict);
        return util::Result<void>::err(util::ErrorCode::OperationNotReady);
    }

    const auto result = _functionProvider(source, request.header, invocation, request.security,
                                          request.data.span(), response.data);
    if (!result.has_value()) {
        // No such function property: §3.4.8 uses E_DATA_TYPE_CONFLICT for a
        // property that is not PDT_FUNCTION / PDT_CONTROL.
        response.data.clear();
        respond(KnxReturnCode::DataTypeConflict);
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    respond(*result);
    return util::Result<void>::ok();
}

} // namespace application
} // namespace knx
