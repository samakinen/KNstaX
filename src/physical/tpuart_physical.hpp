// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tpuart_physical.hpp
 * @brief TP1 physical layer implementation using TPUART
 * 
 * Implements the KNX TP1 physical layer using TPUART chips like NCN5120/NCN5121.
 * Handles low-level frame transmission and reception with collision detection.
 */

#pragma once

#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/platform/platform.hpp"
#include "knx/platform/raii_resources.hpp"
#include "knx/platform/uart_interface.hpp"

#include <array>
#include <span>

namespace knx {
namespace physical {

/**
 * @brief TPUART service codes
 */
enum class TpuartService : uint8_t {
    // Control services
    Reset           = 0x01,
    State           = 0x02,
    ProductId       = 0x03,
    
    // L_Data services
    DataStart       = 0x80,  // First byte with control field
    DataContinue    = 0x80,  // Subsequent bytes
    DataEnd         = 0x40,  // Last byte
    
    // Indication services
    LDataInd        = 0x90,  // Standard frame
    LDataCon        = 0x0B,  // TX confirmation
    LPollDataInd    = 0xF0,  // Poll data
    
    // ACK services
    AckInfo         = 0x00,  // Info acknowledge
    Ack             = 0x11,  // Positive acknowledge
    Nack            = 0x00,  // Negative acknowledge (bit 4 not set)
    Busy            = 0x01,  // Addressed busy
    
    // U_Configure services
    Configure       = 0x10,  // Configure TPUART
};

/**
 * @brief TPUART physical layer implementation
 */
class TpuartPhysical : public Tp1PhysicalFrameSource {
public:
    using ProgressState = util::OperationProgressState;

    static constexpr size_t MAX_TP1_FRAME_SIZE = 23;
    static constexpr size_t MIN_TP1_FRAME_SIZE = 6;
    static constexpr size_t RX_QUEUE_ITEM_SIZE = MAX_TP1_FRAME_SIZE + 1;

    explicit TpuartPhysical(platform::TimingPlatform& timing,
                            platform::QueuePlatform& queuePlatform,
                            platform::UartInterface& uart);
    virtual ~TpuartPhysical();
    
    util::Result<void> init();
    void close();
    bool isOpen() const;
    
    util::Result<size_t> sendFrame(std::span<const uint8_t> frame);
    util::Result<uint32_t> beginTransmit(std::span<const uint8_t> frame);
    util::Result<ProgressState> pollTransmit(uint32_t sequence);
    util::Result<std::vector<uint8_t>> receiveFrame(uint32_t timeoutMs);
    util::Result<void> beginReceive(uint32_t timeoutMs);
    util::Result<ProgressState> pollReceive();
    util::Result<std::span<const uint8_t>> receivedFrameView();
    util::Result<std::span<const uint8_t>> receiveFrameView(uint32_t timeoutMs) override;
    
    void setReceiveCallback(ReceiveCallback callback, void* context);
    
    PhysicalLayerState getState() const;
    util::Result<void> setBusMonitorMode(Toggle mode);
    
private:
    platform::TimingPlatform& _timing;
    platform::QueuePlatform& _queuePlatform;
    platform::UartInterface& _uart;
    PhysicalLayerState _state;
    bool _initialized;
    bool _busMonitorMode;
    
    // RX state machine
    enum class RxState {
        Idle,
        ReceivingControl,
        ReceivingData,
        Complete
    };
    
    RxState _rxState;
    std::array<uint8_t, MAX_TP1_FRAME_SIZE> _rxBuffer;
    std::array<uint8_t, RX_QUEUE_ITEM_SIZE> _rxDequeuedFrame;
    size_t _rxIndex;
    size_t _rxExpectedLength;
    
    // TX state
    bool _txPending;
    uint8_t _lastTxStatus;
    bool _progressTxActive{false};
    uint32_t _progressTxSequence{0};
    ProgressState _progressTxState{ProgressState::Success};
    std::array<uint8_t, MAX_TP1_FRAME_SIZE> _progressTxFrame{};
    size_t _progressTxLength{0};
    bool _progressRxActive{false};
    uint32_t _progressRxDeadlineMs{0};
    size_t _progressRxLength{0};
    
    // Callback
    ReceiveCallback _rxCallback;
    void* _rxCallbackContext;
    
    // RX queue (platform abstraction)
    std::unique_ptr<platform::Queue> _rxQueue;
    
    // UART RX callback (static wrapper)
    static void uartRxCallback(void* context);
    
    // Internal processing
    void processRxByte(uint8_t byte);
    void handleServiceByte(uint8_t service);
    void handleDataByte(uint8_t byte);
    void handleConfirmation(uint8_t status);
    
    // TPUART commands
    util::Result<void> sendReset();
    util::Result<void> sendState();
    util::Result<void> getProductId(uint8_t& manufacturer, uint8_t& device);
    util::Result<void> configure(bool addressedMode = true, bool autoAck = true);
    util::Result<size_t> transmitFrameBlocking(std::span<const uint8_t> frame);
    
    // Frame encoding/decoding
    bool isStandardFrame() const;
    bool isExtendedFrame() const;
    uint8_t calculateChecksum(std::span<const uint8_t> data) const;
};

} // namespace physical
} // namespace knx
