// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property_services.cpp
 * @brief Property services implementation
 */

#include "knx/application/property_services.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"

static const char* TAG = "KNX.App.PropSvc";

namespace knx {
namespace application {

void PropertyServices::setValueResponseCallback(PropertyValueResponseCallback callback) {
    _valueResponseCallback = callback;
}

void PropertyServices::setDescriptionResponseCallback(PropertyDescriptionResponseCallback callback) {
    _descriptionResponseCallback = callback;
}

void PropertyServices::setReadProvider(PropertyReadProvider provider) {
    _readProvider = provider;
}

void PropertyServices::setWriteProvider(PropertyWriteProvider provider) {
    _writeProvider = provider;
}

void PropertyServices::setDescriptionProvider(PropertyDescriptionProvider provider) {
    _descriptionProvider = provider;
}

void PropertyServices::setAccessCheck(PropertyAccessCheck check) {
    _accessCheck = std::move(check);
}

util::Result<void> PropertyServices::handleValueRead(
    const IndividualAddress& source,
    const PropertyValueReadRequest& request
) {
    if (!_valueResponseCallback) {
        KNX_LOGW(TAG, "No value response callback set");
        return util::Result<void>::err(util::ErrorCode::OperationNotReady);
    }

    // KNX 03.03.07: a read of a non-existent or unreadable property is
    // answered with a PropertyValue_Response carrying NumberOfElements = 0
    // and no data — never with silence, which would make the client (ETS)
    // retry into a timeout and abort the whole download.
    const auto sendNegativeResponse = [&]() {
        PropertyValueResponse response;
        response.objectIndex = request.objectIndex;
        response.propertyId = request.propertyId;
        response.elementCount = 0;
        response.startIndex = request.startIndex;
        _valueResponseCallback(source, response);
    };

    if (_accessCheck && !_accessCheck(request.objectIndex, request.propertyId, false)) {
        KNX_LOGW(TAG, "Property read denied by access policy: src=0x%04X obj=%d prop=%d",
                 source.raw, request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
        sendNegativeResponse();
        return util::Result<void>::err(util::ErrorCode::AccessDenied);
    }

    // 03/03/07 §3.4.4.1: a read with start_index = 0 asks for the current
    // number of elements and "shall respond […] with start_index = 0 and
    // nr_of_elem = 1", however many elements the client happened to request.
    const uint8_t responseElementCount =
        (request.startIndex == 0) ? uint8_t{1} : request.elementCount;

    // Optional backend override (live data)
    if (_readProvider) {
        PropertyValueResponse::DataBuffer backendData;
        backendData.resize(backendData.capacity());
        auto providerRes = _readProvider(source, request, backendData.span());
        if (providerRes) {
            if (providerRes->isError()) {
                KNX_LOGW(TAG, "Property read provider failed: obj=%d, prop=%d",
                         request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
                sendNegativeResponse();
                return util::Result<void>::err(providerRes->error());
            }

            const size_t bytesRead = providerRes->value();
            if (bytesRead > backendData.capacity()) {
                KNX_LOGW(TAG, "Property read provider overflow: obj=%d, prop=%d, bytes=%zu",
                         request.objectIndex.value(), static_cast<uint8_t>(request.propertyId), bytesRead);
                return util::Result<void>::err(util::ErrorCode::BufferTooSmall);
            }

            backendData.resize(bytesRead);
            PropertyValueResponse response;
            response.objectIndex = request.objectIndex;
            response.propertyId = request.propertyId;
            response.elementCount = responseElementCount;
            response.startIndex = request.startIndex;
            response.data = backendData;
            _valueResponseCallback(source, response);
            return util::Result<void>::ok();
        }
    }
    
    // Nothing answered: the object or property does not exist, or it cannot
    // be read.
    KNX_LOGW(TAG, "Property not readable: obj=%d, prop=%d",
             request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
    sendNegativeResponse();
    return util::Result<void>::err(util::ErrorCode::OperationFailed);
}

util::Result<void> PropertyServices::handleValueWrite(
    const IndividualAddress& source,
    const PropertyValueWriteRequest& request
) {
    // KNX 03.03.07: a rejected PropertyValue_Write is answered with a
    // PropertyValue_Response carrying NumberOfElements = 0 and no data —
    // never with silence, which would make the client (ETS) retry into a
    // timeout and abort the whole download.
    const auto sendNegativeResponse = [&]() {
        if (!_valueResponseCallback) {
            return;
        }
        PropertyValueResponse response;
        response.objectIndex = request.objectIndex;
        response.propertyId = request.propertyId;
        response.elementCount = 0;
        response.startIndex = request.startIndex;
        _valueResponseCallback(source, response);
    };

    // KNX 03.03.07: the response to A_PropertyValue_Write carries the CURRENT
    // value of the property after the write — not an echo of the request.
    // ETS SystemB relies on this for PID_LOAD_STATE_CONTROL: after writing a
    // load event it checks the returned load state (StartLoading must yield
    // Loading) and aborts the download with "internal device error" when the
    // echoed event byte comes back instead. Falls back to echoing the request
    // only when the property cannot be read back (write-only properties).
    const auto sendWriteConfirmation = [&]() {
        if (!_valueResponseCallback) {
            return;
        }
        PropertyValueResponse response;
        response.objectIndex = request.objectIndex;
        response.propertyId = request.propertyId;
        response.elementCount = request.elementCount;
        response.startIndex = request.startIndex;
        response.data = request.data;  // fallback for write-only properties

        PropertyValueReadRequest readBack;
        readBack.objectIndex = request.objectIndex;
        readBack.propertyId = request.propertyId;
        readBack.elementCount = request.elementCount;
        readBack.startIndex = request.startIndex;

        PropertyValueResponse::DataBuffer readData;
        bool haveReadBack = false;
        if (_readProvider) {
            readData.resize(readData.capacity());
            auto readRes = _readProvider(source, readBack, readData.span());
            if (readRes && readRes->isOk() && readRes->value() <= readData.capacity()) {
                readData.resize(readRes->value());
                haveReadBack = true;
            }
        }
        if (!haveReadBack && request.elementCount == 0 && request.startIndex == 1
                && _descriptionProvider) {
            // Reading back zero elements from the start of an existing
            // fixed-size property yields no data, so the confirmation carries
            // none rather than the echo.
            const auto description = _descriptionProvider(source, request.objectIndex,
                                                          request.propertyId, PropertyIndex(0));
            if (description.has_value() && description->maxElements != 0) {
                PropertyDescriptor element{};
                element.type = description->type;
                if (element.getElementSize() != 0) {
                    readData.clear();
                    haveReadBack = true;
                }
            }
        }
        if (haveReadBack) {
            response.data = readData;
        }
        _valueResponseCallback(source, response);
    };

    if (_accessCheck && !_accessCheck(request.objectIndex, request.propertyId, true)) {
        KNX_LOGW(TAG, "Property write denied by access policy: src=0x%04X obj=%d prop=%d",
                 source.raw, request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
        sendNegativeResponse();
        return util::Result<void>::err(util::ErrorCode::AccessDenied);
    }

    // The write level is the one this property's description advertises, so
    // a client can learn in advance what it needs to authorise for.
    if (_accessLevel && _descriptionProvider) {
        const auto descriptor = _descriptionProvider(source, request.objectIndex,
                                                     request.propertyId, PropertyIndex(0));
        if (descriptor.has_value() && _accessLevel(source) > descriptor->writeLevel) {
            KNX_LOGW(TAG, "Property write refused: src=0x%04X obj=%d prop=%d needs level %u",
                     source.raw, request.objectIndex.value(),
                     static_cast<uint8_t>(request.propertyId),
                     static_cast<unsigned>(descriptor->writeLevel));
            sendNegativeResponse();
            return util::Result<void>::err(util::ErrorCode::AccessDenied);
        }
    }

    if (!_writeProvider) {
        KNX_LOGW(TAG, "No property write provider: obj=%d, prop=%d",
                 request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
        sendNegativeResponse();
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }

    auto providerRes = _writeProvider(source, request);
    if (providerRes.isError()) {
        KNX_LOGW(TAG, "Property write rejected by provider: obj=%d, prop=%d",
                 request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
        sendNegativeResponse();
        return providerRes.error();
    }

    sendWriteConfirmation();

    KNX_LOGD(TAG, "Property value written: obj=%d, prop=%d, bytes=%zu",
             request.objectIndex.value(), static_cast<uint8_t>(request.propertyId), request.data.size());

    return util::Result<void>::ok();
}

util::Result<void> PropertyServices::handleDescriptionRead(
    const IndividualAddress& source,
    InterfaceObjectIndex objectIndex,
    PropertyID propertyId,
    PropertyIndex propertyIndex
) {
    if (!_descriptionResponseCallback) {
        KNX_LOGW(TAG, "No description response callback set");
        return util::Result<void>::err(util::ErrorCode::OperationNotReady);
    }

    // 03/4/1 §6.2.6.3.4: the Permission for a description read is the OR of the
    // Permissions for the value's read and write. The read side is the weaker
    // of the two everywhere this stack restricts anything, so asking about it
    // is asking about the OR.
    //
    // Only the by-PID form is gated. A description carries no property value —
    // it is datatype, element count and access levels — so the index-addressed
    // enumeration that ETS uses to discover an object leaks nothing, and
    // refusing it would break that discovery for no gain.
    const bool addressedByPid = static_cast<uint8_t>(propertyId) != 0;
    if (addressedByPid && _accessCheck && !_accessCheck(objectIndex, propertyId, false)) {
        KNX_LOGW(TAG, "Property description denied by access policy: src=0x%04X obj=%d prop=%d",
                 source.raw, objectIndex.value(), static_cast<uint8_t>(propertyId));
        _descriptionResponseCallback(source,
                                     objectIndex,
                                     propertyId,
                                     propertyIndex,
                                     PropertyWriteAccess::Denied,
                                     PropertyDataType::GenericData,
                                     0,
                                     0,
                                     0);
        return util::Result<void>::err(util::ErrorCode::AccessDenied);
    }

    // The interface objects describe themselves.
    if (_descriptionProvider) {
        auto info = _descriptionProvider(source, objectIndex, propertyId, propertyIndex);
        if (info) {
            _descriptionResponseCallback(
                source,
                info->objectIndex,
                info->propertyId,
                info->propertyIndex,
                info->writeAccess,
                info->type,
                info->maxElements,
                info->readLevel,
                info->writeLevel);
            return util::Result<void>::ok();
        }
    }
    
    // KNX 03.03.07: A_PropertyDescription_Read is ALWAYS answered. When the
    // object or property does not exist, the response echoes the requested
    // indices with max_nr_of_elem = 0 (type and access are meaningless then).
    // Staying silent would make the client retry and eventually abort, e.g.
    // ETS aborts the whole download after two unanswered description reads.
    KNX_LOGW(TAG, "Property description not found: obj=%d, prop=%d, index=%d",
             objectIndex.value(), static_cast<int>(propertyId), propertyIndex.value());
    _descriptionResponseCallback(
        source,
        objectIndex,
        propertyId,
        propertyIndex,
        PropertyWriteAccess::Denied,
        static_cast<PropertyDataType>(0),
        0u,   // maxElements = 0 → "property does not exist"
        0u,
        0u);
    return util::Result<void>::ok();
}

} // namespace application
} // namespace knx
