// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bitbang_driver_tp1_interface.hpp
 * @brief Optional TP1 control/event extension for BitBang drivers
 */

#pragma once

#include "knx/physical/tp1_medium_backend.hpp"

#include <span>

namespace knx {
namespace physical {

class BitBangDriverTp1Interface {
public:
    virtual ~BitBangDriverTp1Interface() = default;

    virtual util::Result<void> setOwnAddress(uint16_t addressRaw) = 0;
    virtual util::Result<void> setBusMonitorMode(bool enabled) = 0;

    // ACK decision inputs, configured ahead of time from task context. The
    // DL-ACK decision itself is made inside the driver's ISR as the frame
    // header streams in — the only context that can meet the 15-bit-time
    // t_ack deadline. There is deliberately no per-telegram ACK submission
    // API: a task-scheduled decision can never reliably beat that deadline.
    virtual void setAckGroupAddresses(std::span<const uint16_t> addresses) = 0;
    virtual void setLocalBusy(bool busy) = 0;

    virtual void pollTp1() = 0;
    virtual bool popTp1Event(Tp1RxEvent& outEvent) = 0;

    virtual Tp1AckDiagnosticsSnapshot getTp1AckDiagnostics() const = 0;

    // Link health, when the board wires the transceiver's bus-health output
    // (STKNX KNX_OK and equivalents) to the driver. Defaulted so drivers
    // without the signal need no changes.
    virtual bool hasTp1LinkStateIndication() const { return false; }
    virtual LinkState getTp1LinkState() const { return LinkState::Unknown; }
};

} // namespace physical
} // namespace knx
