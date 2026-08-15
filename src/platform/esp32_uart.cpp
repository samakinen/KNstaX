// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_uart.cpp
 * @brief ESP32 UART implementation
 */

#include "esp32_uart.hpp"
#include "knx/platform/esp32_uart_build_config.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "knx/util/log.hpp"
#include <algorithm>

static const char* TAG = "KNX_UART";

namespace {

constexpr uint8_t kHardwareFlowControlThreshold = 120;

knx::platform::Esp32UartBuildConfig selectedBuildConfig() {
    knx::platform::Esp32UartBuildConfig config;

#if defined(CONFIG_KNX_TP1_UART_TX_PIN)
    config.txPin = CONFIG_KNX_TP1_UART_TX_PIN;
#endif
#if defined(CONFIG_KNX_TP1_UART_RX_PIN)
    config.rxPin = CONFIG_KNX_TP1_UART_RX_PIN;
#endif
#if defined(CONFIG_KNX_TP1_UART_RTS_PIN)
    config.rtsPin = CONFIG_KNX_TP1_UART_RTS_PIN;
#endif
#if defined(CONFIG_KNX_TP1_UART_CTS_PIN)
    config.ctsPin = CONFIG_KNX_TP1_UART_CTS_PIN;
#endif

    return config;
}

uint8_t hardwareFlowControlThreshold(size_t rxBufferSize) {
    if (rxBufferSize <= 1) {
        return 1;
    }

    return static_cast<uint8_t>(std::min<size_t>(kHardwareFlowControlThreshold, rxBufferSize - 1));
}

} // namespace

namespace knx {
namespace platform {

Esp32Uart::Esp32Uart() 
    : _uartNum(UART_NUM_1)
    , _initialized(false)
    , _rxCallback(nullptr)
    , _rxCallbackContext(nullptr)
    , _eventQueue(nullptr)
    , _eventTaskHandle(nullptr)
    , _overflowFlag(false) {
}

Esp32Uart::~Esp32Uart() {
    close();
}

util::Result<void> Esp32Uart::init(const UartConfig& config) {
    if (_initialized) {
        KNX_LOGW(TAG, "UART already initialized");
        return util::Result<void>::ok();
    }

    auto validation = config.validate();
    if (validation.isError()) {
        KNX_LOGE(TAG, "Invalid UART configuration");
        return validation.error();
    }

    const Esp32UartBuildConfig buildConfig = selectedBuildConfig();
    const bool useHardwareFlowControl =
        shouldEnableEsp32UartHardwareFlowControl(buildConfig, config.flowControl);

    auto buildValidation = validateEsp32UartBuildConfig(buildConfig, useHardwareFlowControl);
    if (buildValidation.isError()) {
        KNX_LOGE(TAG, "Invalid ESP32 UART pin configuration");
        return buildValidation.error();
    }
    
    // Use build-selected TPUART UART when available.
#if defined(CONFIG_KNX_TP1_UART_NUM)
    _uartNum = static_cast<uart_port_t>(CONFIG_KNX_TP1_UART_NUM);
#else
    _uartNum = UART_NUM_1;
#endif
    
    // Convert parity
    uart_parity_t parity;
    switch (config.parity) {
        case UartConfig::Parity::None: parity = UART_PARITY_DISABLE; break;
        case UartConfig::Parity::Even: parity = UART_PARITY_EVEN; break;
        case UartConfig::Parity::Odd:  parity = UART_PARITY_ODD; break;
        default:
            KNX_LOGE(TAG, "Invalid parity");
            return util::ErrorCode::InvalidParameter;
    }
    
    uart_word_length_t dataBits;
    switch (config.dataBits) {
        case 5: dataBits = UART_DATA_5_BITS; break;
        case 6: dataBits = UART_DATA_6_BITS; break;
        case 7: dataBits = UART_DATA_7_BITS; break;
        case 8: dataBits = UART_DATA_8_BITS; break;
        default:
            KNX_LOGE(TAG, "Invalid data bits: %u", static_cast<unsigned>(config.dataBits));
            return util::ErrorCode::InvalidParameter;
    }

    // Convert stop bits (1 or 2)
    uart_stop_bits_t stopBits;
    if (config.stopBits == 1) {
        stopBits = UART_STOP_BITS_1;
    } else if (config.stopBits == 2) {
        stopBits = UART_STOP_BITS_2;
    } else {
        KNX_LOGE(TAG, "Invalid stop bits: %u", static_cast<unsigned>(config.stopBits));
        return util::ErrorCode::InvalidParameter;
    }
    
    // UART configuration (zero-init first to keep compatibility with newer
    // IDF fields like rx_glitch_filt_thresh and flags).
    uart_config_t uart_config{};
    uart_config.baud_rate = static_cast<int>(config.baudRate);
    uart_config.data_bits = dataBits;
    uart_config.parity = parity;
    uart_config.stop_bits = stopBits;
    uart_config.flow_ctrl = useHardwareFlowControl ? UART_HW_FLOWCTRL_CTS_RTS : UART_HW_FLOWCTRL_DISABLE;
    uart_config.rx_flow_ctrl_thresh = useHardwareFlowControl ? hardwareFlowControlThreshold(config.rxBufferSize) : 0;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    
    // Install UART driver with event queue
    const int uart_buffer_size = config.rxBufferSize > 0 ? config.rxBufferSize : 256;
    const int tx_buffer_size = config.txBufferSize > 0 ? config.txBufferSize : uart_buffer_size;
    const int event_queue_size = 10;
    _eventQueue = nullptr;
    
    esp_err_t err = uart_driver_install(_uartNum, 
                                       uart_buffer_size * 2, 
                                       tx_buffer_size,
                                       event_queue_size,
                                       &_eventQueue,
                                       0);
    if (err != ESP_OK) {
        KNX_LOGE(TAG, "Failed to install UART driver: %s", esp_err_to_name(err));
        return util::ErrorCode::OperationFailed;
    }
    
    // Configure UART parameters
    err = uart_param_config(_uartNum, &uart_config);
    if (err != ESP_OK) {
        KNX_LOGE(TAG, "Failed to configure UART: %s", esp_err_to_name(err));
        uart_driver_delete(_uartNum);
        _eventQueue = nullptr;
        return util::ErrorCode::OperationFailed;
    }
    
    err = uart_set_pin(_uartNum,
                       buildConfig.txPin,
                       buildConfig.rxPin,
                       useHardwareFlowControl ? buildConfig.rtsPin : UART_PIN_NO_CHANGE,
                       useHardwareFlowControl ? buildConfig.ctsPin : UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        KNX_LOGE(TAG, "Failed to configure UART pins: %s", esp_err_to_name(err));
        uart_driver_delete(_uartNum);
        _eventQueue = nullptr;
        return util::ErrorCode::OperationFailed;
    }

    _overflowFlag = false;
    _initialized = true;

    auto startResult = startEventTask();
    if (startResult.isError()) {
        uart_driver_delete(_uartNum);
        _eventQueue = nullptr;
        _initialized = false;
        return startResult.error();
    }

    KNX_LOGI(TAG, "UART%d initialized: %u baud, %d%c%d", 
             static_cast<int>(_uartNum), 
             static_cast<unsigned>(config.baudRate),
             static_cast<int>(config.dataBits),
             parity == UART_PARITY_EVEN ? 'E' : (parity == UART_PARITY_ODD ? 'O' : 'N'),
             stopBits == UART_STOP_BITS_1 ? 1 : 2);
    KNX_LOGI(TAG, "UART flow control: %s", useHardwareFlowControl ? "CTS/RTS" : "disabled");
    
    return util::Result<void>::ok();
}

void Esp32Uart::close() {
    if (_initialized) {
        stopEventTask();
        uart_driver_delete(_uartNum);
        _eventQueue = nullptr;
        _initialized = false;
        _overflowFlag = false;
    }
}

bool Esp32Uart::isOpen() const {
    return _initialized;
}

size_t Esp32Uart::available() const {
    if (!_initialized) return 0;
    
    size_t available = 0;
    uart_get_buffered_data_len(_uartNum, &available);
    return available;
}

int Esp32Uart::read() {
    if (!_initialized) return -1;
    
    uint8_t byte;
    int len = uart_read_bytes(_uartNum, &byte, 1, 0);
    return len > 0 ? byte : -1;
}

size_t Esp32Uart::read(std::span<uint8_t> buffer) {
    if (!_initialized || buffer.size() == 0) return 0;

    int len = uart_read_bytes(_uartNum, buffer.data(), buffer.size(), 0);
    return len > 0 ? static_cast<size_t>(len) : 0;
}

size_t Esp32Uart::write(uint8_t byte) {
    if (!_initialized) return 0;
    
    return uart_write_bytes(_uartNum, &byte, 1);
}

size_t Esp32Uart::write(std::span<const uint8_t> data) {
    if (!_initialized || data.size() == 0) return 0;

    return uart_write_bytes(_uartNum, data.data(), data.size());
}

void Esp32Uart::flush() {
    if (_initialized) {
        uart_wait_tx_done(_uartNum, portMAX_DELAY);
    }
}

void Esp32Uart::clear() {
    if (_initialized) {
        uart_flush_input(_uartNum);
        if (_eventQueue) {
            xQueueReset(_eventQueue);
        }
        _overflowFlag = false;
    }
}

bool Esp32Uart::overflow() const {
    return _overflowFlag;
}

void Esp32Uart::setRxCallback(RxCallback callback, void* context) {
    _rxCallback = callback;
    _rxCallbackContext = context;

    if (!_initialized) {
        return;
    }

    if (_rxCallback) {
        auto startResult = startEventTask();
        if (startResult.isError()) {
            KNX_LOGE(TAG, "Failed to start UART event task after callback registration");
            return;
        }

        if (available() > 0) {
            _rxCallback(_rxCallbackContext);
        }
    } else {
        stopEventTask();
    }
}

util::Result<void> Esp32Uart::startEventTask() {
    if (!_initialized || !_rxCallback) {
        return util::Result<void>::ok();
    }

    if (_eventTaskHandle != nullptr) {
        return util::Result<void>::ok();
    }

    if (_eventQueue == nullptr) {
        KNX_LOGE(TAG, "UART event queue not available");
        return util::ErrorCode::OperationFailed;
    }

    BaseType_t created = xTaskCreate(uartEventTask, "uart_events", 3072, this, 12, &_eventTaskHandle);
    if (created != pdPASS) {
        _eventTaskHandle = nullptr;
        KNX_LOGE(TAG, "Failed to create UART event task");
        return util::ErrorCode::ResourceUnavailable;
    }

    return util::Result<void>::ok();
}

void Esp32Uart::stopEventTask() {
    if (_eventTaskHandle != nullptr) {
        vTaskDelete(_eventTaskHandle);
        _eventTaskHandle = nullptr;
    }
}

void Esp32Uart::uartEventTask(void* arg) {
    Esp32Uart* uart = static_cast<Esp32Uart*>(arg);

    uart_event_t event;
    while (uart != nullptr && uart->_initialized) {
        if (uart->_eventQueue == nullptr) {
            break;
        }

        if (xQueueReceive(uart->_eventQueue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        switch (event.type) {
            case UART_DATA:
                if (uart->_rxCallback != nullptr) {
                    uart->_rxCallback(uart->_rxCallbackContext);
                }
                break;
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
                uart->_overflowFlag = true;
                uart_flush_input(uart->_uartNum);
                xQueueReset(uart->_eventQueue);
                KNX_LOGW(TAG, "UART RX overflow detected");
                break;
            case UART_BREAK:
            case UART_PARITY_ERR:
            case UART_FRAME_ERR:
                uart->_overflowFlag = true;
                KNX_LOGW(TAG, "UART line error event: %d", static_cast<int>(event.type));
                break;
            default:
                break;
        }
    }

    if (uart != nullptr) {
        uart->_eventTaskHandle = nullptr;
    }

    vTaskDelete(nullptr);
}

} // namespace platform
} // namespace knx
