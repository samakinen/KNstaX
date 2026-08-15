// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/transport/transport_layer.hpp"
#include "knx/util/log.hpp"
#include "knx/protocol/tpdu_codec.hpp"

static const char* TAG = "KNX.Transport";

namespace knx {
namespace transport {

util::Result<void> TransportLayer::buildNetworkTxFrame(const TDataFrame& frame, network::NDataFrame& nFrame) {
    TDataFrame work = frame;

    // Resolve the connection's TX sequence number before the transform runs:
    // KNX Data Secure binds the outgoing TPCI into the CCM block B0, so the
    // transform has to know which TPCI this frame will carry (03/03/07 §5.1.3.2
    // and NOTE 15, which calls this out as the awkward part of a device-side
    // implementation).
    ConnectionEntry* connection = nullptr;
    if (work.service == TDataService::Connected
            && work.destinationType == AddressType::Individual
            && !work.tpdu.empty()) {
        ConnectionEntry* candidate = _connectionTable.findConnection(IndividualAddress(work.destination.raw));
        if (candidate && candidate->stateMachine
                && candidate->stateMachine->state() == ConnectionState::Connected) {
            connection = candidate;
            work.securityTpci6 = static_cast<uint8_t>(
                protocol::TPCIField::create(protocol::TPCI::NumberedData,
                                            connection->sequenceNumber & 0x0Fu).raw >> 2);
        }
    }

    if (_txTransform) {
        auto txRes = _txTransform(work);
        if (txRes.isError()) {
            return txRes.error();
        }
    }

    nFrame.dlFrame.source = work.source;
    nFrame.dlFrame.destination = work.destination;
    nFrame.dlFrame.destinationType = work.destinationType;
    nFrame.dlFrame.tpdu = work.tpdu;
    nFrame.dlFrame.standardFrame = work.standardFrame;
    nFrame.dlFrame.repeated = work.repeated;
    nFrame.dlFrame.priority = work.priority;
    nFrame.dlFrame.ackRequested = work.ackRequested;
    nFrame.dlFrame.confirmation = work.confirmation;
    nFrame.dlFrame.hopCount = work.hopCount;

    // T_Data_Connected: the application layer always builds the TPDU with
    // UnnumberedData, so the transport layer rewrites the TPCI to NumberedData
    // with the connection's TX sequence number.
    //
    // Only frames explicitly marked TDataService::Connected are put on the
    // connection. Rewriting every individual-addressed frame while a connection
    // happens to be open would hijack connectionless answers: 03/03/04 §5 maps
    // the connectionless services independently of the connection state machine,
    // and ETS's IA-programming final check reads the device descriptor with a
    // connectionless A_DeviceDescriptor_Read — answering it on the connection
    // (and burning a SeqNoSend) leaves that check unanswered.
    if (connection != nullptr && !work.tpdu.empty()) {
        ConnectionEntry* conn = connection;
        const uint8_t tpdu0 = work.tpdu[0];
        const uint8_t tpdu1 = work.tpdu.size() > 1u ? work.tpdu[1] : 0u;
        const auto header = protocol::unpackTpduHeader(tpdu0, tpdu1);
        const size_t payloadStart = work.tpdu.size() >= 2u ? 2u : work.tpdu.size();
        const std::span<const uint8_t> payload(work.tpdu.data() + payloadStart,
                                               work.tpdu.size() - payloadStart);
        auto buildResult = protocol::buildTpduInPlace(
            protocol::TPCIField::create(protocol::TPCI::NumberedData, conn->sequenceNumber & 0x0Fu),
            header.apci,
            payload,
            nFrame.dlFrame.tpdu);
        if (buildResult.isError()) {
            return buildResult.error();
        }
        conn->pendingSeq = conn->sequenceNumber & 0x0Fu;
        conn->pendingRetransmission = true;
        conn->pendingApciRaw = header.apci.raw;
        conn->pendingPriority = work.priority;
        (void)conn->pendingData.assign(payload);
        conn->retransmitCount = 0;
        conn->lastTxTimeMs = nowMs();
        conn->retransmitTimeoutMs = constants::timing::ACK_TIMEOUT_MS;
    }

    return util::Result<void>::ok();
}

uint8_t TransportLayer::connectedTxTpci6(const IndividualAddress& peer) const {
    const ConnectionEntry* conn = _connectionTable.findConnection(peer);
    if (!conn || !conn->stateMachine || conn->stateMachine->state() != ConnectionState::Connected) {
        return 0;
    }
    return static_cast<uint8_t>(
        protocol::TPCIField::create(protocol::TPCI::NumberedData,
                                    conn->sequenceNumber & 0x0Fu).raw >> 2);
}

util::Result<void> TransportLayer::sendFrame(const TDataFrame& frame) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }

    network::NDataFrame nFrame;
    auto buildRes = buildNetworkTxFrame(frame, nFrame);
    if (buildRes.isError()) {
        return buildRes.error();
    }

    return _network.sendFrame(nFrame);
}

util::Result<void> TransportLayer::beginTransmit(const TDataFrame& frame) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }
    if (_sendOperation.active) {
        return util::ErrorCode::Busy;
    }

    network::NDataFrame nFrame;
    auto buildRes = buildNetworkTxFrame(frame, nFrame);
    if (buildRes.isError()) {
        return buildRes.error();
    }

    auto beginResult = _network.beginTransmit(nFrame);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    _sendOperation.active = true;
    return util::Result<void>::ok();
}

void TransportLayer::processBackgroundWork() {
    if (!_initialized) {
        return;
    }

    auto progress = _network.pollForwarding();
    if (progress.isError()) {
        KNX_LOGW(TAG, "Background network maintenance failed: %s",
                 util::errorCodeToString(progress.error()));
    }
}

util::Result<TransportLayer::SendProgressState> TransportLayer::pollTransmit() {
    processBackgroundWork();

    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_sendOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    auto progress = _network.pollTransmit();
    if (progress.isError()) {
        finishSendOperation();
        return progress.error();
    }

    switch (progress.value()) {
        case network::NetworkLayer::TxProgressState::Pending:
            return SendProgressState::Pending;
        case network::NetworkLayer::TxProgressState::Success:
            finishSendOperation();
            return SendProgressState::Success;
        case network::NetworkLayer::TxProgressState::Busy:
            finishSendOperation();
            return SendProgressState::Busy;
        case network::NetworkLayer::TxProgressState::TransmissionFailed:
            finishSendOperation();
            return SendProgressState::TransmissionFailed;
        case network::NetworkLayer::TxProgressState::Timeout:
            finishSendOperation();
            return SendProgressState::Timeout;
    }

    finishSendOperation();
    return util::ErrorCode::OperationFailed;
}

util::Result<ConnectionIndex> TransportLayer::connect(const IndividualAddress& remoteAddress) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }

    if (_connectionTable.findConnection(remoteAddress) != nullptr) {
        KNX_LOGW(TAG, "Already connected to 0x%04X", remoteAddress.raw);
        return util::ErrorCode::AlreadyInitialized;
    }

    ConnectionParams params;
    params.remoteAddress = remoteAddress;
    params.timeoutMs = constants::timing::CONNECTION_TIMEOUT_MS;
    params.retransmitTimeoutMs = constants::timing::ACK_TIMEOUT_MS;
    params.maxRetries = constants::timing::CONNECTION_MAX_REPETITIONS;

    ConnectionIndex connIdx = _connectionTable.createConnection(remoteAddress, params);
    if (!connIdx.isValid()) {
        KNX_LOGE(TAG, "Failed to create connection to 0x%04X", remoteAddress.raw);
        return util::ErrorCode::ResourceUnavailable;
    }

    auto sendRes = sendConnect(remoteAddress);
    if (!sendRes) {
        (void)_connectionTable.removeConnection(connIdx);
        return sendRes.error();
    }

    ConnectionEntry* entry = _connectionTable.getConnection(connIdx);
    if (entry && entry->stateMachine) {
        (void)entry->stateMachine->handleEvent(ConnectionEvent::ConnectRequest);
        _connectionTable.updateActivityTime(connIdx, nowMs());
    }

    KNX_LOGI(TAG, "Connecting to 0x%04X (connection %d)", remoteAddress.raw, connIdx.value());
    return connIdx;
}

util::Result<ConnectionIndex> TransportLayer::beginConnect(const IndividualAddress& remoteAddress) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }
    if (_connectOperation.active) {
        return util::ErrorCode::Busy;
    }
    if (_connectionTable.findConnection(remoteAddress) != nullptr) {
        KNX_LOGW(TAG, "Already connected to 0x%04X", remoteAddress.raw);
        return util::ErrorCode::AlreadyInitialized;
    }

    ConnectionParams params;
    params.remoteAddress = remoteAddress;
    params.timeoutMs = constants::timing::CONNECTION_TIMEOUT_MS;
    params.retransmitTimeoutMs = constants::timing::ACK_TIMEOUT_MS;
    params.maxRetries = constants::timing::CONNECTION_MAX_REPETITIONS;

    ConnectionIndex connIdx = _connectionTable.createConnection(remoteAddress, params);
    if (!connIdx.isValid()) {
        KNX_LOGE(TAG, "Failed to create connection to 0x%04X", remoteAddress.raw);
        return util::ErrorCode::ResourceUnavailable;
    }

    network::NDataFrame nFrame;
    nFrame.dlFrame.source = _ownAddress;
    nFrame.dlFrame.destination = GroupAddress(remoteAddress.raw);
    nFrame.dlFrame.destinationType = AddressType::Individual;
    auto buildConnectTpdu = knx::protocol::buildTpduInPlace(protocol::TPCIField::create(protocol::TPCIControl::Connect),
                                                            application::APCIField(0),
                                                            {},
                                                            nFrame.dlFrame.tpdu);
    if (buildConnectTpdu.isError()) {
        (void)_connectionTable.removeConnection(connIdx);
        return buildConnectTpdu.error();
    }
    nFrame.dlFrame.hopCount = 6;
    // Client-initiated management connection: System priority (as certified
    // devices and ETS use for connection-oriented commissioning traffic).
    nFrame.dlFrame.priority = Priority::System;

    auto beginResult = _network.beginTransmit(nFrame);
    if (beginResult.isError()) {
        (void)_connectionTable.removeConnection(connIdx);
        return beginResult.error();
    }

    _connectOperation.active = true;
    _connectOperation.createdConnection = true;
    _connectOperation.connectionIndex = connIdx;
    _connectOperation.remoteAddress = remoteAddress;
    return connIdx;
}

util::Result<TransportLayer::ControlSendProgressState> TransportLayer::pollConnect() {
    processBackgroundWork();

    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_connectOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    auto progress = _network.pollTransmit();
    if (progress.isError()) {
        if (_connectOperation.createdConnection && _connectOperation.connectionIndex.isValid()) {
            (void)_connectionTable.removeConnection(_connectOperation.connectionIndex);
        }
        finishConnectOperation();
        return progress.error();
    }

    switch (progress.value()) {
        case network::NetworkLayer::TxProgressState::Pending:
            return ControlSendProgressState::Pending;
        case network::NetworkLayer::TxProgressState::Success: {
            ConnectionEntry* entry = _connectionTable.getConnection(_connectOperation.connectionIndex);
            if (entry && entry->stateMachine) {
                (void)entry->stateMachine->handleEvent(ConnectionEvent::ConnectRequest);
                _connectionTable.updateActivityTime(_connectOperation.connectionIndex, nowMs());
            }
            finishConnectOperation();
            return ControlSendProgressState::Success;
        }
        case network::NetworkLayer::TxProgressState::Busy:
            if (_connectOperation.createdConnection && _connectOperation.connectionIndex.isValid()) {
                (void)_connectionTable.removeConnection(_connectOperation.connectionIndex);
            }
            finishConnectOperation();
            return ControlSendProgressState::Busy;
        case network::NetworkLayer::TxProgressState::TransmissionFailed:
            if (_connectOperation.createdConnection && _connectOperation.connectionIndex.isValid()) {
                (void)_connectionTable.removeConnection(_connectOperation.connectionIndex);
            }
            finishConnectOperation();
            return ControlSendProgressState::TransmissionFailed;
        case network::NetworkLayer::TxProgressState::Timeout:
            if (_connectOperation.createdConnection && _connectOperation.connectionIndex.isValid()) {
                (void)_connectionTable.removeConnection(_connectOperation.connectionIndex);
            }
            finishConnectOperation();
            return ControlSendProgressState::Timeout;
    }

    finishConnectOperation();
    return util::ErrorCode::OperationFailed;
}

util::Result<void> TransportLayer::disconnect(ConnectionIndex connectionIndex) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }

    ConnectionEntry* entry = _connectionTable.getConnection(connectionIndex);
    if (!entry) {
        KNX_LOGE(TAG, "Invalid connection index %d", connectionIndex.value());
        return util::ErrorCode::InvalidParameter;
    }

    IndividualAddress remoteAddr = entry->remoteAddress;
    auto sendRes = sendDisconnect(remoteAddr, entry->priority);
    if (!sendRes) {
        return sendRes.error();
    }

    if (entry->stateMachine) {
        (void)entry->stateMachine->handleEvent(ConnectionEvent::DisconnectRequest);
        _connectionTable.updateActivityTime(connectionIndex, nowMs());
    }

    KNX_LOGI(TAG, "Disconnecting from 0x%04X (connection %d)", remoteAddr.raw, connectionIndex.value());
    return util::Result<void>::ok();
}

util::Result<void> TransportLayer::sendConnectedData(ConnectionIndex connectionIndex, std::span<const uint8_t> data) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (data.size() > config::MAX_APDU_LENGTH) {
        return util::ErrorCode::InvalidParameter;
    }

    ConnectionEntry* entry = _connectionTable.getConnection(connectionIndex);
    if (!entry || !entry->stateMachine) {
        return util::ErrorCode::InvalidParameter;
    }
    if (!entry->stateMachine->isConnected()) {
        KNX_LOGW(TAG, "Connection %d not established", connectionIndex.value());
        return util::ErrorCode::OperationNotReady;
    }
    if (entry->pendingRetransmission && !entry->pendingData.empty()) {
        KNX_LOGW(TAG, "Connection %d has pending data awaiting ACK", connectionIndex.value());
        return util::ErrorCode::ResourceUnavailable;
    }

    uint8_t seqNum = entry->sequenceNumber & 0x0F;

    network::NDataFrame nFrame;
    nFrame.dlFrame.source = _ownAddress;
    nFrame.dlFrame.destination = GroupAddress(entry->remoteAddress.raw);
    nFrame.dlFrame.destinationType = AddressType::Individual;
    auto buildDataTpdu = knx::protocol::buildTpduInPlace(protocol::TPCIField::create(protocol::TPCI::NumberedData, seqNum),
                                                         application::APCIField(0),
                                                         data,
                                                         nFrame.dlFrame.tpdu);
    if (buildDataTpdu.isError()) {
        return buildDataTpdu.error();
    }
    nFrame.dlFrame.hopCount = 6;
    nFrame.dlFrame.priority = entry->priority;
    nFrame.dlFrame.ackRequested = false;

    if (!entry->pendingData.assign(data)) {
        return util::ErrorCode::InvalidParameter;
    }
    entry->pendingApciRaw = 0;
    entry->pendingPriority = entry->priority;
    entry->pendingSeq = seqNum;
    entry->pendingRetransmission = true;
    entry->retransmitCount = 0;
    entry->lastTxTimeMs = nowMs();
    entry->retransmitTimeoutMs = constants::timing::ACK_TIMEOUT_MS;

    auto sendRes = _network.sendFrame(nFrame);
    if (sendRes) {
        entry->sequenceNumber = (seqNum + 1) & 0x0F;
        _connectionTable.updateActivityTime(connectionIndex, nowMs());
        return util::Result<void>::ok();
    }

    return sendRes.error();
}

util::Result<void> TransportLayer::beginSendConnectedData(ConnectionIndex connectionIndex, std::span<const uint8_t> data) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (data.size() > config::MAX_APDU_LENGTH) {
        return util::ErrorCode::InvalidParameter;
    }
    if (_connectedSendOperation.active) {
        return util::ErrorCode::Busy;
    }

    ConnectionEntry* entry = _connectionTable.getConnection(connectionIndex);
    if (!entry || !entry->stateMachine) {
        return util::ErrorCode::InvalidParameter;
    }
    if (!entry->stateMachine->isConnected()) {
        KNX_LOGW(TAG, "Connection %d not established", connectionIndex.value());
        return util::ErrorCode::OperationNotReady;
    }
    if (entry->pendingRetransmission && !entry->pendingData.empty()) {
        KNX_LOGW(TAG, "Connection %d has pending data awaiting ACK", connectionIndex.value());
        return util::ErrorCode::ResourceUnavailable;
    }

    const uint8_t seqNum = entry->sequenceNumber & 0x0F;
    network::NDataFrame nFrame;
    nFrame.dlFrame.source = _ownAddress;
    nFrame.dlFrame.destination = GroupAddress(entry->remoteAddress.raw);
    nFrame.dlFrame.destinationType = AddressType::Individual;
    auto buildAsyncDataTpdu = knx::protocol::buildTpduInPlace(protocol::TPCIField::create(protocol::TPCI::NumberedData, seqNum),
                                                              application::APCIField(0),
                                                              data,
                                                              nFrame.dlFrame.tpdu);
    if (buildAsyncDataTpdu.isError()) {
        return buildAsyncDataTpdu.error();
    }
    nFrame.dlFrame.hopCount = 6;
    nFrame.dlFrame.priority = entry->priority;
    nFrame.dlFrame.ackRequested = false;

    auto beginResult = _network.beginTransmit(nFrame);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    _connectedSendOperation.active = true;
    _connectedSendOperation.connectionIndex = connectionIndex;
    _connectedSendOperation.sequenceNumber = seqNum;
    if (!_connectedSendOperation.payload.assign(data)) {
        finishConnectedSendOperation();
        return util::ErrorCode::InvalidParameter;
    }
    return util::Result<void>::ok();
}

util::Result<TransportLayer::ConnectedSendProgressState> TransportLayer::pollConnectedDataSend() {
    processBackgroundWork();

    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_connectedSendOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    auto progress = _network.pollTransmit();
    if (progress.isError()) {
        finishConnectedSendOperation();
        return progress.error();
    }

    switch (progress.value()) {
        case network::NetworkLayer::TxProgressState::Pending:
            return ConnectedSendProgressState::Pending;
        case network::NetworkLayer::TxProgressState::Success: {
            ConnectionEntry* entry = _connectionTable.getConnection(_connectedSendOperation.connectionIndex);
            if (!entry || !entry->stateMachine || !entry->stateMachine->isConnected()) {
                finishConnectedSendOperation();
                return util::ErrorCode::OperationFailed;
            }

            entry->pendingData = _connectedSendOperation.payload;
            entry->pendingApciRaw = 0;
            entry->pendingPriority = entry->priority;
            entry->pendingSeq = _connectedSendOperation.sequenceNumber;
            entry->pendingRetransmission = true;
            entry->retransmitCount = 0;
            entry->lastTxTimeMs = nowMs();
            entry->retransmitTimeoutMs = constants::timing::ACK_TIMEOUT_MS;
            entry->sequenceNumber = (_connectedSendOperation.sequenceNumber + 1u) & 0x0F;
            _connectionTable.updateActivityTime(_connectedSendOperation.connectionIndex, nowMs());

            finishConnectedSendOperation();
            return ConnectedSendProgressState::Success;
        }
        case network::NetworkLayer::TxProgressState::Busy:
            finishConnectedSendOperation();
            return ConnectedSendProgressState::Busy;
        case network::NetworkLayer::TxProgressState::TransmissionFailed:
            finishConnectedSendOperation();
            return ConnectedSendProgressState::TransmissionFailed;
        case network::NetworkLayer::TxProgressState::Timeout:
            finishConnectedSendOperation();
            return ConnectedSendProgressState::Timeout;
    }

    finishConnectedSendOperation();
    return util::ErrorCode::OperationFailed;
}

bool TransportLayer::isConnected(ConnectionIndex connectionIndex) const {
    const ConnectionEntry* entry = _connectionTable.getConnection(connectionIndex);
    if (!entry || !entry->stateMachine) {
        return false;
    }
    return entry->stateMachine->isConnected();
}

void TransportLayer::processRetransmissions(uint32_t currentTimeMs) {
    if (!_initialized) {
        return;
    }

    processBackgroundWork();
    _timebaseMs = currentTimeMs;

    auto needsRetransmit = _connectionTable.processRetransmissions(currentTimeMs);
    for (ConnectionIndex connIdx : needsRetransmit) {
        auto* entry = _connectionTable.getConnection(connIdx);
        if (!entry || entry->pendingData.empty() || !entry->pendingRetransmission) {
            continue;
        }

        KNX_LOGD(TAG, "Retransmitting data on connection %d", connIdx.value());
        network::NDataFrame nFrame;
        nFrame.dlFrame.source = _ownAddress;
        nFrame.dlFrame.destination = GroupAddress(entry->remoteAddress.raw);
        nFrame.dlFrame.destinationType = AddressType::Individual;
        auto buildRetransmitTpdu = knx::protocol::buildTpduInPlace(
            protocol::TPCIField::create(protocol::TPCI::NumberedData, entry->pendingSeq & 0x0F),
            application::APCIField(entry->pendingApciRaw),
            entry->pendingData,
            nFrame.dlFrame.tpdu
        );
        if (buildRetransmitTpdu.isError()) {
            continue;
        }
        nFrame.dlFrame.hopCount = 6;
        nFrame.dlFrame.priority = entry->pendingPriority;
        nFrame.dlFrame.ackRequested = false;

        if (_network.sendFrame(nFrame).isOk()) {
            (void)_connectionTable.markDataSent(connIdx);
            entry->lastTxTimeMs = currentTimeMs;
            _connectionTable.updateActivityTime(connIdx, currentTimeMs);
        }
    }

    auto timedOut = _connectionTable.processTimeouts(currentTimeMs, constants::timing::CONNECTION_TIMEOUT_MS);
    for (ConnectionIndex connIdx : timedOut) {
        KNX_LOGW(TAG, "Connection %d timed out and was removed", connIdx.value());
    }
}

util::Result<void> TransportLayer::beginProcessRetransmissions(uint32_t currentTimeMs) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (_retransmissionOperation.active) {
        return util::ErrorCode::Busy;
    }

    _timebaseMs = currentTimeMs;
    finishRetransmissionOperation();
    _retransmissionOperation.active = true;
    _retransmissionOperation.currentTimeMs = currentTimeMs;

    auto needsRetransmit = _connectionTable.processRetransmissions(currentTimeMs);
    for (size_t i = 0; i < needsRetransmit.size() && i < _retransmissionOperation.pending.size(); ++i) {
        _retransmissionOperation.pending[i] = needsRetransmit[i];
        ++_retransmissionOperation.pendingCount;
    }

    return util::Result<void>::ok();
}

util::Result<TransportLayer::ControlSendProgressState> TransportLayer::pollProcessRetransmissions() {
    processBackgroundWork();

    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_retransmissionOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    if (_retransmissionOperation.sendingCurrent) {
        const ConnectionIndex connIdx = _retransmissionOperation.currentConnection;
        auto progress = _network.pollTransmit();
        if (progress.isError()) {
            finishRetransmissionOperation();
            return progress.error();
        }

        switch (progress.value()) {
            case network::NetworkLayer::TxProgressState::Pending:
                return ControlSendProgressState::Pending;
            case network::NetworkLayer::TxProgressState::Success:
                (void)_connectionTable.markDataSent(connIdx);
                if (auto* updated = _connectionTable.getConnection(connIdx)) {
                    updated->lastTxTimeMs = _retransmissionOperation.currentTimeMs;
                }
                _connectionTable.updateActivityTime(connIdx, _retransmissionOperation.currentTimeMs);
                _retransmissionOperation.sendingCurrent = false;
                _retransmissionOperation.currentConnection = ConnectionIndex::invalid();
                break;
            case network::NetworkLayer::TxProgressState::Busy:
                finishRetransmissionOperation();
                return ControlSendProgressState::Busy;
            case network::NetworkLayer::TxProgressState::TransmissionFailed:
                finishRetransmissionOperation();
                return ControlSendProgressState::TransmissionFailed;
            case network::NetworkLayer::TxProgressState::Timeout:
                finishRetransmissionOperation();
                return ControlSendProgressState::Timeout;
        }
    }

    while (_retransmissionOperation.nextIndex < _retransmissionOperation.pendingCount) {
        const ConnectionIndex connIdx = _retransmissionOperation.pending[_retransmissionOperation.nextIndex++];
        auto* entry = _connectionTable.getConnection(connIdx);
        if (!entry || entry->pendingData.empty() || !entry->pendingRetransmission) {
            continue;
        }

        network::NDataFrame nFrame;
        nFrame.dlFrame.source = _ownAddress;
        nFrame.dlFrame.destination = GroupAddress(entry->remoteAddress.raw);
        nFrame.dlFrame.destinationType = AddressType::Individual;
        auto buildDeferredRetransmitTpdu = knx::protocol::buildTpduInPlace(
            protocol::TPCIField::create(protocol::TPCI::NumberedData, entry->pendingSeq & 0x0F),
            application::APCIField(entry->pendingApciRaw),
            entry->pendingData,
            nFrame.dlFrame.tpdu);
        if (buildDeferredRetransmitTpdu.isError()) {
            finishRetransmissionOperation();
            return ControlSendProgressState::TransmissionFailed;
        }
        nFrame.dlFrame.hopCount = 6;
        nFrame.dlFrame.priority = entry->pendingPriority;
        nFrame.dlFrame.ackRequested = false;

        auto beginResult = _network.beginTransmit(nFrame);
        if (beginResult.isError()) {
            finishRetransmissionOperation();
            if (beginResult.error() == util::ErrorCode::Busy) {
                return ControlSendProgressState::Busy;
            }
            if (beginResult.error() == util::ErrorCode::Timeout) {
                return ControlSendProgressState::Timeout;
            }
            return ControlSendProgressState::TransmissionFailed;
        }

        _retransmissionOperation.sendingCurrent = true;
        _retransmissionOperation.currentConnection = connIdx;
        return ControlSendProgressState::Pending;
    }

    auto timedOut = _connectionTable.processTimeouts(_retransmissionOperation.currentTimeMs, constants::timing::CONNECTION_TIMEOUT_MS);
    for (ConnectionIndex connIdx : timedOut) {
        KNX_LOGW(TAG, "Connection %d timed out and was removed", connIdx.value());
    }

    finishRetransmissionOperation();
    return ControlSendProgressState::Success;
}

} // namespace transport
} // namespace knx
