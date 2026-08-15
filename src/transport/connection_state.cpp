// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file connection_state.cpp
 * @brief Transport layer connection state machine implementation
 * 
 * Implements the KNX transport layer state machine logic for managing
 * connection-oriented communication.
 */

#include "knx/transport/connection_state.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"

static const char* TAG = "KNX.Transport.ConnState";

namespace knx {
namespace transport {

util::Result<void> ConnectionStateMachine::init(const ConnectionParams& params) {
    if (!params.remoteAddress.isValid()) {
        KNX_LOGE(TAG, "Invalid remote address");
        return util::Result<void>::err(util::ErrorCode::InvalidAddress);
    }
    
    _params = params;
    _state = ConnectionState::Idle;
    _sequenceNumber = params.initialSequence;
    _retryCount = 0;
    
    KNX_LOGD(TAG, "ConnectionStateMachine initialized for address 0x%04X",
             params.remoteAddress.raw);
    
    return util::Result<void>::ok();
}

util::Result<void> ConnectionStateMachine::handleEvent(ConnectionEvent event) {
    auto validResult = isEventValid(event);
    if (validResult.isError()) {
        KNX_LOGW(TAG, "Invalid event %d for state %d",
                 static_cast<int>(event), static_cast<int>(_state));
        return util::Result<void>::err(util::ErrorCode::OperationNotSupported);
    }
    
    ConnectionState nextState = _state;
    
    // State machine transition table
    switch (_state) {
        case ConnectionState::Idle:
            if (event == ConnectionEvent::ConnectRequest) {
                nextState = ConnectionState::Connecting;
            }
            break;
            
        case ConnectionState::Connecting:
            if (event == ConnectionEvent::ConnectResponse) {
                nextState = ConnectionState::Connected;
                resetRetry();
            } else if (event == ConnectionEvent::ConnectionRefused) {
                nextState = ConnectionState::Idle;
                resetRetry();
            } else if (event == ConnectionEvent::Timeout) {
                nextState = ConnectionState::Idle;
                resetRetry();
            }
            break;
            
        case ConnectionState::Connected:
            if (event == ConnectionEvent::DisconnectRequest) {
                nextState = ConnectionState::Disconnecting;
            } else if (event == ConnectionEvent::DataIndicate) {
                // Stay connected, just reset retry counter
                resetRetry();
            } else if (event == ConnectionEvent::DataRequest) {
                // Stay connected
                resetRetry();
            } else if (event == ConnectionEvent::Timeout) {
                nextState = ConnectionState::Idle;
                resetRetry();
            }
            break;
            
        case ConnectionState::Disconnecting:
            if (event == ConnectionEvent::DisconnectResponse) {
                nextState = ConnectionState::Idle;
                resetRetry();
            } else if (event == ConnectionEvent::Timeout) {
                nextState = ConnectionState::Idle;
                resetRetry();
            }
            break;
    }
    
    // Perform transition if state changed
    if (nextState != _state) {
        transitionTo(nextState, event);
        return util::Result<void>::ok();
    }
    
    return util::Result<void>::ok();
}

util::Result<void> ConnectionStateMachine::isEventValid(ConnectionEvent event) const {
    switch (_state) {
        case ConnectionState::Idle:
            if (event == ConnectionEvent::ConnectRequest) return util::Result<void>::ok();
            return util::ErrorCode::OperationNotSupported;
            
        case ConnectionState::Connecting:
            if (event == ConnectionEvent::ConnectResponse ||
                event == ConnectionEvent::ConnectionRefused ||
                event == ConnectionEvent::Timeout ||
                event == ConnectionEvent::RetransmitTimeout) {
                return util::Result<void>::ok();
            }
            return util::ErrorCode::OperationNotSupported;
            
        case ConnectionState::Connected:
            if (event == ConnectionEvent::DisconnectRequest ||
                event == ConnectionEvent::DataRequest ||
                event == ConnectionEvent::DataIndicate ||
                event == ConnectionEvent::InvalidSequence ||
                event == ConnectionEvent::Timeout) {
                return util::Result<void>::ok();
            }
            return util::ErrorCode::OperationNotSupported;
            
        case ConnectionState::Disconnecting:
            if (event == ConnectionEvent::DisconnectResponse ||
                event == ConnectionEvent::Timeout) {
                return util::Result<void>::ok();
            }
            return util::ErrorCode::OperationNotSupported;
            
        default:
            return util::ErrorCode::OperationNotSupported;
    }
}

void ConnectionStateMachine::transitionTo(ConnectionState newState, ConnectionEvent event) {
    const char* stateStr[] = {"Idle", "Connecting", "Connected", "Disconnecting"};
    
    KNX_LOGD(TAG, "State transition: %s -> %s (event %d)",
             stateStr[static_cast<int>(_state)],
             stateStr[static_cast<int>(newState)],
             static_cast<int>(event));
    
    _state = newState;
    
    // Invoke event handler if registered
    if (_eventHandler) {
        _eventHandler(event);
    }
}

} // namespace transport
} // namespace knx
