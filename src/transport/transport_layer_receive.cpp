// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/transport/transport_layer.hpp"
#include "knx/util/log.hpp"
#include "knx/util/hex.hpp"
#include "knx/protocol/tpdu_codec.hpp"

static const char* TAG = "KNX.Transport";

namespace knx {
namespace transport {

namespace {

/// E05 of 03_03_04 §5.2: "SeqNo_of_PDU == ((SeqNoRcv - 1) & Fh)", i.e. a
/// repeat of the last frame we accepted. It is only possible once a frame has
/// been accepted on the connection. This used to compare against the last
/// sequence number *seen*, which a frame NAK'd for being out of order also
/// updated, so the partner's repetition of that frame was answered with a
/// T_ACK and never delivered.
bool isRepeatOfLastAccepted(const ConnectionEntry& entry, uint8_t seqNum) {
    return entry.lastReceivedSeq != 0xFFu && seqNum == ((entry.expectedRxSeq - 1u) & 0x0Fu);
}

} // namespace

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
            handleAck(remoteAddr, seqNum, frame.dlFrame.repeated);
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
        KNX_LOGW(TAG, "Numbered data from unconnected device 0x%04X - dropped",
                 remoteAddress.raw);
        // Dropped, not refused. 03/03/04 answers a T_Data_Connected in the
        // CLOSED state with T_Disconnect, and on TP1 that is right: a lost
        // T_Connect is nearly impossible there, so a peer talking on a
        // connection we do not have really is confused and should be told.
        //
        // KNXnet/IP routing is multicast, which 802.11 neither acknowledges nor
        // retries, and ETS opens a connection by sending T_Connect and the first
        // request two milliseconds apart. Losing the first of that pair is
        // routine, and it is indistinguishable here from a confused peer. ETS
        // recovers from it on its own — it times out after 3 s and reconnects,
        // and it was observed doing exactly that three times in a row while a
        // device rebooted. Answering T_Disconnect instead ends that retry loop:
        // ETS reports "connection refused" and abandons the download on the
        // attempt that would have been the one to succeed.
        //
        // So the frame is dropped and the peer's own timeout does the work. The
        // log line is the diagnosis; the silence is deliberate.
        return;
    }

    seqNum &= 0x0F;
    entry->priority = priority;

    if (isRepeatOfLastAccepted(*entry, seqNum)) {
        // E05 → A3: the partner missed our T_ACK and repeated the frame.
        KNX_LOGD(TAG, "Duplicate packet detected from 0x%04X, seq=%d - sending ACK",
                 remoteAddress.raw, seqNum);
        (void)sendAck(remoteAddress, seqNum, priority);
        _connectionTable.updateActivityTime(entry->index, nowMs());
        // ETS didn't receive our last response — retransmit immediately rather
        // than waiting for the 3 s periodic timer. Zero retransmitTimeoutMs so
        // processRetransmissions fires now, then restore it. The entry is
        // looked up again afterwards because the timer pass may close
        // connections.
        if (entry->pendingRetransmission && !entry->pendingData.empty()
                && entry->retransmitCount < entry->maxRetries) {
            const ConnectionIndex index = entry->index;
            entry->retransmitTimeoutMs = 0;
            processRetransmissions(nowMs());
            if (ConnectionEntry* still = _connectionTable.getConnection(index)) {
                still->retransmitTimeoutMs = constants::timing::ACK_TIMEOUT_MS;
            }
        }
        return;
    }

    if (seqNum != (entry->expectedRxSeq & 0x0F)) {
        // E06 → A4: T_NAK carrying the sequence number of the received frame,
        // not the one we expected. The partner matches a T_NAK against its own
        // SeqNoSend (E11/E12), so a T_NAK naming our expectation was read as
        // a T_NAK for some other frame and ignored.
        KNX_LOGW(TAG, "Sequence mismatch: expected %d, got %d",
                 entry->expectedRxSeq & 0x0F, seqNum);
        (void)sendNak(remoteAddress, seqNum, priority);
        _connectionTable.updateActivityTime(entry->index, nowMs());
        return;
    }

    // E04 → A2.
    (void)sendAck(remoteAddress, seqNum, priority);
    _connectionTable.updateActivityTime(entry->index, nowMs());
    entry->lastReceivedSeq = seqNum;
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

void TransportLayer::handleAck(const IndividualAddress& remoteAddress, uint8_t seqNum, bool repeatedFrame) {
    KNX_LOGD(TAG, "Received T_ACK from 0x%04X, seq=%d", remoteAddress.raw, seqNum);

    ConnectionEntry* entry = _connectionTable.findConnection(remoteAddress);
    if (!entry) {
        return;
    }

    // A T_ACK only closes the acknowledgement window (OPEN_WAIT ->
    // OPEN_IDLE); it does not carry SeqNoSend. Advancing the counter here
    // instead of on transmission made a single lost T_ACK freeze it for
    // good: every later response repeated a sequence number the peer had
    // already consumed, the peer discarded them as duplicates, and an ETS
    // download died on the first telegram after the loss.
    if (entry->pendingRetransmission) {
        if ((seqNum & 0x0F) == (entry->pendingSeq & 0x0F)) {
            // E08 → A8.
            (void)_connectionTable.resetRetransmitState(entry->index);
            _connectionTable.updateActivityTime(entry->index, nowMs());
            return;
        }
        if (!repeatedFrame) {
            // E09 in OPEN_WAIT → A6 (Style 1 and Style 1 Rationalised): the
            // partner acknowledged a frame we are not waiting for, so the two
            // sides disagree on SeqNoSend and nothing sent from here on can be
            // matched up. Close now rather than repeat the frame three times
            // into a connection that cannot recover.
            //
            // A data-link repetition (repeat flag set) is exempt: it is the
            // partner's T_ACK for our previous frame sent again because our
            // L2 acknowledgement was lost, the same case handleConnectRequest
            // exempts for T_Connect.
            KNX_LOGW(TAG, "T_ACK seq=%u from 0x%04X while waiting for seq=%u - disconnecting",
                     seqNum & 0x0Fu, remoteAddress.raw, entry->pendingSeq & 0x0Fu);
            closeConnectionWithDisconnect(*entry);
            return;
        }
    }
    // A T_ACK in OPEN_IDLE is E08/E09 → A6 in every style, but it is ignored
    // here: a late or repeated T_ACK for a frame already acknowledged is
    // harmless, and dropping a working ETS connection over it is not.
    _connectionTable.updateActivityTime(entry->index, nowMs());
}

void TransportLayer::handleNak(const IndividualAddress& remoteAddress, uint8_t seqNum) {
    KNX_LOGW(TAG, "Received T_NAK from 0x%04X, seq=%d - will retransmit", remoteAddress.raw, seqNum);

    ConnectionEntry* entry = _connectionTable.findConnection(remoteAddress);
    if (entry) {
        // Only a T_NAK naming the outstanding frame triggers a repetition
        // (E12 → A9; E13, repetitions used up, ends in A6 through the
        // acknowledge timer). A T_NAK with any other sequence number (E11) is
        // ignored, which is Style 2's A0. Style 1 would disconnect (A6), but a
        // stale T_NAK, e.g. one the data link repeated, says nothing about
        // the frame now outstanding, whose own acknowledge timeout still
        // guards it.
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
