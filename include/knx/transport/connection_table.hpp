// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file connection_table.hpp
 * @brief Transport layer connection table management
 * 
 * Manages multiple simultaneous connections with per-connection state tracking.
 * Each connection has its own sequence number, timeout, and retry state.
 * 
 * Limits:
 * - Maximum 16 simultaneous connections (typical for small devices)
 * - One connection per remote device address
 * - Automatic cleanup on timeout
 */

#pragma once

#include "knx/transport/connection_state.hpp"
#include "knx/constants.hpp"
#include "knx/config.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <vector>
#include <memory>
#include <optional>

namespace knx {
namespace transport {

/**
 * @brief Connection entry in the connection table
 * 
 * Represents a single connection with all its state and metadata.
 */
struct ConnectionEntry {
    /// Connection state machine
    std::unique_ptr<ConnectionStateMachine> stateMachine;
    
    /// Connection index (0-15)
    ConnectionIndex index;
    
    /// Remote device address
    IndividualAddress remoteAddress;
    
    /// Sequence number for this connection (TX)
    uint8_t sequenceNumber;

    /// Expected receive sequence number (RX)
    uint8_t expectedRxSeq;

    /// Sequence number of pending (un-ACKed) TX packet
    uint8_t pendingSeq;

    /// Maximum retransmission attempts for this connection
    uint8_t maxRetries;
    
    /// Last received sequence number (for duplicate detection)
    uint8_t lastReceivedSeq;
    
    /// Time last activity occurred (for timeout detection)
    uint32_t lastActivityTimeMs;
    
    /// Pending retransmission flag
    bool pendingRetransmission;
    
    /// Data waiting for ACK
    util::FixedVector<uint8_t, config::MAX_APDU_LENGTH> pendingData;

    /// APCI for pending retransmission (raw 10-bit value)
    uint16_t pendingApciRaw;

    /// Telegram priority for this connection, mirrored from the remote's frames.
    /// All frames we send on the connection (T_ACK/T_NAK/T_Disconnect/data) use it.
    Priority priority;

    /// Priority the pending (un-ACKed) TX packet was originally sent with
    Priority pendingPriority;
    
    /// Retransmission count for current packet
    uint8_t retransmitCount;
    
    /// Time of last transmission (for retransmit timeout)
    uint32_t lastTxTimeMs;
    
    /// Retransmit timeout in milliseconds (starts at 3000, may double)
    uint32_t retransmitTimeoutMs;
};

/**
 * @brief Connection table for managing multiple connections
 * 
 * Handles creation, lookup, and management of connection entries.
 * Provides automatic timeout detection and cleanup.
 * 
 * @thread_safety NOT thread-safe - must be synchronized externally
 * @warning Do not access from multiple threads without external synchronization
 * @note Future: Add internal mutex or use lock-free design
 */
class ConnectionTable {
public:
    /// Maximum number of simultaneous connections
    /// Upper bound on concurrently tracked connections. Only a coupler or an
    /// interface ever needs more than one; see kDefaultMaxConnections.
    static constexpr size_t MAX_CONNECTIONS = 16;

    /// Connections a plain KNX device accepts at once.
    ///
    /// KNX 03/03/04: "This state machine is designed for only one connection at
    /// a time. To use more than one connection at a time several state machines
    /// are needed, one for each connection." An end device has one. Accepting a
    /// second would let two management clients interleave downloads into the
    /// same device, each believing it had exclusive access — a failure mode that
    /// corrupts commissioning silently.
    ///
    /// Couplers and IP interfaces legitimately multiplex; they raise this by
    /// passing a larger value to init().
    static constexpr size_t kDefaultMaxConnections = 1;
    
    /**
     * @brief Initialize connection table
     * 
     * @param maxConnections Maximum connections allowed (default 16)
    * @return Result<void> indicating success or error
     */
    util::Result<void> init(size_t maxConnections = kDefaultMaxConnections);
    
    /**
     * @brief Get current number of active connections
     * @return Number of connections in table
     */
    size_t activeConnectionCount() const { return _connections.size(); }
    
    /**
     * @brief Get maximum allowed connections
     * @return Max connections
     */
    size_t maxConnections() const { return _maxConnections; }
    
    /**
     * @brief Check if a connection can be created
     * @param remoteAddress Remote device address
     * @return true if table has space and connection doesn't exist
     */
    util::Result<void> canCreateConnection(const IndividualAddress& remoteAddress) const;
    
    /**
     * @brief Check and handle retransmission timeouts
     * 
     * Should be called periodically (e.g., 100ms intervals).
     * Returns list of connection indices needing retransmission.
     * 
     * @param currentTimeMs Current system time in milliseconds
     * @return Vector of connection indices that need retransmission
     */
    util::FixedVector<ConnectionIndex, MAX_CONNECTIONS> processRetransmissions(uint32_t currentTimeMs);
    
    /**
     * @brief Check for connection timeouts
     * 
     * Removes connections that have exceeded maximum idle time.
     * Default timeout is 6000ms per KNX spec.
     * 
     * @param currentTimeMs Current system time in milliseconds
     * @param timeoutMs Maximum idle time (default 6000ms)
     * @return Vector of connection indices that timed out
     */
    util::FixedVector<ConnectionIndex, MAX_CONNECTIONS> processTimeouts(uint32_t currentTimeMs,
                    uint32_t timeoutMs = constants::timing::CONNECTION_TIMEOUT_MS);
    
    /**
     * @brief Mark connection data as sent
     * 
     * Updates transmission time and increments retry counter.
     * 
     * @param index Connection index
    * @return Result<void> indicating success or error
     */
    util::Result<void> markDataSent(ConnectionIndex index);
    
    /**
     * @brief Reset retransmit state for connection
     * 
     * Called when ACK is received to clear pending retransmission.
     * 
     * @param index Connection index
    * @return Result<void> indicating success or error
     */
    util::Result<void> resetRetransmitState(ConnectionIndex index);
    
    /**
     * @brief Get retransmit count for connection
     * 
     * @param index Connection index
     * @return Retransmit count (0 if connection not found)
     */
    uint8_t getRetransmitCount(ConnectionIndex index) const;
    
    /**
     * @brief Check if received sequence number is a duplicate
     * 
     * Compares received sequence with last received to detect retransmissions.
     * Updates lastReceivedSeq if not a duplicate.
     * 
     * @param index Connection index
     * @param seqNum Received sequence number (0-15)
     * @return true if duplicate (same as last received), false if new
     */
    util::Result<bool> isDuplicate(ConnectionIndex index, uint8_t seqNum);
    
    /**
     * @brief Create a new connection
     * 
     * Creates connection entry with initialized state machine.
     * Returns false if table is full or connection already exists.
     * 
     * @param remoteAddress Remote device address
     * @param params Connection parameters
    * @return Connection index on success, invalid on failure
     * 
     * @pre remoteAddress must be valid (not 0xFFFF)
     * @pre Connection must not already exist for this address
     * @pre Table must not be full
     */
    ConnectionIndex createConnection(const IndividualAddress& remoteAddress, const ConnectionParams& params);
    
    /**
     * @brief Get connection by index
     * 
     * @param index Connection index (0-15)
     * @return Pointer to connection entry, or nullptr if not found
     */
    ConnectionEntry* getConnection(ConnectionIndex index);
    const ConnectionEntry* getConnection(ConnectionIndex index) const;
    
    /**
     * @brief Get connection by remote address
     * 
     * @param remoteAddress Remote device address
     * @return Pointer to connection entry, or nullptr if not found
     */
    ConnectionEntry* findConnection(const IndividualAddress& remoteAddress);
    const ConnectionEntry* findConnection(const IndividualAddress& remoteAddress) const;
    
    /**
     * @brief Remove a connection
     * 
     * Closes the connection and removes from table.
     * Called when connection is closed or times out.
     * 
     * @param index Connection index
     * @return true if removed, false if not found
     */
    util::Result<void> removeConnection(ConnectionIndex index);
    
    /**
     * @brief Remove connection by address
     * 
     * @param remoteAddress Remote device address
     * @return true if removed, false if not found
     */
    util::Result<void> removeConnectionByAddress(const IndividualAddress& remoteAddress);
    
    /**
     * @brief Check for timed-out connections
     * 
     * Removes any connections that have exceeded their timeout period.
     * Called periodically by transport layer.
     * 
     * @param currentTimeMs Current system time in milliseconds
     * @param timeoutMs Timeout period in milliseconds
     * @return Number of connections that timed out
     */
    size_t checkTimeouts(uint32_t currentTimeMs, uint32_t timeoutMs);
    
    /**
     * @brief Update connection activity time
     * 
     * Called when data is sent or received on the connection.
     * Used for timeout detection.
     * 
     * @param index Connection index
     * @param timeMs Current time in milliseconds
     */
    void updateActivityTime(ConnectionIndex index, uint32_t timeMs);
    
    /**
     * @brief Get all connections (for iteration)
     * 
     * @return Vector of connection entries
     */
    const std::vector<std::unique_ptr<ConnectionEntry>>& allConnections() const {
        return _connections;
    }
    
    /**
     * @brief Clear all connections
     */
    void clear() {
        _connections.clear();
    }

private:
    std::vector<std::unique_ptr<ConnectionEntry>> _connections;
    size_t _maxConnections;
    uint8_t _nextIndex;
    
    /// Maximum idle time for a connection (6000ms per KNX spec)
    static constexpr uint32_t CONNECTION_TIMEOUT_MS = constants::timing::CONNECTION_TIMEOUT_MS;
    
    /// Initial retransmit timeout (3000ms per KNX spec)
    static constexpr uint32_t INITIAL_RETRANSMIT_TIMEOUT = constants::timing::ACK_TIMEOUT_MS;
    
    /// Maximum retransmit attempts per KNX spec
    static constexpr uint8_t MAX_RETRANSMIT_COUNT = 3;
    
    /**
     * @brief Find an available connection index
     * @return Next available index, or 0xFF if none available
     */
    ConnectionIndex findAvailableIndex();
};

} // namespace transport
} // namespace knx
