// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/transport/transport_layer.hpp"
#include "knx/util/log.hpp"
#include "knx/util/hex.hpp"
#include "knx/protocol/tpdu_codec.hpp"

static const char* TAG = "KNX.Transport";

namespace knx {
namespace transport {

util::Result<void> TransportLayer::beginControlResponse(const IndividualAddress& remoteAddress,
                                                        const protocol::TPCIField& tpci,
                                                        Priority priority) {
    network::NDataFrame nFrame;
    nFrame.dlFrame.source = _ownAddress;
    nFrame.dlFrame.destination = GroupAddress(remoteAddress.raw);
    nFrame.dlFrame.destinationType = AddressType::Individual;
    auto buildResult = knx::protocol::buildTpduInPlace(tpci,
                                                       application::APCIField(0),
                                                       {},
                                                       nFrame.dlFrame.tpdu);
    if (buildResult.isError()) {
        return buildResult.error();
    }
    nFrame.dlFrame.hopCount = 6;
    nFrame.dlFrame.repeated = false;  // first transmission
    nFrame.dlFrame.priority = priority;
    nFrame.dlFrame.ackRequested = false;
    return _network.beginTransmit(nFrame);
}

void TransportLayer::finalizeReceiveOperation() {
    if (!_receiveOperation.active) {
        return;
    }

    if (_receiveOperation.advanceExpectedSequence) {
        ConnectionEntry* entry = _connectionTable.getConnection(_receiveOperation.connectionIndex);
        if (entry) {
            entry->expectedRxSeq = (entry->expectedRxSeq + 1u) & 0x0F;
        }
    }

    if (_receiveOperation.deliverFrame) {
        // Same security hook as the synchronous path: connection-oriented
        // frames carry the ETS download, which on a secure device is secured.
        if (_rxTransform) {
            auto rxRes = _rxTransform(_receiveOperation.receivedFrame);
            if (rxRes.isError()) {
                if (rxRes.error() != util::ErrorCode::FrameConsumed) {
                    KNX_LOGW(TAG, "Transport RX transform rejected connected frame src=0x%04X tpdu=%s",
                             _receiveOperation.receivedFrame.source.raw,
                             util::formatHexBytes(_receiveOperation.receivedFrame.tpdu).c_str());
                }
                finishReceiveOperation();
                return;
            }
        }

        const auto* cb = _rxCallback.load(std::memory_order_acquire);
        if (cb) {
            (*cb)(_receiveOperation.receivedFrame);
        } else {
            enqueueReceivedFrame(_receiveOperation.receivedFrame);
        }
    }

    finishReceiveOperation();
}

void TransportLayer::beginHandleNumberedData(const IndividualAddress& remoteAddress,
                                             uint8_t seqNum,
                                             std::span<const uint8_t> tpdu,
                                             Priority priority) {
    ConnectionEntry* entry = _connectionTable.findConnection(remoteAddress);
    if (!entry || !entry->stateMachine) {
        KNX_LOGW(TAG, "Received numbered data from unconnected device 0x%04X", remoteAddress.raw);
        _receiveOperation.terminalState = ControlSendProgressState::Success;
        return;
    }

    seqNum &= 0x0F;
    entry->priority = priority;
    _connectionTable.updateActivityTime(entry->index, nowMs());
    _receiveOperation.connectionIndex = entry->index;

    auto dupRes = _connectionTable.isDuplicate(entry->index, seqNum);
    if (dupRes.isError()) {
        KNX_LOGW(TAG, "Duplicate check failed for connection %u", entry->index.value());
        _receiveOperation.terminalState = ControlSendProgressState::TransmissionFailed;
        return;
    }

    if (dupRes.value()) {
        KNX_LOGD(TAG, "Duplicate packet detected from 0x%04X, seq=%d - sending ACK",
                 remoteAddress.raw, seqNum);
        auto beginResult = beginControlResponse(remoteAddress, protocol::TPCIField::ack(seqNum), priority);
        if (beginResult.isOk()) {
            _receiveOperation.waitingResponse = true;
            return;
        }
        _receiveOperation.terminalState = mapTxErrorToProgressState(beginResult.error());
        return;
    }

    if (seqNum != (entry->expectedRxSeq & 0x0F)) {
        KNX_LOGW(TAG, "Sequence mismatch: expected %d, got %d",
                 entry->expectedRxSeq & 0x0F, seqNum);
        auto beginResult = beginControlResponse(remoteAddress,
                                                protocol::TPCIField::nak(entry->expectedRxSeq & 0x0F),
                                                priority);
        if (beginResult.isOk()) {
            _receiveOperation.waitingResponse = true;
            return;
        }
        _receiveOperation.terminalState = mapTxErrorToProgressState(beginResult.error());
        return;
    }

    TDataFrame tFrame;
    tFrame.service = TDataService::Connected;
    tFrame.source = remoteAddress;
    tFrame.destination = GroupAddress(_ownAddress.raw);
    tFrame.destinationType = AddressType::Individual;
    tFrame.sequenceNumber = seqNum;
    tFrame.tpdu.assign(tpdu);
    tFrame.securityTpci6 = tpdu.empty() ? 0u : static_cast<uint8_t>(tpdu[0] >> 2);

    _receiveOperation.deliverFrame = true;
    _receiveOperation.advanceExpectedSequence = true;
    _receiveOperation.receivedFrame = std::move(tFrame);

    auto beginResult = beginControlResponse(remoteAddress, protocol::TPCIField::ack(seqNum), priority);
    if (beginResult.isOk()) {
        _receiveOperation.waitingResponse = true;
        return;
    }

    _receiveOperation.terminalState = mapTxErrorToProgressState(beginResult.error());
}

void TransportLayer::handleNetworkRx(const network::NDataFrame& frame) {
    if (!_initialized) {
        return;
    }

    // Already logged once by the datalink layer's "RX ..." line.
    IndividualAddress remoteAddr = frame.dlFrame.source;
    if (frame.dlFrame.tpdu.empty()) {
        KNX_LOGW(TAG, "Received network frame too short, src=0x%04X dst=0x%04X",
                 frame.dlFrame.source.raw,
                 frame.dlFrame.destination.raw);
        return;
    }

    const uint8_t tpdu0 = frame.dlFrame.tpdu[0];
    const uint8_t tpdu1 = frame.dlFrame.tpdu.size() > 1u ? frame.dlFrame.tpdu[1] : 0u;
    const auto header = knx::protocol::unpackTpduHeader(tpdu0, tpdu1);
    const auto tpci = header.tpci;
    const auto tpciType = extractTPCI(tpci);

    if (_timebaseMs == 0) {
        _timebaseMs = 1;
    }

    if (tpciType == protocol::TPCI::UnnumberedControl) {
        if (tpci.isControl(protocol::TPCIControl::Connect)) {
            ConnectionEntry* entry = _connectionTable.findConnection(remoteAddr);
            if (entry && entry->stateMachine && entry->stateMachine->state() == ConnectionState::Connecting) {
                handleConnectResponse(remoteAddr);
            } else {
                handleConnectRequest(remoteAddr, frame.dlFrame.priority, frame.dlFrame.repeated);
            }
            return;
        } else if (tpci.isControl(protocol::TPCIControl::Disconnect)) {
            ConnectionEntry* entry = _connectionTable.findConnection(remoteAddr);
            if (entry && entry->stateMachine && entry->stateMachine->state() == ConnectionState::Disconnecting) {
                handleDisconnectResponse(remoteAddr);
            } else if (entry) {
                // Per KNX spec E02 in CLOSED state -> A0: ignore when no connection
                handleDisconnectRequest(remoteAddr);
            }
            return;
        }
    } else if (tpciType == protocol::TPCI::NumberedControl) {
        const uint8_t seqNum = extractSeqNum(tpci);
        if (tpci.isAck()) {
            handleAck(remoteAddr, seqNum);
            return;
        } else if (tpci.isNak()) {
            handleNak(remoteAddr, seqNum);
            return;
        }
    } else if (tpciType == protocol::TPCI::NumberedData) {
        const uint8_t seqNum = extractSeqNum(tpci);
        KNX_LOGD(TAG, "Transport TPCI numbered data from 0x%04X seq=%u", remoteAddr.raw, seqNum);
        handleNumberedData(remoteAddr, seqNum, frame.dlFrame.tpdu, frame.dlFrame.priority);
        return;
    }

    TDataFrame tFrame = convertFromNetwork(frame);
    tFrame.securityTpci6 = static_cast<uint8_t>(tpdu0 >> 2);
    if (_rxTransform) {
        auto rxRes = _rxTransform(tFrame);
        if (rxRes.isError()) {
            if (rxRes.error() == util::ErrorCode::FrameConsumed) {
                // Handled by the transform itself (a KNX Data Secure sync, say),
                // which is a normal outcome rather than a rejection.
                return;
            }
            KNX_LOGW(TAG, "Transport RX transform rejected frame src=0x%04X dst=0x%04X tpdu=%s",
                     tFrame.source.raw,
                     tFrame.destination.raw,
                     util::formatHexBytes(tFrame.tpdu).c_str());
            return;
        }
    }
    const auto* cb = _rxCallback.load(std::memory_order_acquire);
    if (cb) {
        (*cb)(tFrame);
    } else {
        enqueueReceivedFrame(tFrame);
    }
}

util::Result<void> TransportLayer::beginHandleNetworkRx(const network::NDataFrame& frame) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (_receiveOperation.active) {
        return util::ErrorCode::Busy;
    }

    finishReceiveOperation();
    _receiveOperation.active = true;
    _receiveOperation.terminalState = ControlSendProgressState::Success;

    const IndividualAddress remoteAddr = frame.dlFrame.source;
    if (frame.dlFrame.tpdu.empty()) {
        return util::Result<void>::ok();
    }

    const uint8_t tpdu0 = frame.dlFrame.tpdu[0];
    const uint8_t tpdu1 = frame.dlFrame.tpdu.size() > 1u ? frame.dlFrame.tpdu[1] : 0u;
    const auto header = knx::protocol::unpackTpduHeader(tpdu0, tpdu1);
    const auto tpci = header.tpci;
    const auto tpciType = extractTPCI(tpci);

    if (_timebaseMs == 0u) {
        _timebaseMs = 1u;
    }

    if (tpciType == protocol::TPCI::NumberedData) {
        beginHandleNumberedData(remoteAddr, extractSeqNum(tpci), frame.dlFrame.tpdu,
                                frame.dlFrame.priority);
        return util::Result<void>::ok();
    }

    handleNetworkRx(frame);
    return util::Result<void>::ok();
}

util::Result<TransportLayer::ControlSendProgressState> TransportLayer::pollHandleNetworkRx() {
    processBackgroundWork();

    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    if (!_receiveOperation.active) {
        return util::ErrorCode::OperationNotReady;
    }

    if (_receiveOperation.waitingResponse) {
        auto progress = _network.pollTransmit();
        if (progress.isError()) {
            _receiveOperation.waitingResponse = false;
            _receiveOperation.terminalState = mapTxErrorToProgressState(progress.error());
        } else {
            switch (progress.value()) {
                case network::NetworkLayer::TxProgressState::Pending:
                    return ControlSendProgressState::Pending;
                case network::NetworkLayer::TxProgressState::Success:
                    _receiveOperation.waitingResponse = false;
                    _receiveOperation.terminalState = ControlSendProgressState::Success;
                    break;
                case network::NetworkLayer::TxProgressState::Busy:
                    _receiveOperation.waitingResponse = false;
                    _receiveOperation.terminalState = ControlSendProgressState::Busy;
                    break;
                case network::NetworkLayer::TxProgressState::TransmissionFailed:
                    _receiveOperation.waitingResponse = false;
                    _receiveOperation.terminalState = ControlSendProgressState::TransmissionFailed;
                    break;
                case network::NetworkLayer::TxProgressState::Timeout:
                    _receiveOperation.waitingResponse = false;
                    _receiveOperation.terminalState = ControlSendProgressState::Timeout;
                    break;
            }
        }
    }

    const ControlSendProgressState terminalState = _receiveOperation.terminalState;
    finalizeReceiveOperation();
    return terminalState;
}

void TransportLayer::handleConnectRequest(const IndividualAddress& remoteAddress, Priority priority,
                                          bool repeatedFrame) {
    KNX_LOGD(TAG, "Received T_Connect from 0x%04X", remoteAddress.raw);

    ConnectionEntry* entry = _connectionTable.findConnection(remoteAddress);

    if (entry && entry->stateMachine) {
        entry->priority = priority;

        if (repeatedFrame && entry->stateMachine->state() == ConnectionState::Connected) {
            // DL-level retransmit of the T_CONNECT that opened this connection
            // (our L2 ACK was lost). Nothing to re-initialise: just refresh the
            // connection timeout. This is E00 → A0.
            _connectionTable.updateActivityTime(entry->index, nowMs());
            return;
        }

        // A first-transmission T_CONNECT from a peer we still hold a connection
        // for means the peer's own state machine went back to CLOSED (its 6 s
        // connection timeout elapsed, or it gave up on an unanswered request)
        // and it is opening a fresh connection with SeqNoSend = SeqNoRcv = 0.
        // 03/03/04 §5.4.2/§5.4.3 answer E00 in OPEN_IDLE with A0 (ignore),
        // which strands exactly this case: our stale SeqNoRcv NAKs every frame
        // of the new session until the peer disconnects — ETS reports a failed
        // download. Re-initialise the connection instead (A1 without the
        // T_Connect.ind), which is what the peer already assumes we did.
        KNX_LOGI(TAG, "T_Connect from already-connected 0x%04X — resynchronising connection",
                 remoteAddress.raw);
        entry->sequenceNumber = 0u;
        entry->expectedRxSeq = 0u;
        entry->pendingSeq = 0u;
        entry->lastReceivedSeq = 0xFFu;  // no frame received yet on the new session
        entry->pendingRetransmission = false;
        entry->pendingData.clear();
        entry->pendingApciRaw = 0u;
        entry->retransmitCount = 0u;
        entry->retransmitTimeoutMs = constants::timing::ACK_TIMEOUT_MS;
        entry->lastTxTimeMs = nowMs();
        // Back to Idle so the common accept path below drives the same
        // Idle → Connecting → Connected transitions a new connection takes.
        entry->stateMachine->reset();
    }

    if (!entry) {
        ConnectionParams params;
        params.remoteAddress = remoteAddress;
        params.timeoutMs = constants::timing::CONNECTION_TIMEOUT_MS;
        params.maxRetries = constants::timing::CONNECTION_MAX_REPETITIONS;

        ConnectionIndex idx = _connectionTable.createConnection(remoteAddress, params);
        if (idx.isValid()) {
            entry = _connectionTable.getConnection(idx);
        } else {
            // No slot free. A device tracks one connection (03/03/04: the state
            // machine handles one at a time), so this is a second management
            // client arriving while the first still holds the device.
            //
            // Answer T_Disconnect rather than dropping the frame: silence makes
            // the second client wait out its 6 s connection timeout and retry,
            // which looks like a flaky device. An explicit refusal tells it
            // immediately that the device is busy.
            KNX_LOGW(TAG,
                     "Refusing T_Connect from 0x%04X: no free connection slot "
                     "(device already connected)",
                     remoteAddress.raw);
            (void)sendDisconnect(remoteAddress, priority);
            return;
        }
    }

    if (entry && entry->stateMachine) {
        // Server role: accept the connection silently (no T_Connect echo on bus).
        // KNX 03.03.04 §5.1 server state machine goes E00→E03 with no outbound frame.
        // Firing ConnectRequest+ConnectResponse advances the local state to Connected.
        entry->priority = priority;
        (void)entry->stateMachine->handleEvent(ConnectionEvent::ConnectRequest);
        (void)entry->stateMachine->handleEvent(ConnectionEvent::ConnectResponse);
        _connectionTable.updateActivityTime(entry->index, nowMs());
    }
}

void TransportLayer::handleConnectResponse(const IndividualAddress& remoteAddress) {
    KNX_LOGD(TAG, "Received T_Connect response from 0x%04X", remoteAddress.raw);

    ConnectionEntry* entry = _connectionTable.findConnection(remoteAddress);
    if (entry && entry->stateMachine) {
        (void)entry->stateMachine->handleEvent(ConnectionEvent::ConnectResponse);
        _connectionTable.updateActivityTime(entry->index, nowMs());
        KNX_LOGI(TAG, "Connection established with 0x%04X", remoteAddress.raw);
    }
}

void TransportLayer::handleDisconnectRequest(const IndividualAddress& remoteAddress) {
    // Per KNX spec 03.03.04 Table 5: E02 (T_DISCONNECT from connection partner) -> A5:
    // just close connection and notify user. Do NOT send T_DISCONNECT back.
    KNX_LOGI(TAG, "T_Disconnect from 0x%04X, connection closed", remoteAddress.raw);
    (void)_connectionTable.removeConnectionByAddress(remoteAddress);
}

void TransportLayer::handleDisconnectResponse(const IndividualAddress& remoteAddress) {
    KNX_LOGD(TAG, "Received T_Disconnect response from 0x%04X", remoteAddress.raw);

    ConnectionEntry* entry = _connectionTable.findConnection(remoteAddress);
    if (entry && entry->stateMachine) {
        (void)entry->stateMachine->handleEvent(ConnectionEvent::DisconnectResponse);
        _connectionTable.updateActivityTime(entry->index, nowMs());
    }

    (void)_connectionTable.removeConnectionByAddress(remoteAddress);
}

void TransportLayer::handleNumberedData(const IndividualAddress& remoteAddress, uint8_t seqNum,
                                        std::span<const uint8_t> tpdu, Priority priority) {
    ConnectionEntry* entry = _connectionTable.findConnection(remoteAddress);
    if (!entry || !entry->stateMachine) {
        KNX_LOGW(TAG, "Received numbered data from unconnected device 0x%04X", remoteAddress.raw);
        return;
    }

    seqNum &= 0x0F;
    entry->priority = priority;

    auto dupRes = _connectionTable.isDuplicate(entry->index, seqNum);
    if (dupRes.isError()) {
        KNX_LOGW(TAG, "Duplicate check failed for connection %u", entry->index.value());
        return;
    }
    if (dupRes.value()) {
        KNX_LOGD(TAG, "Duplicate packet detected from 0x%04X, seq=%d - sending ACK",
                 remoteAddress.raw, seqNum);
        (void)sendAck(remoteAddress, seqNum, priority);
        _connectionTable.updateActivityTime(entry->index, nowMs());
        // ETS didn't receive our last response — retransmit immediately rather
        // than waiting for the 3 s periodic timer. Zero retransmitTimeoutMs so
        // processRetransmissions fires now; restore it after (the doubling step
        // inside would otherwise leave it at 0, causing retransmit on every tick).
        if (entry->pendingRetransmission && !entry->pendingData.empty()
                && entry->retransmitCount < entry->maxRetries) {
            entry->retransmitTimeoutMs = 0;
            processRetransmissions(nowMs());
            entry->retransmitTimeoutMs = constants::timing::ACK_TIMEOUT_MS;
        }
        return;
    }

    if (seqNum != (entry->expectedRxSeq & 0x0F)) {
        KNX_LOGW(TAG, "Sequence mismatch: expected %d, got %d",
                 entry->expectedRxSeq & 0x0F, seqNum);
        (void)sendNak(remoteAddress, entry->expectedRxSeq & 0x0F, priority);
        _connectionTable.updateActivityTime(entry->index, nowMs());
        return;
    }

    (void)sendAck(remoteAddress, seqNum, priority);
    _connectionTable.updateActivityTime(entry->index, nowMs());
    entry->expectedRxSeq = (entry->expectedRxSeq + 1) & 0x0F;

    TDataFrame tFrame;
    tFrame.service = TDataService::Connected;
    tFrame.source = remoteAddress;
    tFrame.destination = GroupAddress(_ownAddress.raw);
    tFrame.destinationType = AddressType::Individual;
    tFrame.sequenceNumber = seqNum;
    tFrame.tpdu.assign(tpdu);
    tFrame.securityTpci6 = tpdu.empty() ? 0u : static_cast<uint8_t>(tpdu[0] >> 2);

    // Connection-oriented frames carry the ETS download, which on a secure
    // device is where the secured management traffic lives — so they run
    // through the same transform as connectionless ones. The T_ACK above is
    // deliberately already sent: transport-layer acknowledgement is independent
    // of whether the application layer accepts the payload.
    if (_rxTransform) {
        auto rxRes = _rxTransform(tFrame);
        if (rxRes.isError()) {
            if (rxRes.error() != util::ErrorCode::FrameConsumed) {
                KNX_LOGW(TAG, "Transport RX transform rejected connected frame src=0x%04X tpdu=%s",
                         tFrame.source.raw,
                         util::formatHexBytes(tFrame.tpdu).c_str());
            }
            return;
        }
    }

    const auto* cb = _rxCallback.load(std::memory_order_acquire);
    if (cb) {
        (*cb)(tFrame);
    } else {
        enqueueReceivedFrame(tFrame);
    }
}

void TransportLayer::handleAck(const IndividualAddress& remoteAddress, uint8_t seqNum) {
    KNX_LOGD(TAG, "Received T_ACK from 0x%04X, seq=%d", remoteAddress.raw, seqNum);

    ConnectionEntry* entry = _connectionTable.findConnection(remoteAddress);
    if (entry) {
        if (entry->pendingRetransmission && ((seqNum & 0x0F) == (entry->pendingSeq & 0x0F))) {
            (void)_connectionTable.resetRetransmitState(entry->index);
            entry->sequenceNumber = (entry->sequenceNumber + 1u) & 0x0Fu;
        }
        _connectionTable.updateActivityTime(entry->index, nowMs());
    }
}

void TransportLayer::handleNak(const IndividualAddress& remoteAddress, uint8_t seqNum) {
    KNX_LOGW(TAG, "Received T_NAK from 0x%04X, seq=%d - will retransmit", remoteAddress.raw, seqNum);

    ConnectionEntry* entry = _connectionTable.findConnection(remoteAddress);
    if (entry) {
        if (entry->pendingRetransmission && ((seqNum & 0x0F) == (entry->pendingSeq & 0x0F))) {
            entry->pendingRetransmission = true;
            const uint32_t now = nowMs();
            entry->lastTxTimeMs = (now >= entry->retransmitTimeoutMs) ? (now - entry->retransmitTimeoutMs) : 0;
        }
        _connectionTable.updateActivityTime(entry->index, nowMs());
    }
}

util::Result<void> TransportLayer::sendConnect(const IndividualAddress& remoteAddress) {
    network::NDataFrame nFrame;
    nFrame.dlFrame.source = _ownAddress;
    nFrame.dlFrame.destination = GroupAddress(remoteAddress.raw);
    nFrame.dlFrame.destinationType = AddressType::Individual;
    auto buildResult = knx::protocol::buildTpduInPlace(protocol::TPCIField::create(protocol::TPCIControl::Connect),
                                                       application::APCIField(0),
                                                       {},
                                                       nFrame.dlFrame.tpdu);
    if (buildResult.isError()) {
        return buildResult.error();
    }
    nFrame.dlFrame.hopCount = 6;
    nFrame.dlFrame.repeated = false;  // first transmission
    // Client-initiated management connection: System priority (as certified
    // devices and ETS use for connection-oriented commissioning traffic).
    nFrame.dlFrame.priority = Priority::System;
    nFrame.dlFrame.ackRequested = false;

    return _network.sendFrame(nFrame);
}

util::Result<void> TransportLayer::sendDisconnect(const IndividualAddress& remoteAddress, Priority priority) {
    network::NDataFrame nFrame;
    nFrame.dlFrame.source = _ownAddress;
    nFrame.dlFrame.destination = GroupAddress(remoteAddress.raw);
    nFrame.dlFrame.destinationType = AddressType::Individual;
    auto buildResult = knx::protocol::buildTpduInPlace(protocol::TPCIField::create(protocol::TPCIControl::Disconnect),
                                                       application::APCIField(0),
                                                       {},
                                                       nFrame.dlFrame.tpdu);
    if (buildResult.isError()) {
        return buildResult.error();
    }
    nFrame.dlFrame.hopCount = 6;
    nFrame.dlFrame.repeated = false;  // first transmission
    nFrame.dlFrame.priority = priority;
    nFrame.dlFrame.ackRequested = false;

    return _network.sendFrame(nFrame);
}

util::Result<void> TransportLayer::sendAck(const IndividualAddress& remoteAddress, uint8_t seqNum, Priority priority) {
    network::NDataFrame nFrame;
    nFrame.dlFrame.source = _ownAddress;
    nFrame.dlFrame.destination = GroupAddress(remoteAddress.raw);
    nFrame.dlFrame.destinationType = AddressType::Individual;
    auto buildResult = knx::protocol::buildTpduInPlace(protocol::TPCIField::ack(seqNum),
                                                       application::APCIField(0),
                                                       {},
                                                       nFrame.dlFrame.tpdu);
    if (buildResult.isError()) {
        return buildResult.error();
    }
    nFrame.dlFrame.hopCount = 6;
    nFrame.dlFrame.repeated = false;  // first transmission
    nFrame.dlFrame.priority = priority;
    nFrame.dlFrame.ackRequested = false;

    return _network.sendFrame(nFrame);
}

util::Result<void> TransportLayer::sendNak(const IndividualAddress& remoteAddress, uint8_t seqNum, Priority priority) {
    network::NDataFrame nFrame;
    nFrame.dlFrame.source = _ownAddress;
    nFrame.dlFrame.destination = GroupAddress(remoteAddress.raw);
    nFrame.dlFrame.destinationType = AddressType::Individual;
    auto buildResult = knx::protocol::buildTpduInPlace(protocol::TPCIField::nak(seqNum),
                                                       application::APCIField(0),
                                                       {},
                                                       nFrame.dlFrame.tpdu);
    if (buildResult.isError()) {
        return buildResult.error();
    }
    nFrame.dlFrame.hopCount = 6;
    nFrame.dlFrame.repeated = false;  // first transmission
    nFrame.dlFrame.priority = priority;
    nFrame.dlFrame.ackRequested = false;

    return _network.sendFrame(nFrame);
}

} // namespace transport
} // namespace knx
