// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file function_property_services.cpp
 * @brief KNX Function Property service implementation (03/03/07 §3.4.5).
 */

#include "knx/application/function_property_services.hpp"

#include "knx/protocol/tpdu_codec.hpp"
#include "knx/util/log.hpp"

#include <algorithm>

namespace knx {
namespace application {

namespace {
constexpr const char* TAG = "KNX.App.FuncProp";
}  // namespace

util::Result<FunctionPropertyRequest> FunctionPropertyServices::decodeRequest(
    std::span<const uint8_t> apdu)
{
    // 2 octets of TPCI/APCI, then object_index and property_id.
    if (apdu.size() < 2u + kRequestHeaderBytes) {
        return util::ErrorCode::DecodeFailed;
    }

    const auto header = knx::protocol::unpackTpduHeader(apdu[0], apdu[1]);
    const auto service = header.apci.service();
    if (service != APCIService::FunctionPropertyCommand &&
        service != APCIService::FunctionPropertyStateRead) {
        return util::ErrorCode::DecodeFailed;
    }

    FunctionPropertyRequest request{};
    request.objectIndex = InterfaceObjectIndex(apdu[2]);
    request.propertyId = static_cast<PropertyID>(apdu[3]);
    request.invocation = (service == APCIService::FunctionPropertyCommand)
                             ? FunctionPropertyInvocation::Command
                             : FunctionPropertyInvocation::StateRead;

    const auto payload = apdu.subspan(2u + kRequestHeaderBytes);
    if (payload.size() > kMaxFunctionPropertyDataBytes) {
        // A function whose input does not fit our buffer cannot be executed
        // correctly, and truncating it would be worse than refusing it.
        return util::ErrorCode::BufferTooSmall;
    }
    if (!request.data.assign(payload)) {
        return util::ErrorCode::BufferTooSmall;
    }

    return request;
}

util::Result<size_t> FunctionPropertyServices::encodeResponse(
    InterfaceObjectIndex objectIndex,
    PropertyID propertyId,
    bool hasReturnCode,
    FunctionPropertyReturnCode returnCode,
    std::span<const uint8_t> data,
    std::span<uint8_t> out)
{
    // object_index + property_id [+ return_code + data]
    const size_t payloadLength =
        kRequestHeaderBytes + (hasReturnCode ? (1u + data.size()) : 0u);
    if (out.size() < 2u + payloadLength) {
        return util::ErrorCode::BufferTooSmall;
    }

    // Assemble the payload directly in the output buffer past the header, then
    // let buildTpdu write the header over the first two octets.
    std::span<uint8_t> payload = out.subspan(2u, payloadLength);
    payload[0] = static_cast<uint8_t>(objectIndex.value());
    payload[1] = static_cast<uint8_t>(propertyId);
    if (hasReturnCode) {
        payload[2] = static_cast<uint8_t>(returnCode);
        std::copy(data.begin(), data.end(), payload.begin() + 3);
    }

    return knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::FunctionPropertyStateResponse),
        payload,
        out);
}

util::Result<void> FunctionPropertyServices::handleRequest(const IndividualAddress& source,
                                                           const FunctionPropertyRequest& request)
{
    if (!_responseCallback) {
        KNX_LOGE(TAG, "No response callback registered");
        return util::ErrorCode::OperationNotReady;
    }

    // §3.4.5.3: when the addressed property is not PDT_FUNCTION, the response
    // carries neither a return code nor data.  An absent handler is the same
    // observable situation, so it takes the same path.
    if (!_handler) {
        _responseCallback(source, request.objectIndex, request.propertyId, false,
                          FunctionPropertyReturnCode::Success, {});
        return util::Result<void>::ok();
    }

    auto result = _handler(source, request);
    if (result.isError()) {
        KNX_LOGD(TAG, "Object %u property %u is not a function property",
                 static_cast<unsigned>(request.objectIndex.value()),
                 static_cast<unsigned>(static_cast<uint8_t>(request.propertyId)));
        _responseCallback(source, request.objectIndex, request.propertyId, false,
                          FunctionPropertyReturnCode::Success, {});
        return util::Result<void>::ok();
    }

    const auto& value = result.value();
    _responseCallback(source, request.objectIndex, request.propertyId, true,
                      value.returnCode, value.data.span());
    return util::Result<void>::ok();
}

} // namespace application
} // namespace knx
