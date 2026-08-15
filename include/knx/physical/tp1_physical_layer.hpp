// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_physical_layer.hpp
 * @brief TP1 physical layer interface
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"
#include "knx/types.hpp"

namespace knx {
namespace physical {

/**
 * @brief Physical layer state
 */
enum class PhysicalLayerState {
    Idle,           // No transmission in progress
    Transmitting,   // Sending frame
    Receiving,      // Receiving frame
    Error,          // Error state
};

using TxOutcomeState = util::OperationProgressState;

/**
 * @brief RX callback signature
 */
using ReceiveCallback = util::InplaceFunction<void(void*), 32>;

/**
 * @brief TP1 physical layer interface
 */
class Tp1PhysicalLayer {
public:
    using ProgressState = util::OperationProgressState;

    virtual ~Tp1PhysicalLayer() = default;
    
    /**
     * @brief Initialize the physical layer
     */
    virtual util::Result<void> init() = 0;
    
    /**
     * @brief Close the physical layer
     */
    virtual void close() = 0;
    
    /**
     * @brief Check if open
     */
    virtual bool isOpen() const = 0;
    
    /**
     * @brief Send frame
     * @param frame Frame data
     * @return Number of bytes transmitted or error code
     */
    virtual util::Result<size_t> sendFrame(std::span<const uint8_t> frame) = 0;

    /**
     * @brief Begin a polled transmission when supported.
     * @param frame Frame data
     * @return Sequence token for polling completion
     */
    virtual util::Result<uint32_t> beginTransmit(std::span<const uint8_t> frame) = 0;

    /**
     * @brief Poll the outcome of a previously started transmission.
     * @param sequence Sequence token returned by beginTransmit
     * @return Transmission state
     */
    virtual util::Result<ProgressState> pollTransmit(uint32_t sequence) = 0;
    
    /**
     * @brief Receive a raw KNX frame from the physical medium.
     * @param timeoutMs Timeout in milliseconds
     * @return Result containing the received frame bytes or an error
     */
    virtual util::Result<std::vector<uint8_t>> receiveFrame(uint32_t timeoutMs) = 0;

    virtual util::Result<void> beginReceive(uint32_t timeoutMs) = 0;

    virtual util::Result<ProgressState> pollReceive() = 0;

    virtual util::Result<std::span<const uint8_t>> receivedFrameView() = 0;
    
    /**
     * @brief Set RX callback
     */
    virtual void setReceiveCallback(ReceiveCallback callback, void* context) = 0;
    
    /**
     * @brief Get current state
     */
    virtual PhysicalLayerState getState() const = 0;
    
    /**
     * @brief Enable/disable bus monitor mode
     * @param enable true to enable bus monitor mode (RX-only), false for normal mode
     * @return Result indicating success or failure
     */
    virtual util::Result<void> setBusMonitorMode(Toggle mode) = 0;
};

class Tp1PhysicalFrameSource {
public:
    virtual ~Tp1PhysicalFrameSource() = default;

    /**
     * @brief Receive a raw KNX frame as a non-owning view.
     * @param timeoutMs Timeout in milliseconds
     * @return Non-owning frame view valid until the next receive call on the same object
     */
    virtual util::Result<std::span<const uint8_t>> receiveFrameView(uint32_t timeoutMs) = 0;
};

} // namespace physical
} // namespace knx
