// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file restart_service.cpp
 * @brief KNX Restart Service implementation
 */

#include "knx/application/restart_service.hpp"
#include "knx/util/log.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/util/result.hpp"
#include <span>

namespace knx {
namespace application {

static const char* TAG = "KNX.App.Restart";

RestartService::RestartService()
    : _restartPending(false)
    , _pendingRestartType(RestartType::Basic) {
}

util::Result<void> RestartService::handleRequest(const IndividualAddress& source, RestartType type) {
    // Check authorization if callback is set
    if (_authorizationCallback) {
        auto authRes = _authorizationCallback(source);
        if (authRes.isError()) {
            KNX_LOGW(TAG, "Restart denied for %d.%d.%d - not authorized",
                     source.area(), source.line(), source.device());
            return authRes.error();
        }
    }
    
    // Validate restart type
    if (type != RestartType::Basic && type != RestartType::MasterReset) {
        KNX_LOGE(TAG, "Invalid restart type: %d", static_cast<int>(type));
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    // Check if restart callback is set
    if (!_restartCallback) {
        KNX_LOGE(TAG, "No restart callback registered");
        return util::Result<void>::err(util::ErrorCode::OperationNotReady);
    }
    
    // Mark restart as pending
    _restartPending = true;
    _pendingRestartType = type;
    
    KNX_LOGI(TAG, "Restart requested by %d.%d.%d, type=%s",
             source.area(), source.line(), source.device(),
             type == RestartType::Basic ? "Basic" : "MasterReset");
    
    return util::Result<void>::ok();
}

void RestartService::executePendingRestart() {
    if (!_restartPending) {
        return;
    }
    
    KNX_LOGI(TAG, "Executing restart, type=%s",
             _pendingRestartType == RestartType::Basic ? "Basic" : "MasterReset");
    
    // Execute cleanup callback if set
    if (_cleanupCallback) {
        KNX_LOGD(TAG, "Executing cleanup callback");
        _cleanupCallback();
    }
    
    // Clear restart pending flag
    _restartPending = false;
    
    // Execute restart
    if (_restartCallback) {
        auto restartRes = _restartCallback(_pendingRestartType);
        if (restartRes.isError()) {
            KNX_LOGE(TAG, "Restart callback failed");
        }
    }
}

util::Result<void> RestartService::encodeRequest(RestartType type, std::span<uint8_t, kEncodedRequestLength> out) {
    auto result = knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(APCIService::Restart),
        {static_cast<uint8_t>(type)},
        out
    );
    if (result.isError()) return result.error();
    return util::Result<void>::ok();
}

util::Result<void> RestartService::decodeRequest(std::span<const uint8_t> data, RestartType& type) {
    if (data.size() < 3) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    const auto hdr = knx::protocol::unpackTpduHeader(data[0], data[1]);
        if (hdr.apci.service() != APCIService::Restart) {
        return util::Result<void>::err(util::ErrorCode::DecodeFailed);
    }

    // TPDU payload starts at offset 2
    uint8_t typeValue = data[2];
    
    // Validate restart type
    if (typeValue > static_cast<uint8_t>(RestartType::MasterReset)) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    type = static_cast<RestartType>(typeValue);
    return util::Result<void>::ok();
}

} // namespace application
} // namespace knx
