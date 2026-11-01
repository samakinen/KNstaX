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
    _pendingEraseCode = EraseCode::ConfirmedRestart;

    KNX_LOGI(TAG, "Restart requested by %d.%d.%d, type=%s",
             source.area(), source.line(), source.device(),
             type == RestartType::Basic ? "Basic" : "MasterReset");

    return util::Result<void>::ok();
}

bool RestartService::isEraseCodeSupported(uint8_t eraseCode) const {
    if (eraseCode == static_cast<uint8_t>(EraseCode::ConfirmedRestart)) {
        return true;
    }
    return _masterResetCallback
        && eraseCode >= static_cast<uint8_t>(EraseCode::FactoryReset)
        && eraseCode <= static_cast<uint8_t>(EraseCode::FactoryResetWithoutIndividualAddress);
}

RestartErrorCode RestartService::handleMasterResetRequest(const IndividualAddress& source,
                                                          uint8_t eraseCode,
                                                          uint8_t channel) {
    // Table 5 recommends Access Denied only where level-2-or-higher Resources
    // are involved; the authorisation hook decides that, not this service.
    if (_authorizationCallback && _authorizationCallback(source).isError()) {
        KNX_LOGW(TAG, "Master reset denied for %d.%d.%d - not authorized",
                 source.area(), source.line(), source.device());
        return RestartErrorCode::AccessDenied;
    }

    // 03/05/02 Table 4: a reserved or unimplemented Erase Code is answered
    // "Unsupported Erase Code" and neither a Basic Restart nor any Master
    // Reset is executed. Its Channel Number is "not defined", so this check
    // comes first.
    if (!isEraseCodeSupported(eraseCode)) {
        KNX_LOGW(TAG, "Master reset from %d.%d.%d with unsupported erase code 0x%02X",
                 source.area(), source.line(), source.device(), eraseCode);
        return RestartErrorCode::UnsupportedEraseCode;
    }

    // 03/03/07 §3.4.2.2 cases 1 and 2: a non-zero Channel Number is invalid
    // both for the codes fixed at 00h and on a device without application
    // channels, which covers every code on this one.
    if (channel != 0u) {
        KNX_LOGW(TAG, "Master reset from %d.%d.%d with channel %u: device has no channels",
                 source.area(), source.line(), source.device(), channel);
        return RestartErrorCode::InvalidChannelNumber;
    }

    if (!_restartCallback) {
        KNX_LOGE(TAG, "No restart callback registered");
        return RestartErrorCode::NotReady;
    }

    _restartPending = true;
    _pendingRestartType = RestartType::MasterReset;
    _pendingEraseCode = static_cast<EraseCode>(eraseCode);

    KNX_LOGI(TAG, "Master reset requested by %d.%d.%d, erase code 0x%02X",
             source.area(), source.line(), source.device(), eraseCode);
    return RestartErrorCode::NoError;
}

void RestartService::executePendingRestart() {
    if (!_restartPending) {
        return;
    }

    KNX_LOGI(TAG, "Executing restart, type=%s, erase code 0x%02X",
             _pendingRestartType == RestartType::Basic ? "Basic" : "MasterReset",
             static_cast<unsigned>(_pendingEraseCode));

    // Execute cleanup callback if set
    if (_cleanupCallback) {
        KNX_LOGD(TAG, "Executing cleanup callback");
        _cleanupCallback();
    }

    // Clear restart pending flag
    _restartPending = false;
    const EraseCode eraseCode = _pendingEraseCode;
    _pendingEraseCode = EraseCode::ConfirmedRestart;

    // The Resources go before the restart, so that the device comes back up
    // already in the state the client asked for. A failed reset still
    // restarts: the response promised a restart, and the client is now
    // waiting for the device to come back.
    if (_pendingRestartType == RestartType::MasterReset
        && eraseCode != EraseCode::ConfirmedRestart
        && _masterResetCallback) {
        auto resetRes = _masterResetCallback(eraseCode);
        if (resetRes.isError()) {
            KNX_LOGE(TAG, "Master reset with erase code 0x%02X failed: %s",
                     static_cast<unsigned>(eraseCode), util::errorCodeToString(resetRes.error()));
        }
    }

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
