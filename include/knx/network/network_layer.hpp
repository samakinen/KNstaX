// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file network_layer.hpp
 * @brief KNX network layer (layer 3)
 * 
 * Handles routing, hop count management, and address filtering.
 */

#pragma once

#include "knx/types.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/network/routing_table.hpp"
#include "knx/network/filter_table.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include <atomic>
#include <array>
#include <functional>
#include <vector>

namespace knx {
namespace network {

/**
 * @brief N_Data service primitives
 */
enum class NDataService {
    Request,      // N_Data.req - send frame
    Indicate,     // N_Data.ind - receive frame
};

/**
 * @brief Network layer operating mode
 */
enum class NetworkMode {
    Device,       // Normal device mode (no routing)
    LineCoupler,  // Line coupler mode (route between lines)
    AreaCoupler,  // Area coupler mode (route between areas)
};

/**
 * @brief N_Data frame structure
 */
struct NDataFrame {
    datalink::LDataFrame dlFrame;  // Data link frame
    uint8_t hopCount;               // Hop count (decremented at each hop)
    
    NDataFrame() : hopCount(6) {}
};

/**
 * @brief Network layer callback
 */
using NDataCallback = util::InplaceFunction<void(const NDataFrame&), 64>;

/**
 * @brief Network layer
 * 
 * Handles routing, hop count management, and address filtering.
 * 
 * @thread_safety Most methods are NOT thread-safe. sendFrame() should be called from 
 * the same thread that processes the data link layer. Configuration methods (setNetworkMode, 
 * setRoutingEnabled, etc.) should be called during initialization or with external 
 * synchronization. Read-only methods (isOnSameArea, getNetworkMode) are safe to call 
 * concurrently.
 */
class NetworkLayer {
public:
    using ProgressState = util::OperationProgressState;
    using TxProgressState = util::OperationProgressState;

    enum class ForwardProgressState : uint8_t {
        Idle = 0,
        Pending,
        Success,
        Busy,
        TransmissionFailed,
        Timeout,
    };

    explicit NetworkLayer(datalink::Tp1DataLinkLayer& datalink);
    virtual ~NetworkLayer();
    
    /**
     * @brief Initialize network layer
     */
    util::Result<void> init(const IndividualAddress& ownAddress);

    /**
     * @brief Update network layer own address at runtime
     */
    void setOwnAddress(const IndividualAddress& ownAddress);
    
    /**
     * @brief Close network layer
     */
    void close();
    
    /**
     * @brief Send N_Data.req
     * @thread_safety NOT thread-safe - should be called from single thread
     */
    util::Result<void> sendFrame(const NDataFrame& frame);
    util::Result<void> beginTransmit(const NDataFrame& frame);
    util::Result<ProgressState> pollTransmit();

    /**
     * @brief Advance deferred forwarding outside receive-callback context
     * @thread_safety NOT thread-safe - call from the same stack maintenance context as send/poll methods
     */
    util::Result<ForwardProgressState> pollForwarding();
    bool hasPendingForwarding() const { return _forwardOperation.active || _forwardQueueCount > 0u; }
    size_t queuedForwardFrameCount() const { return _forwardQueueCount; }
    size_t droppedForwardFrameCount() const { return _droppedForwardFrames; }
    
    /**
     * @brief Set receive callback
     * @thread_safety NOT thread-safe - should be called during initialization
     * @warning Callback is invoked from data link layer context - avoid blocking operations
     */
    void setReceiveCallback(NDataCallback callback);

    /**
     * @brief Deliver a frame to this network layer as if it had arrived on its
     *        own data link layer.
     *
     * Exists for couplers. A coupler owns both data link layers and takes over
     * their receive callbacks to make routing decisions, which displaces the
     * callback this class installs in init(). The coupler hands back the frames
     * this device should process — its own management traffic and any group
     * telegram — through here, so a coupler can still be a device.
     *
     * Address filtering is unchanged: this runs the same path a directly
     * received frame would, so the coupler does not have to know which group
     * addresses the device subscribes to.
     */
    void deliverLocalFrame(const datalink::LDataFrame& frame) { handleDlRx(frame); }


    /**
     * @brief Check if this device is on the same area
     */
    bool isOnSameArea(const IndividualAddress& addr) const;
    
    /**
     * @brief Check if this device is on the same line
     */
    bool isOnSameLine(const IndividualAddress& addr) const;
    
    /**
     * @brief Set network operating mode
     */
    void setNetworkMode(NetworkMode mode);
    
    /**
     * @brief Get network operating mode
     */
    NetworkMode getNetworkMode() const { return _mode; }
    
    /**
     * @brief Get routing table (for configuration)
     */
    RoutingTable& getRoutingTable() { return _routingTable; }
    
    /**
     * @brief Get filter table (for configuration)
     */
    FilterTable& getFilterTable() { return _filterTable; }
    
    /**
     * @brief Enable/disable routing
     */
    void setRoutingEnabled(Toggle enabled) { _routingEnabled = isEnabled(enabled); }
    
    /**
     * @brief Check if routing is enabled
     */
    bool isRoutingEnabled() const { return _routingEnabled; }
    
    /**
     * @brief Enable/disable filtering
     */
    void setFilteringEnabled(Toggle enabled) { _filteringEnabled = isEnabled(enabled); }
    
    /**
     * @brief Check if filtering is enabled
     */
    bool isFilteringEnabled() const { return _filteringEnabled; }
    
private:
    datalink::Tp1DataLinkLayer& _datalink;
    IndividualAddress _ownAddress;
    std::atomic<const NDataCallback*> _rxCallback;
    NDataCallback _rxCallbackStorage;
    bool _initialized;
    
    // Routing and filtering
    NetworkMode _mode;
    RoutingTable _routingTable;
    FilterTable _filterTable;
    bool _routingEnabled;
    bool _filteringEnabled;

    struct TxOperationState {
        bool active{false};
        bool started{false};
        datalink::LDataFrame dlFrame;
    };
    TxOperationState _txOperation{};

    struct ForwardOperationState {
        bool active{false};
        bool started{false};
        datalink::LDataFrame dlFrame;
    };

    static constexpr size_t FORWARD_QUEUE_CAPACITY = 8u;
    std::array<datalink::LDataFrame, FORWARD_QUEUE_CAPACITY> _forwardQueue{};
    size_t _forwardQueueHead{0};
    size_t _forwardQueueCount{0};
    size_t _droppedForwardFrames{0};
    ForwardOperationState _forwardOperation{};

    void finishTxOperation();
    void finishForwardOperation();
    util::Result<void> enqueueForwardFrame(const NDataFrame& frame);
    
    // Data link callback handler
    void handleDlRx(const datalink::LDataFrame& frame);
    
    // Routing decision logic
    bool shouldForwardFrame(const datalink::LDataFrame& frame, uint8_t hopCount);
    bool shouldFilterFrame(const datalink::LDataFrame& frame);
};

} // namespace network
} // namespace knx
