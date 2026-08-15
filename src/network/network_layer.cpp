// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file network_layer.cpp
 * @brief Network layer implementation
 */

#include "knx/network/network_layer.hpp"
#include "knx/util/log.hpp"
#include "knx/util/hex.hpp"

static const char* TAG [[maybe_unused]] = "KNX.Network";

namespace knx {
namespace network {

NetworkLayer::NetworkLayer(datalink::Tp1DataLinkLayer& datalink)
    : _datalink(datalink)
    , _ownAddress(0)
    , _rxCallback(nullptr)
    , _rxCallbackStorage()
    , _initialized(false)
    , _mode(NetworkMode::Device)
    , _routingEnabled(false)
    , _filteringEnabled(false)
{
}

NetworkLayer::~NetworkLayer() {
    close();
}

void NetworkLayer::finishTxOperation() {
    _txOperation = TxOperationState{};
}

void NetworkLayer::finishForwardOperation() {
    _forwardOperation = ForwardOperationState{};
}

util::Result<void> NetworkLayer::enqueueForwardFrame(const NDataFrame& frame) {
    if (_forwardQueueCount >= FORWARD_QUEUE_CAPACITY) {
        ++_droppedForwardFrames;
        return util::ErrorCode::QueueFull;
    }

    const size_t index = (_forwardQueueHead + _forwardQueueCount) % FORWARD_QUEUE_CAPACITY;
    _forwardQueue[index] = frame.dlFrame;
    ++_forwardQueueCount;
    return util::Result<void>::ok();
}

util::Result<void> NetworkLayer::init(const IndividualAddress& ownAddress) {
    if (_initialized) {
        KNX_LOGW(TAG, "Already initialized");
        return util::Result<void>::ok();
    }
    
    _ownAddress = ownAddress;
    _datalink.setReceiveCallback([this](const datalink::LDataFrame& frame) {
        handleDlRx(frame);
    });
    _initialized = true;
    
    KNX_LOGD(TAG, "Network layer initialized, own address: %d.%d.%d",
             _ownAddress.area(), _ownAddress.line(), _ownAddress.device());

    return util::Result<void>::ok();
}

void NetworkLayer::setOwnAddress(const IndividualAddress& ownAddress) {
    _ownAddress = ownAddress;
}

void NetworkLayer::close() {
    if (_initialized) {
        // Clear our receive callback to avoid delivering into torn-down code.
        // Do NOT call into `_datalink` here — the datalink object may already
        // have been destroyed by the owner, which would cause a use-after-free.
        _rxCallback.store(nullptr, std::memory_order_release);

        finishTxOperation();
        finishForwardOperation();
        _forwardQueueHead = 0u;
        _forwardQueueCount = 0u;
        _initialized = false;
    }
}

void NetworkLayer::setNetworkMode(NetworkMode mode) {
    _mode = mode;
    
    // Enable routing for coupler modes
    if (mode == NetworkMode::LineCoupler || mode == NetworkMode::AreaCoupler) {
        _routingEnabled = true;
    } else {
        _routingEnabled = false;
    }
    
    KNX_LOGI(TAG, "Network mode set to: %s",
             mode == NetworkMode::Device ? "Device" :
             mode == NetworkMode::LineCoupler ? "LineCoupler" : "AreaCoupler");
}

util::Result<void> NetworkLayer::sendFrame(const NDataFrame& frame) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }
    if (_forwardOperation.active) {
        return util::ErrorCode::Busy;
    }
    
    // Check hop count
    if (frame.hopCount > 6) {
        KNX_LOGW(TAG, "Invalid hop count: %d", frame.hopCount);
        return util::ErrorCode::InvalidParameter;
    }
    
    // Create data link frame from network frame
    datalink::LDataFrame dlFrame = frame.dlFrame;
    dlFrame.hopCount = frame.hopCount;
    
    // Send via data link layer
    return _datalink.sendFrame(dlFrame);
}

util::Result<void> NetworkLayer::beginTransmit(const NDataFrame& frame) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }
    if (_forwardOperation.active) {
        return util::ErrorCode::Busy;
    }
    if (_txOperation.active) {
        return util::ErrorCode::Busy;
    }
    if (frame.hopCount > 6) {
        KNX_LOGW(TAG, "Invalid hop count: %d", frame.hopCount);
        return util::ErrorCode::InvalidParameter;
    }

    _txOperation.active = true;
    _txOperation.started = false;
    _txOperation.dlFrame = frame.dlFrame;
    _txOperation.dlFrame.hopCount = frame.hopCount;
    return util::Result<void>::ok();
}

util::Result<NetworkLayer::TxProgressState> NetworkLayer::pollTransmit() {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_txOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    if (!_txOperation.started) {
        auto beginResult = _datalink.beginTransmit(_txOperation.dlFrame);
        if (beginResult.isError()) {
            finishTxOperation();
            const auto error = beginResult.error();
            if (error == util::ErrorCode::Busy) {
                return TxProgressState::Busy;
            }
            if (error == util::ErrorCode::Timeout) {
                return TxProgressState::Timeout;
            }
            return TxProgressState::TransmissionFailed;
        }
        _txOperation.started = true;
    }

    auto progress = _datalink.pollTransmit();
    if (progress.isError()) {
        finishTxOperation();
        return progress.error();
    }

    switch (progress.value()) {
        case datalink::Tp1DataLinkLayer::TxProgressState::Pending:
            return TxProgressState::Pending;
        case datalink::Tp1DataLinkLayer::TxProgressState::Success:
            finishTxOperation();
            return TxProgressState::Success;
        case datalink::Tp1DataLinkLayer::TxProgressState::Busy:
            finishTxOperation();
            return TxProgressState::Busy;
        case datalink::Tp1DataLinkLayer::TxProgressState::TransmissionFailed:
            finishTxOperation();
            return TxProgressState::TransmissionFailed;
        case datalink::Tp1DataLinkLayer::TxProgressState::Timeout:
            finishTxOperation();
            return TxProgressState::Timeout;
    }

    finishTxOperation();
    return util::ErrorCode::OperationFailed;
}

util::Result<NetworkLayer::ForwardProgressState> NetworkLayer::pollForwarding() {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }

    if (_txOperation.active) {
        return hasPendingForwarding() ? ForwardProgressState::Pending
                                      : ForwardProgressState::Idle;
    }

    if (_forwardOperation.active) {
        if (!_forwardOperation.started) {
            auto beginResult = _datalink.beginTransmit(_forwardOperation.dlFrame);
            if (beginResult.isError()) {
                if (beginResult.error() == util::ErrorCode::Busy) {
                    return ForwardProgressState::Pending;
                }

                const auto error = beginResult.error();
                KNX_LOGW(TAG, "Deferred forwarding begin failed: %s",
                         util::errorCodeToString(error));
                finishForwardOperation();
                if (error == util::ErrorCode::Timeout) {
                    return ForwardProgressState::Timeout;
                }
                return ForwardProgressState::TransmissionFailed;
            }

            _forwardOperation.started = true;
            return ForwardProgressState::Pending;
        }

        auto progress = _datalink.pollTransmit();
        if (progress.isError()) {
            const auto error = progress.error();
            KNX_LOGW(TAG, "Deferred forwarding failed: %s",
                     util::errorCodeToString(error));
            finishForwardOperation();
            return error;
        }

        switch (progress.value()) {
            case datalink::Tp1DataLinkLayer::TxProgressState::Pending:
                return ForwardProgressState::Pending;
            case datalink::Tp1DataLinkLayer::TxProgressState::Success:
                finishForwardOperation();
                return ForwardProgressState::Success;
            case datalink::Tp1DataLinkLayer::TxProgressState::Busy:
                finishForwardOperation();
                return ForwardProgressState::Busy;
            case datalink::Tp1DataLinkLayer::TxProgressState::TransmissionFailed:
                finishForwardOperation();
                return ForwardProgressState::TransmissionFailed;
            case datalink::Tp1DataLinkLayer::TxProgressState::Timeout:
                finishForwardOperation();
                return ForwardProgressState::Timeout;
        }

        finishForwardOperation();
        return util::ErrorCode::OperationFailed;
    }

    if (_forwardQueueCount == 0u) {
        return ForwardProgressState::Idle;
    }

    _forwardOperation.active = true;
    _forwardOperation.started = false;
    _forwardOperation.dlFrame = std::move(_forwardQueue[_forwardQueueHead]);
    _forwardQueueHead = (_forwardQueueHead + 1u) % FORWARD_QUEUE_CAPACITY;
    --_forwardQueueCount;
    return pollForwarding();
}

void NetworkLayer::setReceiveCallback(NDataCallback callback) {
    if (callback) {
        _rxCallbackStorage = std::move(callback);
        _rxCallback.store(&_rxCallbackStorage, std::memory_order_release);
    } else {
        _rxCallback.store(nullptr, std::memory_order_release);
    }
}

bool NetworkLayer::isOnSameArea(const IndividualAddress& addr) const {
    return _ownAddress.area() == addr.area();
}

bool NetworkLayer::isOnSameLine(const IndividualAddress& addr) const {
    return _ownAddress.area() == addr.area() && 
           _ownAddress.line() == addr.line();
}

void NetworkLayer::handleDlRx(const datalink::LDataFrame& frame) {
    if (!_initialized) {
        return;
    }

    // Already logged once by the datalink layer's "RX ..." line.
    // Convert L_Data to N_Data
    NDataFrame nFrame;
    nFrame.dlFrame = frame;
    nFrame.hopCount = frame.hopCount;
    
    // Check filtering if enabled
    if (_filteringEnabled && shouldFilterFrame(frame)) {
        KNX_LOGD(TAG, "Frame filtered: dest=0x%04X", frame.destination.raw);
        return;
    }
    
    // Check if frame should be forwarded (for coupler modes)
    bool shouldForward = false;
    if (_routingEnabled) {
        shouldForward = shouldForwardFrame(frame, nFrame.hopCount);
        
        // Decrement hop count for forwarding
        if (shouldForward && nFrame.hopCount > 0) {
            nFrame.hopCount--;
            
            // Forward the frame outside the receive callback context.
            if (nFrame.hopCount > 0) {
                NDataFrame fwdFrame = nFrame;
                auto enqueueResult = enqueueForwardFrame(fwdFrame);
                if (enqueueResult.isError()) {
                    KNX_LOGW(TAG, "Dropping forwarded frame: %s",
                             util::errorCodeToString(enqueueResult.error()));
                }
            }
        }
    }
    
    // Always deliver to local application if addressed to us or broadcast
    bool forLocalDelivery = false;
    if (isGroupAddress(frame.destinationType)) {
        // Group addresses are always delivered locally
        forLocalDelivery = true;
    } else {
        // Individual address - check if it's for us (destination is stored as GroupAddress but represents IndividualAddress)
        IndividualAddress destAddr(frame.destination.raw);
        forLocalDelivery = isIndividualBroadcastAddress(destAddr) || (destAddr == _ownAddress);
    }
    
    if (forLocalDelivery) {
        const auto* cb = _rxCallback.load(std::memory_order_acquire);
        if (cb) {
            (*cb)(nFrame);
        }
    }
}

bool NetworkLayer::shouldForwardFrame(const datalink::LDataFrame& frame, uint8_t hopCount) {
    // Don't forward if hop count exhausted
    if (hopCount == 0) {
        return false;
    }
    
    // Get routing decision from routing table
    RoutingDecision decision = _routingTable.route(
        frame.source,
        frame.destination,
        frame.destinationType,
        hopCount,
        _ownAddress
    );
    
    // Forward based on decision
    switch (decision) {
        case RoutingDecision::Forward:
        case RoutingDecision::ForwardLocal:
            return true;
            
        case RoutingDecision::Block:
        case RoutingDecision::Drop:
        default:
            return false;
    }
}

bool NetworkLayer::shouldFilterFrame(const datalink::LDataFrame& frame) {
    // Only filter group addresses
    if (!isGroupAddress(frame.destinationType)) {
        return false;
    }
    
    // Check filter table
    FilterAction action = _filterTable.checkFilter(frame.destination);
    
    // Block means filter out (don't pass)
    return (action == FilterAction::Block);
}

} // namespace network
} // namespace knx
