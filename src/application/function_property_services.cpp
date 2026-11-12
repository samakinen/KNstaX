// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file function_property_services.cpp
 * @brief KNX Function Property service implementation (03/03/07 §3.4.5).
 */

#include "knx/application/function_property_services.hpp"

#include "knx/util/log.hpp"

#include <algorithm>

namespace knx {
namespace application {

namespace {
constexpr const char* TAG = "KNX.App.FuncProp";
}  // namespace

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
