// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file connection_table.cpp
 * @brief Connection table implementation
 */

#include "knx/transport/connection_table.hpp"
#include "knx/util/log.hpp"
#include <algorithm>

static const char* TAG = "KNX.Transport.ConnTable";

namespace knx {
namespace transport {

util::Result<void> ConnectionTable::init(size_t maxConnections) {
    
    if (maxConnections == 0 || maxConnections > MAX_CONNECTIONS) {
        KNX_LOGE(TAG, "Invalid maxConnections: %zu", maxConnections);
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    _maxConnections = maxConnections;
    _nextIndex = 0;
    _connections.clear();
    _spare.clear();
    // One entry and state machine per connection the table may hold, built now.
    _connections.reserve(_maxConnections);
    _spare.reserve(_maxConnections);
    for (size_t i = 0; i < _maxConnections; ++i) {
        auto spare = std::make_unique<ConnectionEntry>();
        spare->stateMachine = std::make_unique<ConnectionStateMachine>();
        _spare.push_back(std::move(spare));
    }

    KNX_LOGD(TAG, "ConnectionTable initialized with capacity %zu", maxConnections);
    return util::Result<void>::ok();
}

util::Result<void> ConnectionTable::canCreateConnection(const IndividualAddress& remoteAddress) const {
    // Check if full
    if (_connections.size() >= _maxConnections) {
        return util::ErrorCode::ResourceUnavailable;
    }

    // Check if connection already exists
    if (findConnection(remoteAddress) != nullptr) {
        return util::ErrorCode::OperationFailed;
    }

    return util::Result<void>::ok();
}

ConnectionIndex ConnectionTable::createConnection(const IndividualAddress& remoteAddress, const ConnectionParams& params) {
    
    // Validate address
    if (!remoteAddress.isValid()) {
        KNX_LOGE(TAG, "Invalid remote address 0x%04X", remoteAddress.raw);
        return ConnectionIndex::invalid();
    }
    
    // Check capacity
    if (_connections.size() >= _maxConnections) {
        KNX_LOGE(TAG, "Connection table full (%zu/%zu)",
                 _connections.size(), _maxConnections);
        return ConnectionIndex::invalid();
    }
    
    // Check for duplicate
    if (findConnection(remoteAddress) != nullptr) {
        KNX_LOGW(TAG, "Connection already exists for address 0x%04X", remoteAddress.raw);
        return ConnectionIndex::invalid();
    }
    
    // Create new entry
    std::unique_ptr<ConnectionEntry> entry;
    if (!_spare.empty()) {
        entry = std::move(_spare.back());
        _spare.pop_back();
        entry->pendingData.clear();
    } else {
        entry = std::make_unique<ConnectionEntry>();
        entry->stateMachine = std::make_unique<ConnectionStateMachine>();
    }
    entry->index = ConnectionIndex(_nextIndex);
    entry->remoteAddress = remoteAddress;
    entry->sequenceNumber = params.initialSequence & 0x0F;
    entry->expectedRxSeq = params.initialSequence & 0x0F;
    entry->pendingSeq = 0;
    entry->maxRetries = params.maxRetries;
    entry->lastReceivedSeq = 0xFF;  // Invalid value - first packet not duplicate
    entry->lastActivityTimeMs = 0;
    entry->pendingRetransmission = false;
    entry->pendingApciRaw = 0;
    // Connection-oriented traffic defaults to System priority (matches certified
    // devices during commissioning); overwritten by the remote's actual priority
    // as soon as a frame is received on the connection.
    entry->priority = Priority::System;
    entry->pendingPriority = Priority::System;
    entry->retransmitCount = 0;
    entry->lastTxTimeMs = 0;
    entry->retransmitTimeoutMs = INITIAL_RETRANSMIT_TIMEOUT;
    
    // Initialize state machine
    ConnectionParams adjustedParams = params;
    adjustedParams.remoteAddress = remoteAddress;
    
    auto smInit = entry->stateMachine->init(adjustedParams);
    if (!smInit) {
        KNX_LOGE(TAG, "Failed to initialize state machine for 0x%04X", remoteAddress.raw);
        recycle(std::move(entry));
        return ConnectionIndex::invalid();
    }
    
    ConnectionIndex index(_nextIndex);
    _connections.push_back(std::move(entry));
    
    // Increment for next connection
    _nextIndex = (_nextIndex + 1) % MAX_CONNECTIONS;
    
    KNX_LOGD(TAG, "Created connection %d to address 0x%04X", index.value(), remoteAddress.raw);
    
    return index;
}

ConnectionEntry* ConnectionTable::getConnection(ConnectionIndex index) {
    
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [index](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->index == index;
        });
    
    return (it != _connections.end()) ? it->get() : nullptr;
}

const ConnectionEntry* ConnectionTable::getConnection(ConnectionIndex index) const {
    
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [index](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->index == index;
        });
    
    return (it != _connections.end()) ? it->get() : nullptr;
}

ConnectionEntry* ConnectionTable::findConnection(const IndividualAddress& remoteAddress) {
    
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [remoteAddress](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->remoteAddress.raw == remoteAddress.raw;
        });
    
    return (it != _connections.end()) ? it->get() : nullptr;
}

const ConnectionEntry* ConnectionTable::findConnection(const IndividualAddress& remoteAddress) const {
    
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [remoteAddress](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->remoteAddress.raw == remoteAddress.raw;
        });
    
    return (it != _connections.end()) ? it->get() : nullptr;
}

util::Result<void> ConnectionTable::removeConnection(ConnectionIndex index) {
    
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [index](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->index == index;
        });
    
    if (it != _connections.end()) {
        uint16_t addr = (*it)->remoteAddress.raw;
        eraseEntry(it);
        notifyClosed(IndividualAddress(addr));
        KNX_LOGD(TAG, "Removed connection %d (address 0x%04X)", index.value(), addr);
        return util::Result<void>::ok();
    }
    
    return util::ErrorCode::OperationFailed;
}

util::Result<void> ConnectionTable::removeConnectionByAddress(const IndividualAddress& remoteAddress) {
    
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [remoteAddress](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->remoteAddress.raw == remoteAddress.raw;
        });
    
    if (it != _connections.end()) {
        ConnectionIndex index = (*it)->index;
        // A copy: the caller may have passed the entry's own field.
        const IndividualAddress remote = remoteAddress;
        eraseEntry(it);
        notifyClosed(remote);
        KNX_LOGD(TAG, "Removed connection %d (address 0x%04X)", index.value(), remote.raw);
        return util::Result<void>::ok();
    }
    
    return util::ErrorCode::OperationFailed;
}

size_t ConnectionTable::checkTimeouts(uint32_t currentTimeMs, uint32_t timeoutMs) {
    
    size_t timedOut = 0;
    
    // Iterate in reverse to safely remove while iterating
    for (int i = static_cast<int>(_connections.size()) - 1; i >= 0; --i) {
        const auto& entry = _connections[i];
        
        // Skip if no activity time set (connection just created)
        if (entry->lastActivityTimeMs == 0) {
            continue;
        }
        
        uint32_t elapsed = currentTimeMs - entry->lastActivityTimeMs;
        if (elapsed > timeoutMs) {
            KNX_LOGW(TAG, "Connection %d to 0x%04X timed out (%lu ms)",
                     entry->index.value(), entry->remoteAddress.raw,
                     static_cast<unsigned long>(elapsed));
            const IndividualAddress remote = entry->remoteAddress;
            eraseEntry(_connections.begin() + i);
            notifyClosed(remote);
            timedOut++;
        }
    }
    
    return timedOut;
}

void ConnectionTable::updateActivityTime(ConnectionIndex index, uint32_t timeMs) {
    
    // Find connection without locking (we already have the lock)
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [index](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->index == index;
        });
    
    if (it != _connections.end()) {
        (*it)->lastActivityTimeMs = timeMs;
    }
}

util::Result<void> ConnectionTable::markDataSent(ConnectionIndex index) {
    
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [index](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->index == index;
        });
    
    if (it == _connections.end()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    (*it)->retransmitCount++;
    (*it)->lastTxTimeMs = 0;  // Will be set by transport layer timer
    return util::Result<void>::ok();
}

util::Result<void> ConnectionTable::resetRetransmitState(ConnectionIndex index) {
    
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [index](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->index == index;
        });
    
    if (it == _connections.end()) {
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }
    
    (*it)->pendingRetransmission = false;
    (*it)->retransmitCount = 0;
    (*it)->lastTxTimeMs = 0;
    (*it)->retransmitTimeoutMs = INITIAL_RETRANSMIT_TIMEOUT;
    (*it)->pendingData.clear();
    return util::Result<void>::ok();
}

uint8_t ConnectionTable::getRetransmitCount(ConnectionIndex index) const {
    
    auto it = std::find_if(_connections.begin(), _connections.end(),
        [index](const std::unique_ptr<ConnectionEntry>& entry) {
            return entry->index == index;
        });
    
    return (it != _connections.end()) ? (*it)->retransmitCount : 0;
}

util::FixedVector<ConnectionIndex, ConnectionTable::MAX_CONNECTIONS> ConnectionTable::processRetransmissions(
    uint32_t currentTimeMs,
    util::FixedVector<ClosedConnection, MAX_CONNECTIONS>* closed) {
    util::FixedVector<ConnectionIndex, ConnectionTable::MAX_CONNECTIONS> needsRetransmit;

    auto it = _connections.begin();
    while (it != _connections.end()) {
        auto& entry = *it;
        if (!entry || !entry->pendingRetransmission) {
            ++it;
            continue;
        }

        // The acknowledge timeout is fixed (03_03_04 §5, A7/A9 "start the
        // acknowledge timeout timer"); each repetition waits the same 3 s.
        const uint32_t timeSinceTx = currentTimeMs - entry->lastTxTimeMs;
        if (timeSinceTx < entry->retransmitTimeoutMs) {
            ++it;
            continue;
        }

        if (entry->retransmitCount < entry->maxRetries) {
            // E17 → A9: repeat the stored frame.
            (void)needsRetransmit.push_back(entry->index);
            ++it;
            continue;
        }

        // E18 (acknowledge timeout with rep_count >= max_rep_count) → A6:
        // CLOSED, with a T_Disconnect to the partner. Clearing the pending
        // frame but keeping the connection, as this used to do, left OPEN_IDLE
        // with a sequence number the partner never saw acknowledged, so every
        // later frame on the connection was out of step.
        KNX_LOGW(TAG, "Connection %d to 0x%04X: no T_ACK after %u repetitions, closing",
                 entry->index.value(), entry->remoteAddress.raw,
                 static_cast<unsigned>(entry->retransmitCount));
        if (closed) {
            (void)closed->push_back(ClosedConnection{entry->index, entry->remoteAddress, entry->priority});
        }
        const IndividualAddress remote = entry->remoteAddress;
        it = eraseEntry(it);
        notifyClosed(remote);
    }

    return needsRetransmit;
}

util::FixedVector<ClosedConnection, ConnectionTable::MAX_CONNECTIONS> ConnectionTable::processTimeouts(uint32_t currentTimeMs, uint32_t timeoutMs) {
    util::FixedVector<ClosedConnection, ConnectionTable::MAX_CONNECTIONS> timedOut;
    auto it = _connections.begin();

    while (it != _connections.end()) {
        auto& entry = *it;
        if (!entry) {
            ++it;
            continue;
        }

        uint32_t idleTime = currentTimeMs - entry->lastActivityTimeMs;
        if (idleTime >= timeoutMs) {
            (void)timedOut.push_back(ClosedConnection{entry->index, entry->remoteAddress, entry->priority});
            KNX_LOGW(TAG, "Connection %d timed out after %lums idle",
                     entry->index.value(), static_cast<unsigned long>(idleTime));
            const IndividualAddress remote = entry->remoteAddress;
            it = eraseEntry(it);
            notifyClosed(remote);
        } else {
            ++it;
        }
    }
    
    return timedOut;
}

ConnectionIndex ConnectionTable::findAvailableIndex() {
    // Simple: find first unused index
    for (uint8_t i = 0; i < MAX_CONNECTIONS; ++i) {
        if (getConnection(ConnectionIndex(i)) == nullptr) {
            return ConnectionIndex(i);
        }
    }
    return ConnectionIndex::invalid();
}

util::Result<bool> ConnectionTable::isDuplicate(ConnectionIndex index, uint8_t seqNum) {
    auto entry = getConnection(index);
    if (!entry) {
        KNX_LOGW(TAG, "isDuplicate: Connection %d not found", index.value());
        return util::ErrorCode::InvalidParameter;
    }
    
    // Mask to 4-bit
    seqNum &= 0x0F;
    
    // Check if this is same as last received
    if (entry->lastReceivedSeq == seqNum) {
        KNX_LOGD(TAG, "Duplicate packet detected: seq=%d", seqNum);
        return true;
    }
    
    // Not a duplicate - update last received
    entry->lastReceivedSeq = seqNum;
    return false;
}

} // namespace transport
} // namespace knx
