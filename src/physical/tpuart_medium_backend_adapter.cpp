// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tpuart_medium_backend_adapter.cpp
 * @brief TP1 medium backend adapter for TPUART-class physical implementations
 */

#include "knx/physical/tpuart_medium_backend_adapter.hpp"

namespace knx {
namespace physical {

util::Result<void> TpuartMediumBackendAdapter::init(const Tp1MediumConfig& config) {
    _config = config;

    auto initResult = _physical.init(_physical.context);
    if (initResult.isError()) {
        return initResult;
    }

    auto monitorResult = _physical.setBusMonitorMode(
        _physical.context,
        config.busMonitorMode ? Toggle::Enable : Toggle::Disable);
    if (monitorResult.isError()) {
        _physical.close(_physical.context);
        return monitorResult;
    }

    _initialized = true;
    return util::Result<void>::ok();
}

void TpuartMediumBackendAdapter::close() {
    _physical.close(_physical.context);
    _initialized = false;
}

util::Result<size_t> TpuartMediumBackendAdapter::sendFrame(std::span<const uint8_t> frame) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }

    auto sendResult = _physical.sendFrame(_physical.context, frame);
    if (_eventCallback) {
        Tp1RxEvent event;
        event.type = Tp1RxEventType::TxAckResponse;
        event.ackClass = sendResult.isOk() ? Tp1AckClass::Ack : mapSendErrorToAckClass(sendResult.error());
        _eventCallback(event, _eventContext);
    }

    if (sendResult.isError()) {
        return sendResult.error();
    }

    return frame.size();
}

void TpuartMediumBackendAdapter::setEventCallback(Tp1EventCallback callback, void* context) {
    _eventCallback = std::move(callback);
    _eventContext = context;
}

Tp1MediumState TpuartMediumBackendAdapter::getState() const {
    if (!_initialized) {
        return Tp1MediumState::Uninitialized;
    }
    return mapState(_physical.getState(_physical.context));
}

Tp1CapabilityProfile TpuartMediumBackendAdapter::getCapabilities() const {
    Tp1CapabilityProfile capabilities;
    capabilities.supportsHardwareAutoAck = true;
    capabilities.supportsAddressedFiltering = true;
    capabilities.supportsByteEventStream = false;
    capabilities.supportsDetailedTxConfirm = true;
    capabilities.supportsCollisionIndication = false;
    capabilities.supportsDiagnosticsSnapshot = false;
    // A TPUART2 has the same information the bitbang driver reads off STKNX's
    // KNX_OK — the SAVE pin for supply loss, and the U_State.indication warning
    // bits for a degraded transceiver. Neither is plumbed through this adapter
    // yet, so it reports LinkState::Unknown via the base-class default. Wiring
    // them up means setting this flag, overriding getLinkState(), and emitting
    // Tp1RxEventType::LinkStateChanged; nothing above this adapter changes.
    capabilities.supportsLinkStateIndication = false;
    return capabilities;
}

util::Result<void> TpuartMediumBackendAdapter::setBusMonitorMode(bool enabled) {
    _config.busMonitorMode = enabled;

    if (!_initialized) {
        return util::Result<void>::ok();
    }

    return _physical.setBusMonitorMode(_physical.context, enabled ? Toggle::Enable : Toggle::Disable);
}

util::Result<void> TpuartMediumBackendAdapter::service() {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }

    auto receiveFrameView = [&]() -> util::Result<std::span<const uint8_t>> {
        if (_frameSource) {
            return _frameSource->receiveFrameView(0);
        }
        return _physical.receiveFrameView(_physical.context, 0);
    };

    auto frameViewResult = receiveFrameView();
    if (frameViewResult.isError()) {
        if (frameViewResult.error() == util::ErrorCode::Timeout) {
            return util::Result<void>::ok();
        }
        return frameViewResult.error();
    }

    if (_eventCallback) {
        Tp1RxEvent event;
        event.type = Tp1RxEventType::TelegramEnd;
        event.frame = frameViewResult.value();
        _eventCallback(event, _eventContext);
    }

    return util::Result<void>::ok();
}

Tp1MediumState TpuartMediumBackendAdapter::mapState(PhysicalLayerState state) {
    switch (state) {
        case PhysicalLayerState::Idle:
            return Tp1MediumState::Idle;
        case PhysicalLayerState::Receiving:
            return Tp1MediumState::Receiving;
        case PhysicalLayerState::Transmitting:
            return Tp1MediumState::Transmitting;
        case PhysicalLayerState::Error:
        default:
            return Tp1MediumState::Error;
    }
}

Tp1AckClass TpuartMediumBackendAdapter::mapSendErrorToAckClass(util::ErrorCode code) {
    if (code == util::ErrorCode::Busy) {
        return Tp1AckClass::Busy;
    }
    return Tp1AckClass::Nack;
}

} // namespace physical
} // namespace knx
