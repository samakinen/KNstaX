// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file property_services.cpp
 * @brief Property services implementation
 */

#include "knx/application/property_services.hpp"
#include "knx/objects/interface_object.hpp"
#include "knx/util/log.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/util/result.hpp"

static const char* TAG = "KNX.App.PropSvc";

namespace knx {
namespace application {

PropertyServices::PropertyServices(PropertyStoreManager& storeManager)
    : _storeManager(storeManager)
    , _valueResponseCallback(nullptr)
    , _descriptionResponseCallback(nullptr)
    , _readProvider(nullptr)
    , _writeConsumer(nullptr)
    , _writeProvider(nullptr)
    , _descriptionProvider(nullptr)
{
}

void PropertyServices::setValueResponseCallback(PropertyValueResponseCallback callback) {
    _valueResponseCallback = callback;
}

void PropertyServices::setDescriptionResponseCallback(PropertyDescriptionResponseCallback callback) {
    _descriptionResponseCallback = callback;
}

void PropertyServices::setReadProvider(PropertyReadProvider provider) {
    _readProvider = provider;
}

void PropertyServices::setWriteConsumer(PropertyWriteConsumer consumer) {
    _writeConsumer = consumer;
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
    
    // Read from property store
    const auto* obj = _storeManager.getObject(request.objectIndex);
    if (!obj) {
        KNX_LOGW(TAG, "Object %d not found", request.objectIndex.value());
        sendNegativeResponse();
        return util::Result<void>::err(util::ErrorCode::OperationFailed);
    }

    auto descriptor = obj->getDescriptor(request.propertyId);
    if (!descriptor) {
        KNX_LOGW(TAG, "Descriptor not found: obj=%d, prop=%d",
                 request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
        sendNegativeResponse();
        return util::Result<void>::err(util::ErrorCode::OperationFailed);
    }

    const size_t byteCount = static_cast<size_t>(request.elementCount) * descriptor->getElementSize();
    PropertyValueResponse::DataBuffer data;
    if (byteCount > data.capacity()) {
        KNX_LOGW(TAG, "Property read exceeds bounded payload: obj=%d, prop=%d, bytes=%zu",
                 request.objectIndex.value(), static_cast<uint8_t>(request.propertyId), byteCount);
        sendNegativeResponse();
        return util::Result<void>::err(util::ErrorCode::BufferTooSmall);
    }
    data.resize(byteCount);
    const auto readRes = _storeManager.readProperty(
        request.objectIndex,
        request.propertyId,
        request.startIndex,
        request.elementCount,
        data.span()
    );

    if (readRes.isError()) {
        KNX_LOGW(TAG, "Failed to read property: obj=%d, prop=%d",
             request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
        sendNegativeResponse();
        return util::Result<void>::err(readRes.error());
    }

    data.resize(readRes.value());
    
    // Create response
    PropertyValueResponse response;
    response.objectIndex = request.objectIndex;
    response.propertyId = request.propertyId;
    response.elementCount = responseElementCount;
    response.startIndex = request.startIndex;
    response.data = data;
    
    // Send response
    _valueResponseCallback(source, response);
    
    KNX_LOGD(TAG, "Property value read: obj=%d, prop=%d, bytes=%zu",
             request.objectIndex.value(), static_cast<uint8_t>(request.propertyId), response.data.size());
    
    return util::Result<void>::ok();
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
        if (!haveReadBack) {
            const auto* obj = _storeManager.getObject(request.objectIndex);
            if (obj) {
                auto descriptor = obj->getDescriptor(request.propertyId);
                if (descriptor) {
                    const size_t byteCount =
                        static_cast<size_t>(request.elementCount) * descriptor->getElementSize();
                    if (byteCount <= readData.capacity()) {
                        readData.resize(byteCount);
                        auto readRes = _storeManager.readProperty(request.objectIndex,
                                                                  request.propertyId,
                                                                  request.startIndex,
                                                                  request.elementCount,
                                                                  readData.span());
                        if (readRes.isOk()) {
                            readData.resize(readRes.value());
                            haveReadBack = true;
                        }
                    }
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

    // Full backend override (live object dispatch)
    if (_writeProvider) {
        auto providerRes = _writeProvider(source, request);
        if (providerRes.isError()) {
            KNX_LOGW(TAG, "Property write rejected by provider: obj=%d, prop=%d",
                     request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
            sendNegativeResponse();
            return providerRes.error();
        }

        sendWriteConfirmation();
        return util::Result<void>::ok();
    }

    // Optional backend policy/enforcement before writing to store.
    if (_writeConsumer) {
        auto consumerRes = _writeConsumer(source, request);
        if (consumerRes.isError()) {
            KNX_LOGW(TAG, "Property write rejected by backend: obj=%d, prop=%d",
                     request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
            sendNegativeResponse();
            return consumerRes.error();
        }
    }

    // Write to property store
    auto writeRes = _storeManager.writeProperty(
        request.objectIndex,
        request.propertyId,
        request.startIndex,
        request.elementCount,
        request.data.span()
    );

    if (writeRes.isError()) {
        KNX_LOGW(TAG, "Failed to write property: obj=%d, prop=%d",
             request.objectIndex.value(), static_cast<uint8_t>(request.propertyId));
        sendNegativeResponse();
        return writeRes;
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

    // Full backend override (interface object manager, etc.)
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
    const auto sendNotFound = [&](PropertyID echoPid) {
        _descriptionResponseCallback(
            source,
            objectIndex,
            echoPid,
            propertyIndex,
            PropertyWriteAccess::Denied,
            static_cast<PropertyDataType>(0),
            0u,   // maxElements = 0 → "property does not exist"
            0u,
            0u);
        return util::Result<void>::ok();
    };

    const auto* obj = _storeManager.getObject(objectIndex);
    if (!obj) {
        KNX_LOGW(TAG, "Object %d not found", objectIndex.value());
        return sendNotFound(propertyId);
    }

    PropertyID pid{};
    if (propertyIndex.value() == 0) {
        // By ID
        pid = propertyId;
    } else {
        // By index (1-based) — use query-then-fill pattern
        auto neededRes = obj->getAllPropertyIds(std::span<PropertyID>{});
        if (neededRes.isError()) {
            KNX_LOGW(TAG, "Failed to enumerate property IDs: obj=%d", objectIndex.value());
            return sendNotFound(propertyId);
        }
        const size_t needed = neededRes.value();
        if (propertyIndex.value() > needed) {
            KNX_LOGW(TAG, "Property index out of range: obj=%d, index=%d", objectIndex.value(), propertyIndex.value());
            return sendNotFound(propertyId);
        }
        util::FixedVector<PropertyID, objects::InterfaceObject::kMaxPropertyRegistrations> ids;
        if (needed > ids.capacity()) {
            KNX_LOGW(TAG, "Property enumeration exceeds bounded metadata: obj=%d, count=%zu",
                     objectIndex.value(), needed);
            return sendNotFound(propertyId);
        }
        ids.resize(needed);
        auto fillRes = obj->getAllPropertyIds(ids.span());
        if (fillRes.isError()) {
            KNX_LOGW(TAG, "Failed to populate property IDs: obj=%d", objectIndex.value());
            return sendNotFound(propertyId);
        }
        pid = ids[propertyIndex.value() - 1];
    }

    auto descriptor = obj->getDescriptor(pid);
    if (!descriptor) {
        KNX_LOGW(TAG, "Property descriptor not found: obj=%d, prop=%d",
                 objectIndex.value(), static_cast<int>(pid));
        return sendNotFound(pid);
    }
    
    // Send description response
    PropertyWriteAccess writeAccess = (descriptor->access != PropertyAccess::ReadOnly)
        ? PropertyWriteAccess::Allowed
        : PropertyWriteAccess::Denied;
    
    _descriptionResponseCallback(
        source,
        objectIndex,
        pid,
        propertyIndex,
        writeAccess,
        descriptor->type,
        descriptor->maxElements,
        descriptor->readLevel,
        descriptor->writeLevel
    );
    
    KNX_LOGD(TAG, "Property description read: obj=%d, prop=%d",
             objectIndex.value(), static_cast<int>(pid));
    
    return util::Result<void>::ok();
}

// ============================================================================
// Encoding/Decoding Helpers
// ============================================================================

util::Result<void> PropertyServices::encodeValueReadRequest(
    const PropertyValueReadRequest& request,
    std::span<uint8_t, kEncodedValueReadRequestLength> out
) {
    auto result = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::PropertyValueRead),
        {
            request.objectIndex.value(),
            static_cast<uint8_t>(request.propertyId),
            static_cast<uint8_t>((request.elementCount << 4) | ((request.startIndex >> 8) & 0x0F)),
            static_cast<uint8_t>(request.startIndex & 0xFFu)
        },
        out);
    if (result.isError()) {
        return result.error();
    }
    return util::Result<void>::ok();
}

PropertyValueReadRequest PropertyServices::decodeValueReadRequest(
    std::span<const uint8_t> data
) {
    PropertyValueReadRequest request;
    
    if (data.size() < 6) {
        KNX_LOGW(TAG, "Invalid property value read request size: %zu", data.size());
        return request;
    }

    // TPDU payload starts at offset 2
    request.objectIndex = InterfaceObjectIndex(data[2]);
    request.propertyId = static_cast<PropertyID>(data[3]);
    request.elementCount = (data[4] >> 4) & 0x0F;
    request.startIndex = ((data[4] & 0x0F) << 8) | data[5];
    
    return request;
}

util::Result<size_t> PropertyServices::encodeValueResponse(
    const PropertyValueResponse& response,
    std::span<uint8_t> out
) {
    const size_t payloadLength = kEncodedValueResponseHeaderLength - 2 + response.data.size();
    const size_t tpduLength = knx::protocol::tpduLengthFromPayload(payloadLength);
    if (out.size() < tpduLength) {
        return util::ErrorCode::BufferTooSmall;
    }

    knx::protocol::packTpduHeader(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::PropertyValueResponse),
        out[0],
        out[1]);
    out[2] = response.objectIndex.value();
    out[3] = static_cast<uint8_t>(response.propertyId);
    out[4] = static_cast<uint8_t>((response.elementCount << 4) | ((response.startIndex >> 8) & 0x0F));
    out[5] = static_cast<uint8_t>(response.startIndex & 0xFFu);
    for (size_t i = 0; i < response.data.size(); ++i) {
        out[6 + i] = response.data[i];
    }
    return tpduLength;
}

PropertyValueResponse PropertyServices::decodeValueResponse(
    std::span<const uint8_t> data
) {
    PropertyValueResponse response;
    
    if (data.size() < 6) {
        KNX_LOGW(TAG, "Invalid property value response size: %zu", data.size());
        return response;
    }

    // TPDU payload starts at offset 2
    response.objectIndex = InterfaceObjectIndex(data[2]);
    response.propertyId = static_cast<PropertyID>(data[3]);
    response.elementCount = (data[4] >> 4) & 0x0F;
    response.startIndex = ((data[4] & 0x0F) << 8) | data[5];
    
    // Extract property data
    if (data.size() > 6) {
        if (!response.data.assign(data.subspan(6))) {
            KNX_LOGW(TAG, "Property value response payload exceeds bounded storage: %zu", data.size() - 6);
            response.data.clear();
        }
    }
    
    return response;
}

util::Result<size_t> PropertyServices::encodeValueWriteRequest(
    const PropertyValueWriteRequest& request,
    std::span<uint8_t> out
) {
    const size_t payloadLength = kEncodedValueWriteRequestHeaderLength - 2 + request.data.size();
    const size_t tpduLength = knx::protocol::tpduLengthFromPayload(payloadLength);
    if (out.size() < tpduLength) {
        return util::ErrorCode::BufferTooSmall;
    }

    knx::protocol::packTpduHeader(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::PropertyValueWrite),
        out[0],
        out[1]);
    out[2] = request.objectIndex.value();
    out[3] = static_cast<uint8_t>(request.propertyId);
    out[4] = static_cast<uint8_t>((request.elementCount << 4) | ((request.startIndex >> 8) & 0x0F));
    out[5] = static_cast<uint8_t>(request.startIndex & 0xFFu);
    for (size_t i = 0; i < request.data.size(); ++i) {
        out[6 + i] = request.data[i];
    }
    return tpduLength;
}

PropertyValueWriteRequest PropertyServices::decodeValueWriteRequest(
    std::span<const uint8_t> data
) {
    PropertyValueWriteRequest request;
    
    if (data.size() < 6) {
        KNX_LOGW(TAG, "Invalid property value write request size: %zu", data.size());
        return request;
    }

    // TPDU payload starts at offset 2
    request.objectIndex = InterfaceObjectIndex(data[2]);
    request.propertyId = static_cast<PropertyID>(data[3]);
    request.elementCount = (data[4] >> 4) & 0x0F;
    request.startIndex = ((data[4] & 0x0F) << 8) | data[5];
    
    // Extract property data
    if (data.size() > 6) {
        if (!request.data.assign(data.subspan(6))) {
            KNX_LOGW(TAG, "Property value write payload exceeds bounded storage: %zu", data.size() - 6);
            request.data.clear();
        }
    }
    
    return request;
}

util::Result<void> PropertyServices::encodeDescriptionReadRequest(
    InterfaceObjectIndex objectIndex,
    PropertyID propertyId,
    PropertyIndex propertyIndex,
    std::span<uint8_t, kEncodedDescriptionReadRequestLength> out
) {
    auto result = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::PropertyDescriptionRead),
        {objectIndex.value(), static_cast<uint8_t>(propertyId), propertyIndex.value()},
        out);
    if (result.isError()) {
        return result.error();
    }
    return util::Result<void>::ok();
}

util::Result<void> PropertyServices::encodeDescriptionResponse(
    InterfaceObjectIndex objectIndex,
    PropertyID propertyId,
    PropertyIndex propertyIndex,
    PropertyWriteAccess writeAccess,
    PropertyDataType type,
    uint16_t maxElements,
    uint8_t readLevel,
    uint8_t writeLevel,
    std::span<uint8_t, kEncodedDescriptionResponseLength> out
) {
    const uint8_t typeByte = static_cast<uint8_t>(
        static_cast<uint8_t>(type) | (isWriteAllowed(writeAccess) ? 0x80u : 0x00u));

    auto result = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::PropertyDescriptionResponse),
        {
            objectIndex.value(),
            static_cast<uint8_t>(propertyId),
            propertyIndex.value(),
            typeByte,
            static_cast<uint8_t>((maxElements >> 8) & 0xFFu),
            static_cast<uint8_t>(maxElements & 0xFFu),
            static_cast<uint8_t>((readLevel << 4) | (writeLevel & 0x0F))
        },
        out);
    if (result.isError()) {
        return result.error();
    }
    return util::Result<void>::ok();
}

} // namespace application
} // namespace knx
