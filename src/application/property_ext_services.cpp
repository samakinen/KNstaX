// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property_ext_services.cpp
 * @brief KNX extended property services implementation
 *
 * 03/03/07 Application Layer v02.01.01 §3.4.3.2, §3.4.5, §3.4.8.
 */

#include "knx/application/property_ext_services.hpp"
#include "knx/objects/interface_object.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/log.hpp"

#include <algorithm>
#include <array>

namespace knx {
namespace application {

namespace {
constexpr const char* TAG = "KNX.App.PropExt";

/// Upper bound for the bounded object-index enumeration.  The stack registers
/// far fewer than this; it exists so resolveObject() never heap-allocates.
constexpr size_t kMaxInterfaceObjects = 32u;

/// §3.4.5.5 maps refusals onto the shared Error Code Set.  A write to a
/// read-only property is E_ACCESS_READ_ONLY, not the authorisation-flavoured
/// E_ACCESS_DENIED — the two mean different things to the tool.
constexpr KnxReturnCode accessRefusalFor(PropertyAccess access, AccessType requested)
{
    if (requested == AccessType::Write && access == PropertyAccess::ReadOnly) {
        return KnxReturnCode::AccessReadOnly;
    }
    if (requested == AccessType::Read && access == PropertyAccess::WriteOnly) {
        return KnxReturnCode::AccessWriteOnly;
    }
    return KnxReturnCode::AccessDenied;
}
} // namespace

PropertyExtServices::PropertyExtServices(PropertyStoreManager& storeManager)
    : _storeManager(storeManager)
{
}

std::optional<InterfaceObjectIndex> PropertyExtServices::resolveObject(uint16_t objectType,
                                                                      uint16_t objectInstance) const
{
    // Object instance is 1-based and counts the objects of that type in index
    // order (03/05/01).  Instance 0 does not exist.
    if (objectInstance == 0) {
        return std::nullopt;
    }

    // Query-then-fill, the same bounded pattern PropertyServices uses.
    const auto neededRes = _storeManager.getAllObjectIndices(std::span<InterfaceObjectIndex>{});
    if (neededRes.isError()) {
        return std::nullopt;
    }
    util::FixedVector<InterfaceObjectIndex, kMaxInterfaceObjects> indices;
    if (neededRes.value() > indices.capacity()) {
        KNX_LOGW(TAG, "Interface object count %zu exceeds bounded metadata", neededRes.value());
        return std::nullopt;
    }
    indices.resize(neededRes.value());
    if (_storeManager.getAllObjectIndices(indices.span()).isError()) {
        return std::nullopt;
    }

    uint16_t seen = 0;
    for (size_t i = 0; i < indices.size(); ++i) {
        const auto* store = _storeManager.getObject(indices[i]);
        if (store == nullptr) {
            continue;
        }
        if (store->getObjectType().value() != objectType) {
            continue;
        }
        if (++seen == objectInstance) {
            return indices[i];
        }
    }

    return std::nullopt;
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

    const auto* store = _storeManager.getObject(*objectIndex);
    const auto descriptor = store != nullptr
                                ? store->getDescriptor(static_cast<PropertyID>(request.header.propertyId))
                                : std::nullopt;
    if (!descriptor.has_value()) {
        sendValueFailure(source, kService, request, KnxReturnCode::AddressVoid);
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    if (descriptor->access == PropertyAccess::WriteOnly) {
        sendValueFailure(source, kService, request,
                         accessRefusalFor(descriptor->access, AccessType::Read));
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }

    PropertyExtValueResponse response;
    response.header = request.header;
    response.startIndex = request.startIndex;

    // §3.4.5.1: start_index 0 asks for the *current* element count, answered as
    // a 2-octet value with nr_of_elem forced to 1 however many were requested.
    // The interface objects own that number — for a table property it is the
    // downloaded length, not the capacity — so ask them first and fall back to
    // the descriptor only for stores that cannot answer.
    if (request.startIndex == 0) {
        response.elementCount = 1;
        uint16_t currentElements = descriptor->maxElements;
        if (_readProvider) {
            PropertyValueReadRequest countRequest;
            countRequest.objectIndex = *objectIndex;
            countRequest.propertyId = static_cast<PropertyID>(request.header.propertyId);
            countRequest.elementCount = 1;
            countRequest.startIndex = 0;
            std::array<uint8_t, config::MAX_APDU_LENGTH> countBuffer{};
            if (auto provided = _readProvider(source, countRequest, countBuffer);
                provided.has_value() && provided->isOk() && provided->value() == 2u) {
                currentElements = static_cast<uint16_t>((countBuffer[0] << 8) | countBuffer[1]);
            }
        }
        (void)response.data.push_back(static_cast<uint8_t>((currentElements >> 8) & 0xFF));
        (void)response.data.push_back(static_cast<uint8_t>(currentElements & 0xFF));
        if (_valueResponseCallback) {
            _valueResponseCallback(source, kService, response);
        }
        return util::Result<void>::ok();
    }

    if (request.elementCount == 0) {
        sendValueFailure(source, kService, request, KnxReturnCode::DataVoid);
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    std::array<uint8_t, config::MAX_APDU_LENGTH> buffer{};
    if (_readProvider) {
        PropertyValueReadRequest classicRequest;
        classicRequest.objectIndex = *objectIndex;
        classicRequest.propertyId = static_cast<PropertyID>(request.header.propertyId);
        classicRequest.elementCount = static_cast<uint8_t>(request.elementCount);
        classicRequest.startIndex = request.startIndex;

        if (auto provided = _readProvider(source, classicRequest, buffer); provided.has_value()) {
            if (provided->isError()) {
                sendValueFailure(source, kService, request, KnxReturnCode::DataVoid);
                return provided->error();
            }
            response.elementCount = request.elementCount;
            for (size_t i = 0; i < provided->value(); ++i) {
                (void)response.data.push_back(buffer[i]);
            }
            if (_valueResponseCallback) {
                _valueResponseCallback(source, kService, response);
            }
            return util::Result<void>::ok();
        }
    }

    const auto read = _storeManager.readProperty(*objectIndex,
                                                 static_cast<PropertyID>(request.header.propertyId),
                                                 request.startIndex,
                                                 request.elementCount,
                                                 buffer);
    if (read.isError()) {
        sendValueFailure(source, kService, request, KnxReturnCode::AddressVoid);
        return read.error();
    }

    if (read.value() > PropertyExtValueResponse::DataBuffer::capacity()) {
        sendValueFailure(source, kService, request, KnxReturnCode::ExceedsMaxApduLength);
        return util::Result<void>::err(util::ErrorCode::BufferTooSmall);
    }

    response.elementCount = request.elementCount;
    for (size_t i = 0; i < read.value(); ++i) {
        (void)response.data.push_back(buffer[i]);
    }

    if (_valueResponseCallback) {
        _valueResponseCallback(source, kService, response);
    }

    KNX_LOGD(TAG, "Ext read OT=%u OI=%u PID=%u -> %zu bytes",
             request.header.objectType, request.header.objectInstance,
             request.header.propertyId, read.value());
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

    const auto* store = _storeManager.getObject(*objectIndex);
    const auto descriptor = store != nullptr
                                ? store->getDescriptor(static_cast<PropertyID>(request.header.propertyId))
                                : std::nullopt;
    if (!descriptor.has_value()) {
        return fail(KnxReturnCode::AddressVoid, util::ErrorCode::InvalidAddress);
    }

    if (descriptor->access == PropertyAccess::ReadOnly) {
        return fail(accessRefusalFor(descriptor->access, AccessType::Write),
                    util::ErrorCode::OperationNotSupported);
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
        const size_t elementSize = descriptor->getElementSize();
        if (elementSize != 0 && request.data.size() < elementSize * request.elementCount) {
            return fail(KnxReturnCode::ExceedsMaxApduLength, util::ErrorCode::InvalidParameter);
        }
    }

    if (_writeProvider) {
        PropertyValueWriteRequest classicRequest;
        classicRequest.objectIndex = *objectIndex;
        classicRequest.propertyId = static_cast<PropertyID>(request.header.propertyId);
        classicRequest.elementCount = static_cast<uint8_t>(request.elementCount);
        classicRequest.startIndex = request.startIndex;
        if (!classicRequest.data.assign(request.data.span())) {
            return fail(KnxReturnCode::ExceedsMaxApduLength, util::ErrorCode::BufferTooSmall);
        }

        const auto provided = _writeProvider(source, classicRequest);
        if (provided.isError()) {
            return fail(KnxReturnCode::DataVoid, provided.error());
        }
    } else {
        const auto written = _storeManager.writeProperty(*objectIndex,
                                                         static_cast<PropertyID>(request.header.propertyId),
                                                         request.startIndex,
                                                         request.elementCount,
                                                         request.data.span());
        if (written.isError()) {
            return fail(KnxReturnCode::DataVoid, written.error());
        }
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
    PropertyExtDescriptionResponse response;
    response.header = request.header;
    response.descriptionType = request.descriptionType;
    response.propertyIndex = request.propertyIndex;

    const auto respond = [&]() {
        if (_descriptionResponseCallback) {
            _descriptionResponseCallback(source, response);
        }
    };

    // Only description type zero is defined; anything else gets the all-zero
    // answer §3.4.3.2 prescribes rather than a silent drop.
    if (request.descriptionType != 0 || !request.header.fieldsInRange()) {
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

    const auto* store = _storeManager.getObject(*objectIndex);
    if (store == nullptr) {
        respond();
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    // §3.4.3.2: a zero Property Identifier selects by index instead of by ID.
    PropertyID propertyId = static_cast<PropertyID>(request.header.propertyId);
    if (request.header.propertyId == 0) {
        // propertyIndex is 0-based here: index 0 selects the object's first
        // property, which is PID_OBJECT_TYPE by construction.
        const auto neededRes = store->getAllPropertyIds(std::span<PropertyID>{});
        if (neededRes.isError()) {
            respond();
            return util::Result<void>::err(util::ErrorCode::InvalidAddress);
        }
        util::FixedVector<PropertyID, objects::InterfaceObject::kMaxPropertyRegistrations> ids;
        if (neededRes.value() > ids.capacity() || request.propertyIndex >= neededRes.value()) {
            respond();
            return util::Result<void>::err(util::ErrorCode::InvalidAddress);
        }
        ids.resize(neededRes.value());
        if (store->getAllPropertyIds(ids.span()).isError()) {
            respond();
            return util::Result<void>::err(util::ErrorCode::InvalidAddress);
        }
        propertyId = ids[request.propertyIndex];
        response.header.propertyId = static_cast<uint16_t>(propertyId);
    }

    const auto descriptor = store->getDescriptor(propertyId);
    if (!descriptor.has_value()) {
        respond();
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }

    response.writeEnabled = descriptor->access != PropertyAccess::ReadOnly;
    response.propertyDataType = static_cast<uint8_t>(descriptor->type) & 0x3Fu;
    response.maxElements = descriptor->maxElements;
    response.readLevel = descriptor->readLevel & 0x0Fu;
    response.writeLevel = descriptor->writeLevel & 0x0Fu;
    // DPT main/sub are not tracked per property by the store; zero means
    // "not specified", which is what the reserved encoding calls for.
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
