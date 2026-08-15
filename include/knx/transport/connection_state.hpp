// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file connection_state.hpp
 * @brief Transport layer connection state machine
 * 
 * Implements the KNX transport layer connection state machine for connection-oriented
 * services (T_Connect/T_Disconnect/T_ACK/T_NAK).
 * 
 * States:
 * - Idle: No connection
 * - Connecting: T_Connect.req sent, awaiting T_Connect.res
 * - Connected: Connection established, data transfer possible
 * - Disconnecting: T_Disconnect.req sent, awaiting confirmation
 */

#pragma once

#include <cstdint>
#include <functional>
#include "knx/constants.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

namespace knx {
namespace transport {

/**
 * @brief Connection state enumeration
 * 
 * Defines the states a connection can be in during its lifecycle.
 */
enum class ConnectionState {
    Idle,          ///< No connection established
    Connecting,    ///< T_Connect sent, awaiting response
    Connected,     ///< Connection established, ready for data
    Disconnecting, ///< T_Disconnect sent, awaiting confirmation
};

/**
 * @brief Connection event enumeration
 * 
 * Events that trigger state transitions.
 */
enum class ConnectionEvent {
    // User-initiated events
    ConnectRequest,      ///< User requested T_Connect
    DisconnectRequest,   ///< User requested T_Disconnect
    DataRequest,         ///< User wants to send data
    
    // Remote events
    ConnectResponse,     ///< Received T_Connect.res
    DisconnectResponse,  ///< Received T_Disconnect.res
    DataIndicate,        ///< Received T_Data while connected
    
    // Timer events
    Timeout,             ///< Connection timeout
    RetransmitTimeout,   ///< Retransmission timeout
    
    // Error events
    InvalidSequence,     ///< Invalid sequence number received
    ConnectionRefused,   ///< Remote refused connection
};

/**
 * @brief Connection parameters
 * 
 * Parameters for a connection session.
 */
struct ConnectionParams {
    /// Individual address of remote device
    IndividualAddress remoteAddress;
    
    /// Initial sequence number (typically 0)
    uint8_t initialSequence;
    
    /// Maximum retransmission attempts
    uint8_t maxRetries;
    
    /// Connection timeout in milliseconds
    uint32_t timeoutMs;
    
    /// Retransmission timeout in milliseconds
    uint32_t retransmitTimeoutMs;
    
    ConnectionParams()
        : remoteAddress()
        , initialSequence(0)
        , maxRetries(constants::timing::MAX_RETRANSMIT_COUNT)
        , timeoutMs(constants::timing::CONNECTION_TIMEOUT_MS)
        , retransmitTimeoutMs(constants::timing::RETRANSMIT_TIMEOUT_MS)
    {}
};

/**
 * @brief Transport layer connection state machine
 * 
 * Manages the state transitions and event handling for a single connection.
 * This is a simple synchronous state machine - for each connection, there should
 * be one instance of this class.
 * 
 * @note This class is not thread-safe. Synchronization must be done at a higher level.
 * @note State transitions are synchronous - no async callbacks.
 */
class ConnectionStateMachine {
public:
    /// Event handler callback type
    using EventHandler = std::function<void(ConnectionEvent event)>;
    
    /**
     * @brief Initialize state machine
     * @param params Connection parameters
    * @return Result<void> indicating success or error
     */
    util::Result<void> init(const ConnectionParams& params);
    
    /**
     * @brief Get current connection state
     * @return Current state
     */
    ConnectionState state() const { return _state; }
    
    /**
     * @brief Get remote device address
     * @return Remote address
     */
    IndividualAddress remoteAddress() const { return _params.remoteAddress; }
    
    /**
     * @brief Get current sequence number
     * @return Current sequence (0-15)
     */
    uint8_t sequenceNumber() const { return _sequenceNumber; }
    
    /**
     * @brief Increment sequence number (wraps at 16 - 4-bit per KNX spec)
     * @return New sequence number
     */
    uint8_t nextSequence() {
        _sequenceNumber = (_sequenceNumber + 1) & 0x0F;
        return _sequenceNumber;
    }
    
    /**
     * @brief Get retransmission counter
     * @return Current retry count
     */
    uint8_t retryCount() const { return _retryCount; }
    
    /**
     * @brief Increment retry counter
     * @return true if retries exhausted, false otherwise
     */
    bool incrementRetry() {
        _retryCount++;
        return _retryCount >= _params.maxRetries;
    }
    
    /**
     * @brief Reset retry counter
     * 
     * Called when data is successfully acknowledged.
     */
    void resetRetry() {
        _retryCount = 0;
    }
    
    /**
     * @brief Return to Idle, keeping the configured connection parameters
     *
     * For when the peer re-opens the connection with a fresh T_Connect: the
     * session starts over from Idle instead of continuing from a state whose
     * events no longer match what the peer believes.
     */
    void reset() {
        _state = ConnectionState::Idle;
        _sequenceNumber = _params.initialSequence;
        resetRetry();
    }

    /**
     * @brief Handle a state machine event
     *
     * Processes the event and updates internal state accordingly.
     * Calls the registered event handler if transition is valid.
     * 
     * @param event The event to process
    * @return Result<void> indicating success or invalid transition
     * 
     * @note Invalid event transitions are logged as warnings
     */
    util::Result<void> handleEvent(ConnectionEvent event);
    
    /**
     * @brief Set event handler callback
     * 
     * The callback is invoked when a significant state change occurs.
     * Used to trigger protocol-level actions (send frames, set timers, etc.).
     * 
     * @param handler Callback function (nullptr to disable)
     */
    void setEventHandler(EventHandler handler) {
        _eventHandler = handler;
    }
    
    /**
     * @brief Check if connection is ready for data transfer
     * @return true if state is Connected
     */
    bool isConnected() const {
        return _state == ConnectionState::Connected;
    }
    
    /**
     * @brief Check if connection is in process (connecting or disconnecting)
     * @return true if state is Connecting or Disconnecting
     */
    bool isTransitioning() const {
        return _state == ConnectionState::Connecting || 
               _state == ConnectionState::Disconnecting;
    }

private:
    ConnectionParams _params;
    ConnectionState _state;
    uint8_t _sequenceNumber;
    uint8_t _retryCount;
    EventHandler _eventHandler;
    
    /**
     * @brief Execute state transition
     * 
     * @param newState The new state
     * @param triggerEvent The event that triggered transition
     */
    void transitionTo(ConnectionState newState, ConnectionEvent event);
    
    /**
     * @brief Validate event for current state
     * 
     * Checks if the event is valid for the current state.
     * 
     * @param event Event to validate
     * @return true if event is valid for current state
     */
    util::Result<void> isEventValid(ConnectionEvent event) const;
};

} // namespace transport
} // namespace knx
