// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file transport_layer.cpp
 * @brief Transport layer implementation
 */

#include "knx/transport/transport_layer.hpp"
#include "knx/util/log.hpp"

#include <utility>

static const char* TAG = "KNX.Transport";
namespace knx {
namespace transport {

TransportLayer::TransportLayer(network::NetworkLayer& network)
    : _network(network)
    , _ownAddress(0)
    , _rxCallback(nullptr)
    , _rxCallbackStorage()
    , _txTransform(nullptr)
    , _rxTransform(nullptr)
    , _initialized(false)
    , _timebaseMs(0)
{
}

TransportLayer::~TransportLayer() {
    close();
}

void TransportLayer::enqueueReceivedFrame(const TDataFrame& frame) {
    if (_rxQueueCount >= RX_QUEUE_CAPACITY) {
        ++_droppedReceiveFrames;
        KNX_LOGW(TAG, "Dropping transport RX frame: queue full (%zu dropped)", _droppedReceiveFrames);
        return;
    }

    const size_t index = (_rxQueueHead + _rxQueueCount) % RX_QUEUE_CAPACITY;
    _rxQueue[index] = frame;
    ++_rxQueueCount;
}

bool TransportLayer::popReceivedFrame(TDataFrame& frame) {
    if (_rxQueueCount == 0u) {
        return false;
    }

    frame = _rxQueue[_rxQueueHead];
    _rxQueueHead = (_rxQueueHead + 1u) % RX_QUEUE_CAPACITY;
    --_rxQueueCount;
    return true;
}

void TransportLayer::finishConnectedSendOperation() {
    _connectedSendOperation = ConnectedSendOperationState{};
}

void TransportLayer::finishSendOperation() {
    _sendOperation = SendOperationState{};
}

void TransportLayer::finishConnectOperation() {
    _connectOperation = ConnectOperationState{};
}

void TransportLayer::finishRetransmissionOperation() {
    _retransmissionOperation = RetransmissionOperationState{};
}

void TransportLayer::finishReceiveOperation() {
    _receiveOperation = ReceiveOperationState{};
}

TransportLayer::ControlSendProgressState TransportLayer::mapTxErrorToProgressState(util::ErrorCode error) const {
    if (error == util::ErrorCode::Busy) {
        return ControlSendProgressState::Busy;
    }
    if (error == util::ErrorCode::Timeout) {
        return ControlSendProgressState::Timeout;
    }
    return ControlSendProgressState::TransmissionFailed;
}

util::Result<void> TransportLayer::init(const IndividualAddress& ownAddress) {
    if (_initialized) {
        KNX_LOGW(TAG, "Already initialized");
        return util::Result<void>::ok();
    }

    _ownAddress = ownAddress;

    // Ensure network layer is initialized (idempotent if already initialized)
    auto netInit = _network.init(ownAddress);
    if (!netInit) {
        KNX_LOGE(TAG, "Failed to initialize network layer: %s",
                 util::errorCodeToString(netInit.error()));
        return netInit;
    }

    auto tableInit = _connectionTable.init();
    if (!tableInit) {
        KNX_LOGE(TAG, "Failed to initialize connection table: %s",
                 util::errorCodeToString(tableInit.error()));
        return tableInit;
    }

    _network.setReceiveCallback([this](const network::NDataFrame& frame) {
        handleNetworkRx(frame);
    });
    _initialized = true;

    KNX_LOGD(TAG, "Transport layer initialized, own address: %d.%d.%d",
             _ownAddress.area(), _ownAddress.line(), _ownAddress.device());

    return util::Result<void>::ok();
}

util::Result<void> TransportLayer::setOwnAddress(const IndividualAddress& ownAddress) {
    _ownAddress = ownAddress;
    _network.setOwnAddress(ownAddress);
    return util::Result<void>::ok();
}

void TransportLayer::close() {
    if (_initialized) {
        // Prevent lower layers from calling back into this transport layer
        _network.setReceiveCallback(nullptr);
        _rxCallback.store(nullptr, std::memory_order_release);

        _rxQueueHead = 0u;
        _rxQueueCount = 0u;
        _droppedReceiveFrames = 0u;
        finishSendOperation();
        finishConnectedSendOperation();
        finishConnectOperation();
        finishRetransmissionOperation();
        finishReceiveOperation();
        _initialized = false;
    }
}

void TransportLayer::setReceiveCallback(TDataCallback callback) {
    if (callback) {
        _rxCallbackStorage = std::move(callback);
        _rxCallback.store(&_rxCallbackStorage, std::memory_order_release);
    } else {
        _rxCallback.store(nullptr, std::memory_order_release);
    }
}

void TransportLayer::setTxTransform(TDataTransform transform) {
    _txTransform = std::move(transform);
}

void TransportLayer::setRxTransform(TDataTransform transform) {
    _rxTransform = std::move(transform);
}

TDataFrame TransportLayer::convertFromNetwork(const network::NDataFrame& nFrame) {
    TDataFrame tFrame;
    
    tFrame.source = nFrame.dlFrame.source;
    tFrame.destination = nFrame.dlFrame.destination;
    tFrame.destinationType = nFrame.dlFrame.destinationType;
    tFrame.tpdu = nFrame.dlFrame.tpdu;

    tFrame.standardFrame = nFrame.dlFrame.standardFrame;
    tFrame.repeated = nFrame.dlFrame.repeated;
    tFrame.priority = nFrame.dlFrame.priority;
    tFrame.ackRequested = nFrame.dlFrame.ackRequested;
    tFrame.confirmation = nFrame.dlFrame.confirmation;
    tFrame.hopCount = nFrame.dlFrame.hopCount;
    
    // Determine service type
    if (isGroupAddress(tFrame.destinationType)) {
        tFrame.service = TDataService::Group;
    } else {
        tFrame.service = TDataService::Individual;
    }
    
    return tFrame;
}

} // namespace transport
} // namespace knx
