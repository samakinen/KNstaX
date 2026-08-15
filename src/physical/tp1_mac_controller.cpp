// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_mac_controller.cpp
 * @brief TP1 medium-neutral MAC controller skeleton implementation
 */

#include "knx/physical/tp1_mac_controller.hpp"

#include "knx/datalink/tp1_dl_common.hpp"
#include "knx/types.hpp"

namespace knx {
namespace physical {

Tp1MacController::Tp1MacController(Tp1MediumBackend& backend)
    : _backend(backend)
    , _eventObserver(nullptr)
    , _eventObserverContext(nullptr)
    , _lastTxAckResponse(Tp1AckClass::None)
    , _lastTxAckDeadlineMiss(false)
    , _txOutcomeSequence(0)
    , _hasAckDiagnosticsSnapshot(false)
    , _latestAckDiagnosticsSnapshot{}
    , _diagnosticsSnapshotPeriodTicks(0)
    , _diagnosticsTickCounter(0)
    , _diagnosticsRetryBackoffTicks(0)
    , _diagnosticsRetryCooldownTicks(0)
{
}

util::Result<void> Tp1MacController::init(const Tp1MediumConfig& config) {
    _backend.setEventCallback(backendEventShim, this);
    return _backend.init(config);
}

util::Result<void> Tp1MacController::setOwnAddress(uint16_t addressRaw) {
    return _backend.setOwnAddress(addressRaw);
}

void Tp1MacController::close() {
    _backend.setEventCallback(nullptr, nullptr);
    _backend.close();
    _lastTxAckResponse = Tp1AckClass::None;
    _lastTxAckDeadlineMiss = false;
    _txOutcomeSequence = 0;
    _hasAckDiagnosticsSnapshot = false;
    _latestAckDiagnosticsSnapshot = Tp1AckDiagnosticsSnapshot{};
    _diagnosticsTickCounter = 0;
    _diagnosticsRetryCooldownTicks = 0;
}

util::Result<size_t> Tp1MacController::sendFrame(std::span<const uint8_t> frame) {
    return _backend.sendFrame(frame);
}

bool Tp1MacController::supportsDiagnosticsSnapshot() const {
    return _backend.diagnostics() != nullptr && _backend.getCapabilities().supportsDiagnosticsSnapshot;
}

util::Result<void> Tp1MacController::requestDiagnosticsSnapshot() {
    auto* diagnostics = _backend.diagnostics();
    if (!diagnostics) {
        return util::ErrorCode::OperationNotSupported;
    }
    return diagnostics->requestDiagnosticsSnapshot();
}

void Tp1MacController::setDiagnosticsSnapshotPeriod(uint32_t ticks) {
    _diagnosticsSnapshotPeriodTicks = ticks;
    _diagnosticsTickCounter = 0;
}

void Tp1MacController::setDiagnosticsRetryBackoffTicks(uint32_t ticks) {
    _diagnosticsRetryBackoffTicks = ticks;
    _diagnosticsRetryCooldownTicks = 0;
}

util::Result<void> Tp1MacController::onTick() {
    auto serviceResult = _backend.service();
    if (serviceResult.isError()) {
        return serviceResult;
    }

    if (_diagnosticsSnapshotPeriodTicks == 0) {
        return util::Result<void>::ok();
    }

    if (_diagnosticsRetryCooldownTicks > 0) {
        --_diagnosticsRetryCooldownTicks;
        return util::Result<void>::ok();
    }

    if (!supportsDiagnosticsSnapshot()) {
        return util::Result<void>::ok();
    }

    ++_diagnosticsTickCounter;
    if (_diagnosticsTickCounter >= _diagnosticsSnapshotPeriodTicks) {
        _diagnosticsTickCounter = 0;
        auto result = requestDiagnosticsSnapshot();
        if (result.isError() && _diagnosticsRetryBackoffTicks > 0) {
            _diagnosticsRetryCooldownTicks = _diagnosticsRetryBackoffTicks;
        }
        return result;
    }

    return util::Result<void>::ok();
}

void Tp1MacController::setEventObserver(Tp1EventCallback callback, void* context) {
    _eventObserver = std::move(callback);
    _eventObserverContext = context;
}

void Tp1MacController::onBackendEvent(const Tp1RxEvent& event) {
    switch (event.type) {
        case Tp1RxEventType::TxAckResponse:
            _lastTxAckResponse = event.ackClass;
            _lastTxAckDeadlineMiss = false;
            ++_txOutcomeSequence;
            break;
        case Tp1RxEventType::TxAckDeadlineMiss:
            _lastTxAckResponse = Tp1AckClass::None;
            _lastTxAckDeadlineMiss = true;
            ++_txOutcomeSequence;
            break;
        case Tp1RxEventType::AckDiagnosticsSnapshot:
            _latestAckDiagnosticsSnapshot = event.ackDiagnostics;
            _hasAckDiagnosticsSnapshot = true;
            break;
        default:
            break;
    }

    if (_eventObserver) {
        _eventObserver(event, _eventObserverContext);
    }
}

Tp1AckClass Tp1MacController::getLastTxAckResponse() const {
    return _lastTxAckResponse;
}

bool Tp1MacController::didLastTxAckMissDeadline() const {
    return _lastTxAckDeadlineMiss;
}

uint32_t Tp1MacController::getTxOutcomeSequence() const {
    return _txOutcomeSequence;
}

bool Tp1MacController::hasAckDiagnosticsSnapshot() const {
    return _hasAckDiagnosticsSnapshot;
}

Tp1AckDiagnosticsSnapshot Tp1MacController::getLatestAckDiagnosticsSnapshot() const {
    return _latestAckDiagnosticsSnapshot;
}

void Tp1MacController::setLocalBusy(bool busy) {
    _backend.setLocalBusy(busy);
}

void Tp1MacController::setAckGroupAddresses(std::span<const uint16_t> addresses) {
    _backend.setAckGroupAddresses(addresses);
}

void Tp1MacController::backendEventShim(const Tp1RxEvent& event, void* context) {
    auto* self = static_cast<Tp1MacController*>(context);
    if (self) {
        self->onBackendEvent(event);
    }
}

} // namespace physical
} // namespace knx
