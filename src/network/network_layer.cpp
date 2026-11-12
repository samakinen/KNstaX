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
    , _filteringEnabled(false)
{
}

NetworkLayer::~NetworkLayer() {
    close();
}

void NetworkLayer::finishTxOperation() {
    _txOperation = TxOperationState{};
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
        _initialized = false;
    }
}

util::Result<void> NetworkLayer::sendFrame(const NDataFrame& frame) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
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
