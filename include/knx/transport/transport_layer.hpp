// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file transport_layer.hpp
 * @brief KNX transport layer (layer 4)
 * 
 * Handles connectionless transport services (T_Data_Individual, T_Data_Group).
 */

#pragma once

#include "knx/types.hpp"
#include "knx/config.hpp"
#include "knx/network/network_layer.hpp"
#include "knx/transport/connection_table.hpp"
#include "knx/protocol/tpci.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/result.hpp"
#include <atomic>
#include <array>
#include <initializer_list>
#include <functional>
#include <span>
#include <vector>
#include <cstdint>

namespace knx {
namespace transport {

/**
 * @brief T_Data service types
 */
enum class TDataService {
    Individual,  // T_Data_Individual - point-to-point, connectionless
    Group,       // T_Data_Group - group communication
    Connected,   // T_Data_Connected - point-to-point on an open connection
};

/**
 * @brief T_Data frame structure
 */
struct TDataFrame {
    // On RX: which transport service the frame arrived on. On TX: which one to
    // send it with — Connected is the only value that makes the transport layer
    // rewrite the TPCI to T_Data_Connected and put the frame on the connection.
    // 03/03/04 §5: connectionless services are mapped independently of the
    // connection state machine, so an answer to a T_Data_Individual request
    // must go back as T_Data_Individual even while a connection is open.
    TDataService service;          // Service type
    IndividualAddress source;      // Source address
    GroupAddress destination;      // Destination (interpreted by destinationType)
    AddressType destinationType;   // Destination address type

    // L_Data metadata (required for secure context derivation)
    bool standardFrame;            // True for standard, false for extended
    bool repeated;                 // Repeat flag
    Priority priority;             // Message priority
    bool ackRequested;             // Acknowledge requested
    bool confirmation;             // Confirmation (0=error, 1=success)
    uint8_t hopCount;              // Hop count
    
    uint8_t sequenceNumber;        // Sequence number (for connected mode)

    // The six TPCI bits the frame carries on the wire, as needed by KNX Data
    // Secure: the CCM block B0 binds TPCI/APCISec (03/03/07 Figure 101), so a
    // frame that goes out on a transport connection must be secured against the
    // TPCI it will actually carry. The transport layer resolves the connection's
    // sequence number into this field *before* the TX transform runs; on RX it
    // is filled from the received TPDU.
    uint8_t securityTpci6;

    // On RX: how the S-AL found the frame protected, filled in by the RX
    // transform when it unwraps an S-A_Data PDU. A frame that arrived in plain
    // keeps the all-false default, and so does every frame in a build without
    // KNX Secure. Unused on TX.
    RequestSecurity security{};

    // TPDU bounded to 2-byte header + config::MAX_APDU_LENGTH bytes payload
    util::FixedVector<uint8_t, 2 + config::MAX_APDU_LENGTH> tpdu;
    
    TDataFrame() 
        : service(TDataService::Group)
        , source(0)
        , destination()
        , destinationType(AddressType::Group)
        , standardFrame(true)
        , repeated(false)
        , priority(Priority::Low)
        , ackRequested(true)
        , confirmation(false)
        , hopCount(6)
        , sequenceNumber(0)
        , securityTpci6(0)
    {}

    protocol::TPCIField tpci() const {
        if (tpdu.size() < 2) return protocol::TPCIField(0);
        return knx::protocol::unpackTpduHeader(tpdu[0], tpdu[1]).tpci;
    }

    application::APCIField apci() const {
        if (tpdu.size() < 2) return application::APCIField(0);
        return knx::protocol::unpackTpduHeader(tpdu[0], tpdu[1]).apci;
    }

    std::span<const uint8_t> payload() const {
        if (tpdu.size() < 2) return {};
        return std::span<const uint8_t>(tpdu).subspan(2);
    }
};

/**
 * @brief Transport layer callback
 */
using TDataCallback = util::InplaceFunction<void(const TDataFrame&), 64>;

/**
 * @brief Optional in-place frame transform hook
 *
 * Used to inject processing between application/transport and the medium
 * (e.g., KNX Secure wrapping/unwrapping) without coupling TransportLayer to
 * higher-level components.
 *
 * Return an error Result to drop/abort the frame.
 */
using TDataTransform = util::InplaceFunction<util::Result<void>(TDataFrame&), 64>;

/**
 * @brief Transport layer (connectionless)
 * 
 * Handles connectionless and connection-oriented transport services.
 * 
 * @thread_safety Most methods are NOT thread-safe. sendFrame() and connection 
 * management methods should be called from the same thread that calls the network 
 * layer loop. Callbacks are invoked from the network layer context.
 */
class TransportLayer {
public:
    using ProgressState = util::OperationProgressState;
    using SendProgressState = util::OperationProgressState;
    using ControlSendProgressState = util::OperationProgressState;
    using ConnectedSendProgressState = util::OperationProgressState;

    explicit TransportLayer(network::NetworkLayer& network);
    virtual ~TransportLayer();
    
    /**
     * @brief Initialize transport layer
     */
    util::Result<void> init(const IndividualAddress& ownAddress);
    
    /**
     * @brief Update transport layer own address at runtime
     */
    util::Result<void> setOwnAddress(const IndividualAddress& ownAddress);

    /**
     * @brief Close transport layer
     */
    void close();
    
    /**
     * @brief Send T_Data.req
     * @thread_safety NOT thread-safe - should be called from single thread
     */
    util::Result<void> sendFrame(const TDataFrame& frame);
    util::Result<void> beginTransmit(const TDataFrame& frame);
    util::Result<ProgressState> pollTransmit();

    /**
     * @brief Progress background network-layer maintenance work such as deferred forwarding
     * @thread_safety NOT thread-safe - call from the same maintenance context as other poll/process methods
     */
    void processBackgroundWork();
    
    /**
     * @brief Set receive callback
     * @thread_safety NOT thread-safe - should be called during initialization
     * @warning Callback is invoked from network layer context - avoid blocking operations
     */
    void setReceiveCallback(TDataCallback callback);

    /**
     * @brief Set optional TX transform (applied before sending)
     * @thread_safety NOT thread-safe - should be called during initialization
     * @note Transform is called while holding internal locks - avoid blocking operations
     */
    void setTxTransform(TDataTransform transform);

    /**
     * @brief Set optional RX transform (applied before delivering to upper layer)
     * @thread_safety NOT thread-safe - should be called during initialization
     * @note Transform is called while holding internal locks - avoid blocking operations
     */
    void setRxTransform(TDataTransform transform);
    
    // Connection-oriented services
    
    /**
     * @brief Connect to remote device (T_Connect.req)
     * @param remoteAddress Individual address of remote device
    * @return Connection index (0-15) on success, invalid on failure
     */
    util::Result<ConnectionIndex> connect(const IndividualAddress& remoteAddress);
    util::Result<ConnectionIndex> beginConnect(const IndividualAddress& remoteAddress);
    util::Result<ControlSendProgressState> pollConnect();
    
    /**
     * @brief Disconnect from remote device (T_Disconnect.req)
     * @param connectionIndex Connection index from connect()
     * @return true on success
     */
    util::Result<void> disconnect(ConnectionIndex connectionIndex);
    
    /**
     * @brief Send data on established connection
     * @param connectionIndex Connection index
     * @param data Data to send
     * @return true on success
     */
    /**
     * @brief The six TPCI bits the next T_Data_Connected frame to @p peer will
     *        carry, or 0 when no connection to @p peer is open.
     *
     * KNX Data Secure binds the TPCI into the CCM block B0, so an APDU that is
     * secured before the transport assigns the sequence number has to ask for
     * it first (03/03/07 §5.1.3.2, NOTE 15).
     */
    uint8_t connectedTxTpci6(const IndividualAddress& peer) const;

    util::Result<void> sendConnectedData(ConnectionIndex connectionIndex, std::span<const uint8_t> data);
    util::Result<void> sendConnectedData(ConnectionIndex connectionIndex, std::initializer_list<uint8_t> data) {
        return sendConnectedData(connectionIndex, std::span<const uint8_t>(data));
    }
    util::Result<void> beginSendConnectedData(ConnectionIndex connectionIndex, std::span<const uint8_t> data);
    util::Result<void> beginSendConnectedData(ConnectionIndex connectionIndex, std::initializer_list<uint8_t> data) {
        return beginSendConnectedData(connectionIndex, std::span<const uint8_t>(data));
    }
    util::Result<ConnectedSendProgressState> pollConnectedDataSend();
    
    /**
     * @brief Check if connection is established
     * @param connectionIndex Connection index
     * @return true if connected
     */
    bool isConnected(ConnectionIndex connectionIndex) const;
    
    /**
     * @brief Process retransmissions and timeouts
     * 
     * Should be called periodically (e.g., every 100ms) to handle
     * automatic retransmission of acknowledged packets and connection
     * timeout detection.
     * 
     * @param currentTimeMs Current system time in milliseconds
     * @note This is typically called from a timer task
     */
    void processRetransmissions(uint32_t currentTimeMs);
    util::Result<void> beginProcessRetransmissions(uint32_t currentTimeMs);
    util::Result<ControlSendProgressState> pollProcessRetransmissions();

    util::Result<void> beginHandleNetworkRx(const network::NDataFrame& frame);
    util::Result<ControlSendProgressState> pollHandleNetworkRx();
    util::Result<void> beginReceive(const network::NDataFrame& frame) { return beginHandleNetworkRx(frame); }
    util::Result<ProgressState> pollReceive() { return pollHandleNetworkRx(); }

    bool popReceivedFrame(TDataFrame& frame);
    size_t queuedReceiveCount() const { return _rxQueueCount; }
    size_t droppedReceiveFrameCount() const { return _droppedReceiveFrames; }
    
    // For testing - make handleNetworkRx accessible
    void handleNetworkRx(const network::NDataFrame& frame);
    
    // For testing - TPCI extraction utilities
    static protocol::TPCI extractTPCI(protocol::TPCIField tpci) { return tpci.type(); }
    static uint8_t extractSeqNum(protocol::TPCIField tpci) { return tpci.seqNum(); }

private:
    network::NetworkLayer& _network;
    IndividualAddress _ownAddress;
    std::atomic<const TDataCallback*> _rxCallback;
    TDataCallback _rxCallbackStorage;
    TDataTransform _txTransform;
    TDataTransform _rxTransform;
    bool _initialized;

    // Timebase for connected-mode timers (ms). Updated by processRetransmissions().
    uint32_t _timebaseMs;

    struct SendOperationState {
        bool active{false};
    };
    SendOperationState _sendOperation{};

    struct ConnectedSendOperationState {
        bool active{false};
        ConnectionIndex connectionIndex{ConnectionIndex::invalid()};
        uint8_t sequenceNumber{0};
        util::FixedVector<uint8_t, config::MAX_APDU_LENGTH> payload;
    };
    ConnectedSendOperationState _connectedSendOperation{};

    struct ConnectOperationState {
        bool active{false};
        bool createdConnection{false};
        ConnectionIndex connectionIndex{ConnectionIndex::invalid()};
        IndividualAddress remoteAddress{0};
    };
    ConnectOperationState _connectOperation{};

    struct RetransmissionOperationState {
        bool active{false};
        uint32_t currentTimeMs{0};
        std::array<ConnectionIndex, ConnectionTable::MAX_CONNECTIONS> pending{};
        size_t pendingCount{0};
        size_t nextIndex{0};
        bool sendingCurrent{false};
        ConnectionIndex currentConnection{ConnectionIndex::invalid()};
    };
    RetransmissionOperationState _retransmissionOperation{};

    struct ReceiveOperationState {
        bool active{false};
        bool waitingResponse{false};
        bool deliverFrame{false};
        bool advanceExpectedSequence{false};
        ControlSendProgressState terminalState{ControlSendProgressState::Success};
        ConnectionIndex connectionIndex{ConnectionIndex::invalid()};
        TDataFrame receivedFrame{};
    };
    ReceiveOperationState _receiveOperation{};

    static constexpr size_t RX_QUEUE_CAPACITY = 8u;
    std::array<TDataFrame, RX_QUEUE_CAPACITY> _rxQueue{};
    size_t _rxQueueHead{0};
    size_t _rxQueueCount{0};
    size_t _droppedReceiveFrames{0};
    
    // Connection-oriented support
    ConnectionTable _connectionTable;

    uint32_t nowMs() const { return _timebaseMs; }

    util::Result<void> buildNetworkTxFrame(const TDataFrame& frame, network::NDataFrame& nFrame);
    
    // Helper to convert N_Data to T_Data
    TDataFrame convertFromNetwork(const network::NDataFrame& nFrame);
    
    // Connection-oriented frame handlers. Handlers taking a Priority mirror the
    // received frame's priority into the connection so responses use the same one.
    void handleConnectRequest(const IndividualAddress& remoteAddress, Priority priority,
                              bool repeatedFrame = false);
    void handleConnectResponse(const IndividualAddress& remoteAddress);
    void handleDisconnectRequest(const IndividualAddress& remoteAddress);
    void handleDisconnectResponse(const IndividualAddress& remoteAddress);
    void handleNumberedData(const IndividualAddress& remoteAddress, uint8_t seqNum,
                            std::span<const uint8_t> tpdu, Priority priority);
    void handleAck(const IndividualAddress& remoteAddress, uint8_t seqNum);
    void handleNak(const IndividualAddress& remoteAddress, uint8_t seqNum);

    // Send control frames
    util::Result<void> sendConnect(const IndividualAddress& remoteAddress);
    util::Result<void> sendDisconnect(const IndividualAddress& remoteAddress, Priority priority);
    util::Result<void> sendAck(const IndividualAddress& remoteAddress, uint8_t seqNum, Priority priority);
    util::Result<void> sendNak(const IndividualAddress& remoteAddress, uint8_t seqNum, Priority priority);
    void finishConnectedSendOperation();
    void finishConnectOperation();
    void finishRetransmissionOperation();
    void finishSendOperation();
    void finishReceiveOperation();
    void finalizeReceiveOperation();
    ControlSendProgressState mapTxErrorToProgressState(util::ErrorCode error) const;
    util::Result<void> beginControlResponse(const IndividualAddress& remoteAddress,
                                            const protocol::TPCIField& tpci,
                                            Priority priority);
    void beginHandleNumberedData(const IndividualAddress& remoteAddress,
                                 uint8_t seqNum,
                                 std::span<const uint8_t> tpdu,
                                 Priority priority);
    void enqueueReceivedFrame(const TDataFrame& frame);
};

} // namespace transport
} // namespace knx
