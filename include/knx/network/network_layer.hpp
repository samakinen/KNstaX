// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file network_layer.hpp
 * @brief KNX network layer (layer 3)
 * 
 * Handles hop count management and local delivery. Group filtering is a
 * coupler function (TwoPortCoupler and its Filter Table), not an end device's.
 */

#pragma once

#include "knx/types.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include <atomic>
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
 * Handles hop count management and local delivery. Group filtering is a
 * coupler function (TwoPortCoupler and its Filter Table), not an end device's.
 * 
 * @thread_safety Most methods are NOT thread-safe. sendFrame() should be called from 
 * the same thread that processes the data link layer. Configuration methods
 * should be called during initialization or with external 
 * synchronization. Read-only methods (isOnSameArea) are safe to call 
 * concurrently.
 */
class NetworkLayer {
public:
    using ProgressState = util::OperationProgressState;
    using TxProgressState = util::OperationProgressState;

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
    
private:
    datalink::Tp1DataLinkLayer& _datalink;
    IndividualAddress _ownAddress;
    std::atomic<const NDataCallback*> _rxCallback;
    NDataCallback _rxCallbackStorage;
    bool _initialized;
    
    struct TxOperationState {
        bool active{false};
        bool started{false};
        datalink::LDataFrame dlFrame;
    };
    TxOperationState _txOperation{};

    void finishTxOperation();
    
    // Data link callback handler
    void handleDlRx(const datalink::LDataFrame& frame);
    
};

} // namespace network
} // namespace knx
