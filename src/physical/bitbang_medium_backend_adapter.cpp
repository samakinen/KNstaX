// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bitbang_medium_backend_adapter.cpp
 * @brief TP1 medium backend adapter for BitBangDriverInterface
 */

#include "knx/physical/bitbang_board_profile.hpp"
#include "knx/physical/bitbang_medium_backend_adapter.hpp"
#include "knx/physical/manchester_codec.hpp"
#include "knx/physical/tp1_ack_utils.hpp"

namespace knx {
namespace physical {

namespace {
void pollTp1(BitBangDriverTp1Interface& tp1Driver) {
    tp1Driver.pollTp1();
}

void emitTp1Events(BitBangDriverTp1Interface& tp1Driver, const Tp1EventCallback& callback, void* context) {
    if (!callback) {
        return;
    }

    Tp1RxEvent event;
    while (tp1Driver.popTp1Event(event)) {
        callback(event, context);
    }
}
} // namespace

BitBangMediumBackendAdapter::BitBangMediumBackendAdapter(BitBangDriverInterface& driver,
                                                         BitBangDriverTp1Interface& tp1Driver)
    : _driver(driver)
    , _tp1Driver(tp1Driver)
    , _eventCallback(nullptr)
    , _eventContext(nullptr)
    , _config()
    , _initialized(false)
{
}

util::Result<void> BitBangMediumBackendAdapter::init(const Tp1MediumConfig& config) {
    _config = config;

    BitBangConfig driverConfig;
    driverConfig.baudRate = ManchesterCodec::BAUD_RATE;
    driverConfig.bitRate = ManchesterCodec::BIT_RATE;
    driverConfig.bitCellUs = ManchesterCodec::BIT_CELL_US;
    driverConfig.halfBitUs = ManchesterCodec::HALF_BIT_US;
    driverConfig.bitCellNs = ManchesterCodec::BIT_CELL_NS;
    driverConfig.halfBitNs = ManchesterCodec::HALF_BIT_NS;
    applyDefaultBitBangBoardProfile(driverConfig);

    auto result = _driver.init(driverConfig);
    if (result.isError()) {
        return result;
    }

    _driver.setRxCallback(rxShim, this);

    auto ownAddrRes = _tp1Driver.setOwnAddress(config.ownIndividualAddressRaw);
    if (ownAddrRes.isError()) {
        _driver.close();
        return ownAddrRes;
    }

    auto monitorRes = _tp1Driver.setBusMonitorMode(config.busMonitorMode);
    if (monitorRes.isError()) {
        _driver.close();
        return monitorRes;
    }

    pollTp1(_tp1Driver);
    emitTp1Events(_tp1Driver, _eventCallback, _eventContext);

    _initialized = true;

    return util::Result<void>::ok();
}

void BitBangMediumBackendAdapter::close() {
    _driver.close();
    _initialized = false;
}

util::Result<size_t> BitBangMediumBackendAdapter::sendFrame(std::span<const uint8_t> frame) {
    pollTp1(_tp1Driver);
    auto result = _driver.send(frame);
    pollTp1(_tp1Driver);
    emitTp1Events(_tp1Driver, _eventCallback, _eventContext);
    if (result.isError()) {
        return result.error();
    }
    return frame.size();
}

void BitBangMediumBackendAdapter::setEventCallback(Tp1EventCallback callback, void* context) {
    _eventCallback = std::move(callback);
    _eventContext = context;
    pollTp1(_tp1Driver);
    emitTp1Events(_tp1Driver, _eventCallback, _eventContext);
}

Tp1MediumState BitBangMediumBackendAdapter::getState() const {
    return mapState(_driver.getState());
}

Tp1CapabilityProfile BitBangMediumBackendAdapter::getCapabilities() const {
    Tp1CapabilityProfile capabilities;
    // The bitbang ISR decides and transmits DL-ACKs fully autonomously from
    // its own timing domain (address match mid-frame, ACK byte at t_ack).
    capabilities.supportsHardwareAutoAck = true;
    capabilities.supportsAddressedFiltering = false;
    capabilities.supportsByteEventStream = true;
    capabilities.supportsDetailedTxConfirm = true;
    capabilities.supportsCollisionIndication = true;
    capabilities.supportsDiagnosticsSnapshot = true;
    // Only when the board actually wires the transceiver's bus-health output.
    capabilities.supportsLinkStateIndication = _tp1Driver.hasTp1LinkStateIndication();
    return capabilities;
}

LinkState BitBangMediumBackendAdapter::getLinkState() const {
    return _tp1Driver.getTp1LinkState();
}

util::Result<void> BitBangMediumBackendAdapter::setBusMonitorMode(bool enabled) {
    _config.busMonitorMode = enabled;
    return _tp1Driver.setBusMonitorMode(enabled);
}

util::Result<void> BitBangMediumBackendAdapter::setOwnAddress(uint16_t addressRaw) {
    _config.ownIndividualAddressRaw = addressRaw;
    return _tp1Driver.setOwnAddress(addressRaw);
}

void BitBangMediumBackendAdapter::setAckGroupAddresses(std::span<const uint16_t> addresses) {
    _tp1Driver.setAckGroupAddresses(addresses);
}

void BitBangMediumBackendAdapter::setLocalBusy(bool busy) {
    _tp1Driver.setLocalBusy(busy);
}

util::Result<void> BitBangMediumBackendAdapter::requestDiagnosticsSnapshot() {
    pollTp1(_tp1Driver);

    if (!_eventCallback) {
        return util::Result<void>::ok();
    }

    const auto stats = _tp1Driver.getTp1AckDiagnostics();

    Tp1RxEvent event;
    event.type = Tp1RxEventType::AckDiagnosticsSnapshot;
    event.ackDiagnostics.windowOpenedNoDecisionCount = stats.windowOpenedNoDecisionCount;
    event.ackDiagnostics.decisionLatchedLateCount = stats.decisionLatchedLateCount;
    event.ackDiagnostics.responseEmittedCount = stats.responseEmittedCount;
    event.ackDiagnostics.deadlineMissCount = stats.deadlineMissCount;
    event.ackDiagnostics.overflowErrorCount = stats.overflowErrorCount;
    event.ackDiagnostics.rxAckObservedCount = stats.rxAckObservedCount;
    event.ackDiagnostics.unsupportedRawIngressCount = stats.unsupportedRawIngressCount;
    _eventCallback(event, _eventContext);

    return util::Result<void>::ok();
}

void BitBangMediumBackendAdapter::setQueueNotifyCallback(QueueNotifyCallback callback, void* context) {
    _driver.setQueueNotifyCallback(callback, context);
}

util::Result<void> BitBangMediumBackendAdapter::service() {
    pollTp1(_tp1Driver);
    emitTp1Events(_tp1Driver, _eventCallback, _eventContext);
    return util::Result<void>::ok();
}

void BitBangMediumBackendAdapter::rxShim(std::span<const uint8_t> frame, void* context) {
    auto* self = static_cast<BitBangMediumBackendAdapter*>(context);
    if (self) {
        self->onRxFrame(frame);
    }
}

void BitBangMediumBackendAdapter::onRxFrame(std::span<const uint8_t> frame) {
    if (!_eventCallback || frame.empty()) {
        return;
    }

    if (frame.size() == 1u && isTp1AckByte(frame[0])) {
        Tp1RxEvent ackEvent;
        ackEvent.type = Tp1RxEventType::RxAckObserved;
        ackEvent.ackClass = tp1AckClassFromByte(frame[0]);
        _eventCallback(ackEvent, _eventContext);

        emitTp1Events(_tp1Driver, _eventCallback, _eventContext);
        return;
    }

    Tp1RxEvent event;
    event.type = Tp1RxEventType::TelegramEnd;
    event.frame = frame;
    _eventCallback(event, _eventContext);

    emitTp1Events(_tp1Driver, _eventCallback, _eventContext);
}

Tp1MediumState BitBangMediumBackendAdapter::mapState(DriverState state) {
    switch (state) {
        case DriverState::Uninitialized:
            return Tp1MediumState::Uninitialized;
        case DriverState::Idle:
            return Tp1MediumState::Idle;
        case DriverState::Receiving:
            return Tp1MediumState::Receiving;
        case DriverState::Transmitting:
            return Tp1MediumState::Transmitting;
        case DriverState::Error:
        default:
            return Tp1MediumState::Error;
    }
}

} // namespace physical
} // namespace knx
