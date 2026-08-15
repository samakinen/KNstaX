// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file fault_model.hpp
 * @brief Fault classification and recovery model for KNX product-layer objects.
 *
 * CommissionedProductRuntime exposes an onFault() callback that fires
 * whenever the stack encounters an unrecoverable or operationally-significant
 * fault.  Firmware can use this to:
 *
 *  - Drive a fault LED or alarm output.
 *  - Log structured diagnostic information.
 *  - Trigger a software watchdog reset.
 *  - Attempt an ordered recovery (see FaultCode documentation below).
 *
 * Recovery transitions (per FaultCode):
 *
 *  StartFailed:
 *    The stack did not initialise.  Call stop(), correct the configuration,
 *    and retry start().  If the fault repeats, the hardware or NVS is faulty.
 *
 *  PhysicalLayerLost:
 *    The TP1 medium or IP interface has become unavailable.  The stack will
 *    not automatically reconnect.  Firmware should call stop() and re-start
 *    the device (or trigger a watchdog reset for simplicity).
 *
 *  TxQueueOverflow:
 *    The outgoing frame queue is full.  Firmware can throttle its send() calls.
 *    This is a soft fault — the device remains operational.
 *
 *  PersistenceError:
 *    A save or load to NVS/flash failed.  ETS-programmed tables may be lost
 *    after reset.  Firmware should log and alert the operator.
 *
 *  InternalError:
 *    An unexpected internal assertion or protocol error occurred.  Firmware
 *    should trigger a watchdog reset for deterministic recovery.
 */

#pragma once

#include <cstdint>

#include "knx/util/inplace_function.hpp"

namespace knx::product {

/// Classification of fault events reported to the firmware via onFault().
enum class FaultCode : uint8_t {
    /// Stack failed to start (hardware init error, bad config, or NVS failure).
    StartFailed         = 0x01,
    /// Physical layer connection was lost after a successful start.
    PhysicalLayerLost   = 0x02,
    /// Outgoing frame queue overflowed (soft fault, device still operational).
    TxQueueOverflow     = 0x03,
    /// Persistence (NVS/flash) read or write failed.
    PersistenceError    = 0x04,
    /// Unexpected internal error; a reset is the safest recovery action.
    InternalError       = 0xFF,
};

/// Structured fault event delivered to the firmware's onFault() callback.
struct FaultInfo {
    /// Fault classification.
    FaultCode   code    = FaultCode::InternalError;
    /// Optional human-readable description (null-terminated, may be nullptr).
    const char* detail  = nullptr;

    /// Convenience constructor.
    constexpr FaultInfo(FaultCode c, const char* d = nullptr) noexcept
        : code(c), detail(d) {}
};

/// Signature of the fault callback registered via onFault().
using FaultCallback = util::InplaceFunction<void(FaultInfo), 32>;

} // namespace knx::product
