// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file esp32_uart_build_config.hpp
 * @brief ESP32 UART build-time pin configuration helpers
 */

#pragma once

#include "knx/util/result.hpp"

namespace knx {
namespace platform {

inline constexpr int kEsp32UartUnusedPin = -1;

struct Esp32UartBuildConfig {
    int txPin{kEsp32UartUnusedPin};
    int rxPin{kEsp32UartUnusedPin};
    int rtsPin{kEsp32UartUnusedPin};
    int ctsPin{kEsp32UartUnusedPin};
};

constexpr bool esp32UartPinAssigned(int pin) {
    return pin >= 0;
}

constexpr bool esp32UartHardwareFlowControlConfigured(const Esp32UartBuildConfig& config) {
    return esp32UartPinAssigned(config.rtsPin) && esp32UartPinAssigned(config.ctsPin);
}

constexpr bool esp32UartHardwareFlowControlPartiallyConfigured(const Esp32UartBuildConfig& config) {
    return esp32UartPinAssigned(config.rtsPin) != esp32UartPinAssigned(config.ctsPin);
}

constexpr bool shouldEnableEsp32UartHardwareFlowControl(const Esp32UartBuildConfig& config,
                                                        bool runtimeFlowControlRequest) {
    return runtimeFlowControlRequest || esp32UartHardwareFlowControlConfigured(config);
}

inline util::Result<void> validateEsp32UartBuildConfig(const Esp32UartBuildConfig& config,
                                                       bool useHardwareFlowControl) {
    if (!esp32UartPinAssigned(config.txPin) || !esp32UartPinAssigned(config.rxPin)) {
        return util::ErrorCode::InvalidParameter;
    }

    if (config.txPin == config.rxPin) {
        return util::ErrorCode::InvalidParameter;
    }

    if (esp32UartHardwareFlowControlPartiallyConfigured(config)) {
        return util::ErrorCode::InvalidParameter;
    }

    if (useHardwareFlowControl && !esp32UartHardwareFlowControlConfigured(config)) {
        return util::ErrorCode::InvalidParameter;
    }

    if (esp32UartPinAssigned(config.rtsPin) &&
        (config.rtsPin == config.txPin || config.rtsPin == config.rxPin)) {
        return util::ErrorCode::InvalidParameter;
    }

    if (esp32UartPinAssigned(config.ctsPin) &&
        (config.ctsPin == config.txPin || config.ctsPin == config.rxPin || config.ctsPin == config.rtsPin)) {
        return util::ErrorCode::InvalidParameter;
    }

    return util::Result<void>::ok();
}

} // namespace platform
} // namespace knx