// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tpuart_physical.cpp
 * @brief TP1 physical layer implementation using TPUART
 */

#include "tpuart_physical.hpp"
#include "knx/util/log.hpp"
#include "knx/util/result.hpp"
#include <algorithm>
#include <cstring>

static const char* TAG = "KNX.TP.UART";

// TPUART timing constants (in milliseconds)
static const uint32_t TPUART_RESET_DELAY = 50;
static const uint32_t TPUART_INIT_DELAY = 100;
static const uint32_t TPUART_TX_TIMEOUT = 50;
static const uint32_t TPUART_RX_TIMEOUT = 100;

namespace knx {
namespace physical {

TpuartPhysical::TpuartPhysical(platform::TimingPlatform& timing,
                               platform::QueuePlatform& queuePlatform,
                               platform::UartInterface& uart)
    : _timing(timing)
    , _queuePlatform(queuePlatform)
    , _uart(uart)
    , _state(PhysicalLayerState::Idle)
    , _initialized(false)
    , _busMonitorMode(false)
    , _rxState(RxState::Idle)
    , _rxBuffer{}
    , _rxDequeuedFrame{}
    , _rxIndex(0)
    , _rxExpectedLength(0)
    , _txPending(false)
    , _lastTxStatus(0)
    , _rxCallback(nullptr)
    , _rxCallbackContext(nullptr)
    , _rxQueue(nullptr)
{
}

TpuartPhysical::~TpuartPhysical() {
    close();
}

util::Result<void> TpuartPhysical::init() {
    if (_initialized) {
        KNX_LOGW(TAG, "Already initialized");
        return util::Result<void>::ok();
    }
    
    // Configure UART for TPUART (19200 baud, 8E1)
    platform::UartConfig config;
    config.baudRate = 19200;
    config.parity = platform::UartConfig::Parity::Even;
    config.stopBits = 1;
    config.rxBufferSize = 256;
    
        auto uartInit = _uart.init(config);
        if (uartInit.isError()) {
            KNX_LOGE(TAG, "Failed to initialize UART");
            return uartInit.error();
        }
    
    // Set RX callback
    _uart.setRxCallback(uartRxCallback, this);
    
    // Create RX queue for frame buffering
    _rxQueue = std::make_unique<platform::Queue>(_queuePlatform, RX_QUEUE_ITEM_SIZE, 8);
    if (!_rxQueue || !_rxQueue->handle()) {
        KNX_LOGE(TAG, "Failed to create RX queue");
        _uart.close();
        return util::ErrorCode::ResourceUnavailable;
    }
    
    // Reset TPUART
    auto resetRes = sendReset();
    if (resetRes.isError()) {
        KNX_LOGE(TAG, "Failed to reset TPUART");
        _rxQueue.reset();
        _uart.close();
        return resetRes.error();
    }
    
    _timing.delay(TPUART_RESET_DELAY);
    
    // Get product ID
    uint8_t manufacturer, device;
    auto productRes = getProductId(manufacturer, device);
    if (productRes.isOk()) {
        KNX_LOGI(TAG, "TPUART detected: Manufacturer=0x%02X, Device=0x%02X", 
                 manufacturer, device);
    }
    
    // Configure TPUART for addressed mode with auto-ACK
    auto configureRes = configure(true, true);
    if (configureRes.isError()) {
        KNX_LOGE(TAG, "Failed to configure TPUART");
        _rxQueue.reset();
        _uart.close();
        return configureRes.error();
    }
    
    _timing.delay(TPUART_INIT_DELAY);
    
    _state = PhysicalLayerState::Idle;
    _initialized = true;
    
    KNX_LOGI(TAG, "TPUART initialized successfully");
    return util::Result<void>::ok();
}

void TpuartPhysical::close() {
    if (_initialized) {
        _rxQueue.reset();
        _uart.close();
        _initialized = false;
        _state = PhysicalLayerState::Idle;
    }
}

bool TpuartPhysical::isOpen() const {
    return _initialized && _uart.isOpen();
}

util::Result<size_t> TpuartPhysical::sendFrame(std::span<const uint8_t> frame) {
    auto beginResult = beginTransmit(frame);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    auto pollResult = pollTransmit(beginResult.value());
    if (pollResult.isError()) {
        return pollResult.error();
    }

    switch (pollResult.value()) {
        case ProgressState::Success:
            return frame.size();
        case ProgressState::Busy:
            return util::ErrorCode::Busy;
        case ProgressState::Timeout:
            return util::ErrorCode::Timeout;
        default:
            return util::ErrorCode::TransmissionFailed;
    }
}

util::Result<size_t> TpuartPhysical::transmitFrameBlocking(std::span<const uint8_t> frame) {
    if (!_initialized) {
        KNX_LOGE(TAG, "Not initialized");
        return util::ErrorCode::NotInitialized;
    }
    if (frame.size() < MIN_TP1_FRAME_SIZE || frame.size() > MAX_TP1_FRAME_SIZE) {
        KNX_LOGE(TAG, "Invalid send parameters");
        return util::ErrorCode::InvalidFrameSize;
    }
    
    if (_busMonitorMode) {
        KNX_LOGW(TAG, "Bus monitor mode active; TX disabled");
        return util::ErrorCode::OperationNotSupported;
    }
    
    if (_state != PhysicalLayerState::Idle) {
        KNX_LOGW(TAG, "PHY not idle, cannot send");
        return util::ErrorCode::ResourceUnavailable;
    }
    
    _state = PhysicalLayerState::Transmitting;
    _lastTxStatus = 0;
    _txPending = true;
    
    // Send frame using TPUART protocol
    // First byte: U_L_Data.req with control field from frame
    uint8_t cmd = static_cast<uint8_t>(TpuartService::DataStart) | (frame[0] & 0x0F);
    if (_uart.write(cmd) != 1) {
        KNX_LOGE(TAG, "Failed to send start byte");
        _state = PhysicalLayerState::Idle;
        _txPending = false;
        return util::ErrorCode::TransmissionFailed;
    }
    
    // Send remaining bytes (except checksum which is added by TPUART)
    for (size_t i = 1; i < frame.size() - 1; i++) {
        if (_uart.write(frame[i]) != 1) {
            KNX_LOGE(TAG, "Failed to send data byte %zu", i);
            _state = PhysicalLayerState::Idle;
            _txPending = false;
            return util::ErrorCode::TransmissionFailed;
        }
    }
    
    // Send last byte with END marker
    uint8_t lastByte = static_cast<uint8_t>(TpuartService::DataEnd) | (frame[frame.size() - 1] & 0x3F);
    if (_uart.write(lastByte) != 1) {
        KNX_LOGE(TAG, "Failed to send end byte");
        _state = PhysicalLayerState::Idle;
        _txPending = false;
        return util::ErrorCode::TransmissionFailed;
    }
    
    // Wait for transmission confirmation
    const uint32_t startTime = _timing.millis();
    while (_txPending && (_timing.millis() - startTime) < TPUART_TX_TIMEOUT) {
        _timing.delay(1);
    }
    
    if (_txPending) {
        KNX_LOGW(TAG, "TX timeout");
        _state = PhysicalLayerState::Idle;
        _txPending = false;
        return util::ErrorCode::Timeout;
    }
    
    _state = PhysicalLayerState::Idle;
    
    // Decode confirmation status
    // Bit 7 = transmission successful
    if ((_lastTxStatus & 0x80) != 0) {
        return frame.size();
    }

    // BUSY condition (addressed device busy)
    if ((_lastTxStatus & static_cast<uint8_t>(TpuartService::Busy)) != 0) {
        KNX_LOGW(TAG, "TX busy: status=0x%02X", _lastTxStatus);
        return util::ErrorCode::Busy;
    }

    // NACK, collision, or other error
    KNX_LOGW(TAG, "TX NACK or error: status=0x%02X", _lastTxStatus);
    return util::ErrorCode::TransmissionFailed;
}

util::Result<uint32_t> TpuartPhysical::beginTransmit(std::span<const uint8_t> frame) {
    if (frame.size() < MIN_TP1_FRAME_SIZE || frame.size() > MAX_TP1_FRAME_SIZE) {
        return util::ErrorCode::InvalidFrameSize;
    }

    std::copy(frame.begin(), frame.end(), _progressTxFrame.begin());
    _progressTxLength = frame.size();
    auto transmitResult = transmitFrameBlocking(std::span<const uint8_t>(_progressTxFrame).first(_progressTxLength));
    _progressTxState = transmitResult.isOk() ? ProgressState::Success : util::progressStateFromError(transmitResult.error());
    _progressTxActive = true;
    return ++_progressTxSequence;
}

util::Result<TpuartPhysical::ProgressState> TpuartPhysical::pollTransmit(uint32_t sequence) {
    if (!_progressTxActive || sequence != _progressTxSequence) {
        return util::ErrorCode::OperationNotReady;
    }

    _progressTxActive = false;
    return _progressTxState;
}

util::Result<std::vector<uint8_t>> TpuartPhysical::receiveFrame(uint32_t timeoutMs) {
    auto viewResult = receiveFrameView(timeoutMs);
    if (viewResult.isError()) {
        return viewResult.error();
    }

    const auto frameView = viewResult.value();
    return std::vector<uint8_t>(frameView.begin(), frameView.end());
}

util::Result<void> TpuartPhysical::beginReceive(uint32_t timeoutMs) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }

    _progressRxActive = true;
    _progressRxDeadlineMs = _timing.millis() + timeoutMs;
    _progressRxLength = 0;
    _state = PhysicalLayerState::Receiving;
    return util::Result<void>::ok();
}

util::Result<TpuartPhysical::ProgressState> TpuartPhysical::pollReceive() {
    if (!_progressRxActive) {
        return util::ErrorCode::OperationNotReady;
    }

    if (_rxQueue && _rxQueue->receive(_rxDequeuedFrame.data(), 0)) {
        const size_t frameLen = _rxDequeuedFrame[0];
        if (frameLen < MIN_TP1_FRAME_SIZE || frameLen > MAX_TP1_FRAME_SIZE) {
            _progressRxActive = false;
            _state = PhysicalLayerState::Idle;
            return util::ErrorCode::InvalidFrameSize;
        }

        _progressRxLength = frameLen;
        _progressRxActive = false;
        _state = PhysicalLayerState::Idle;
        return ProgressState::Success;
    }

    if (_timing.millis() >= _progressRxDeadlineMs) {
        _progressRxActive = false;
        _state = PhysicalLayerState::Idle;
        return ProgressState::Timeout;
    }

    return ProgressState::Pending;
}

util::Result<std::span<const uint8_t>> TpuartPhysical::receivedFrameView() {
    if (_progressRxLength < MIN_TP1_FRAME_SIZE || _progressRxLength > MAX_TP1_FRAME_SIZE) {
        return util::ErrorCode::OperationNotReady;
    }

    return std::span<const uint8_t>(_rxDequeuedFrame).subspan(1, _progressRxLength);
}

util::Result<std::span<const uint8_t>> TpuartPhysical::receiveFrameView(uint32_t timeoutMs) {
    auto beginResult = beginReceive(timeoutMs);
    if (beginResult.isError()) {
        return beginResult.error();
    }

    while (true) {
        auto pollResult = pollReceive();
        if (pollResult.isError()) {
            return pollResult.error();
        }
        if (pollResult.value() == ProgressState::Pending) {
            _timing.delay(1);
            continue;
        }
        if (pollResult.value() == ProgressState::Timeout) {
            return util::ErrorCode::Timeout;
        }
        if (pollResult.value() != ProgressState::Success) {
            return util::ErrorCode::TransmissionFailed;
        }
        return receivedFrameView();
    }
}

void TpuartPhysical::setReceiveCallback(ReceiveCallback callback, void* context) {
    _rxCallback = std::move(callback);
    _rxCallbackContext = context;
}

PhysicalLayerState TpuartPhysical::getState() const {
    return _state;
}

util::Result<void> TpuartPhysical::setBusMonitorMode(Toggle mode) {
    if (!_initialized) {
        return util::ErrorCode::NotInitialized;
    }
    
    // Bus monitor mode: RX-only, no collision detection or TX/ACK responses.
    // Configure TPUART accordingly: disable addressed mode and auto-ACK when monitoring.
    const bool enable = isEnabled(mode);
    bool addressedMode = !enable;
    bool autoAck = !enable;

    auto configRes = configure(addressedMode, autoAck);
    if (configRes.isError()) {
        KNX_LOGW(TAG, "Failed to configure TPUART for %s mode",
                 enable ? "bus monitor" : "normal");
        return configRes.error();
    }

    _busMonitorMode = enable;
    KNX_LOGI(TAG, "Bus monitor mode: %s", enable ? "enabled" : "disabled");
    return util::Result<void>::ok();
}

// Static UART callback wrapper
void TpuartPhysical::uartRxCallback(void* context) {
    TpuartPhysical* self = static_cast<TpuartPhysical*>(context);
    if (self) {
        // Process all available bytes
        while (self->_uart.available() > 0) {
            int byte = self->_uart.read();
            if (byte >= 0) {
                self->processRxByte(static_cast<uint8_t>(byte));
            }
        }
    }
}

void TpuartPhysical::processRxByte(uint8_t byte) {
    // Check if this is a service byte (bit 7 set)
    if ((byte & 0x80) != 0) {
        handleServiceByte(byte);
    } else {
        handleDataByte(byte);
    }
}

void TpuartPhysical::handleServiceByte(uint8_t service) {
    // Check for L_Data.ind
    if ((service & 0xF0) == static_cast<uint8_t>(TpuartService::LDataInd)) {
        // Start of new frame
        _rxState = RxState::ReceivingControl;
        _rxBuffer[0] = static_cast<uint8_t>(service & 0x0F);  // Control field
        _rxIndex = 1;
        _rxExpectedLength = 0;
        
    } else if (service == static_cast<uint8_t>(TpuartService::LDataCon)) {
        // TX confirmation: next byte is status
        _rxState = RxState::ReceivingData;
        _rxExpectedLength = 1;
        _rxIndex = 0;
        
    } else {
        // Other service codes
        KNX_LOGD(TAG, "Service: 0x%02X", service);
    }
}

void TpuartPhysical::handleDataByte(uint8_t byte) {
    // TX confirmation status byte
    if (_rxState == RxState::ReceivingData && _rxExpectedLength == 1) {
        handleConfirmation(byte);
        _rxState = RxState::Idle;
        return;
    }

    if (_rxState == RxState::ReceivingControl || _rxState == RxState::ReceivingData) {
        if (_rxIndex >= MAX_TP1_FRAME_SIZE) {
            KNX_LOGW(TAG, "RX frame exceeds fixed TPUART buffer capacity");
            _rxState = RxState::Idle;
            return;
        }

        _rxBuffer[_rxIndex] = byte;
        _rxIndex++;
        
        // Determine expected length from control field
        if (_rxIndex == 6) {
            // Byte 6 contains length field
            size_t dataLen = (byte & 0x0F);
            _rxExpectedLength = 6 + dataLen + 1;  // Header + data + checksum
            
            if (_rxExpectedLength > MAX_TP1_FRAME_SIZE) {
                KNX_LOGW(TAG, "Invalid frame length: %zu", _rxExpectedLength);
                _rxState = RxState::Idle;
                return;
            }
        }
        
        // Check if frame is complete
        if (_rxIndex >= _rxExpectedLength && _rxExpectedLength > 0) {
            _rxState = RxState::Complete;
            
            // Verify checksum
            uint8_t expectedChecksum = calculateChecksum(std::span<const uint8_t>(_rxBuffer).first(_rxIndex - 1));
            if (_rxBuffer[_rxIndex - 1] == expectedChecksum) {
                // Frame valid - queue it or notify callback
                if (_rxCallback) {
                    _rxCallback(_rxCallbackContext);
                }
                
                // Also queue for polling
                // Use stack-based frame to avoid dynamic allocation
                // Frame format: [length][data...]
                uint8_t frameBuffer[RX_QUEUE_ITEM_SIZE]{};
                frameBuffer[0] = static_cast<uint8_t>(_rxIndex);
                std::memcpy(&frameBuffer[1], _rxBuffer.data(), _rxIndex);
                if (!_rxQueue || !_rxQueue->send(frameBuffer, 0)) {
                    KNX_LOGW(TAG, "RX queue full, frame dropped");
                }
            } else {
                KNX_LOGW(TAG, "Checksum error: expected 0x%02X, got 0x%02X", 
                         expectedChecksum, _rxBuffer[_rxIndex - 1]);
            }
            
            _rxState = RxState::Idle;
        }
    }
}

void TpuartPhysical::handleConfirmation(uint8_t status) {
    _lastTxStatus = status;
    _txPending = false;
}

util::Result<void> TpuartPhysical::sendReset() {
    uint8_t cmd = static_cast<uint8_t>(TpuartService::Reset);
    if (_uart.write(cmd) == 1) return util::Result<void>::ok();
    return util::ErrorCode::TransmissionFailed;
}

util::Result<void> TpuartPhysical::sendState() {
    uint8_t cmd = static_cast<uint8_t>(TpuartService::State);
    if (_uart.write(cmd) == 1) return util::Result<void>::ok();
    return util::ErrorCode::TransmissionFailed;
}

util::Result<void> TpuartPhysical::getProductId(uint8_t& manufacturer, uint8_t& device) {
    uint8_t cmd = static_cast<uint8_t>(TpuartService::ProductId);
    if (_uart.write(cmd) != 1) {
        return util::ErrorCode::TransmissionFailed;
    }
    
    // Wait for 2-byte response
    _timing.delay(10);
    
    uint8_t response[2];
    if (_uart.read(std::span<uint8_t>(response, 2)) == 2) {
        manufacturer = response[0];
        device = response[1];
        return util::Result<void>::ok();
    }
    
    return util::ErrorCode::Timeout;
}

util::Result<void> TpuartPhysical::configure(bool addressedMode, bool autoAck) {
    uint8_t cmd = static_cast<uint8_t>(TpuartService::Configure);
    uint8_t config = 0x00;
    
    if (addressedMode) config |= 0x04;  // Bit 2: Enable addressed mode
    if (autoAck) config |= 0x01;        // Bit 0: Enable auto-ACK
    
    uint8_t data[2] = { cmd, config };
    if (_uart.write(std::span<const uint8_t>(data, 2)) == 2) return util::Result<void>::ok();
    return util::ErrorCode::TransmissionFailed;
}

uint8_t TpuartPhysical::calculateChecksum(std::span<const uint8_t> data) const {
    uint8_t checksum = 0xFF;
    for (uint8_t byte : data) {
        checksum ^= byte;
    }
    return checksum;
}

} // namespace physical
} // namespace knx
