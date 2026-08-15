// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_mac_controller.hpp
 * @brief TP1 medium-neutral MAC controller skeleton
 */

#pragma once

#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/util/inplace_function.hpp"

#include <array>
#include <cstddef>
#include <span>

namespace knx {
namespace physical {

// Medium-neutral TP1 MAC controller.
//
// Note on acknowledgement: this controller deliberately has NO per-telegram
// ACK decision logic. The DL-ACK decision must be made within t_ack (15
// bit-times, ~1.56 ms at 9600 baud) of the frame end — a deadline only the
// medium's own timing domain (bitbang ISR, TPUART silicon) can reliably meet.
// The controller's role is limited to forwarding the decision INPUTS (own
// address, subscribed group addresses, busy state) to the backend ahead of
// time and tracking TX-side ACK outcomes.
class Tp1MacController {
public:
    explicit Tp1MacController(Tp1MediumBackend& backend);

    util::Result<void> init(const Tp1MediumConfig& config);
    util::Result<void> setOwnAddress(uint16_t addressRaw);
    void close();

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame);
    bool supportsDiagnosticsSnapshot() const;
    util::Result<void> requestDiagnosticsSnapshot();
    void setDiagnosticsSnapshotPeriod(uint32_t ticks);
    void setDiagnosticsRetryBackoffTicks(uint32_t ticks);
    util::Result<void> onTick();

    void setEventObserver(Tp1EventCallback callback, void* context);

    void onBackendEvent(const Tp1RxEvent& event);

    Tp1AckClass getLastTxAckResponse() const;
    bool didLastTxAckMissDeadline() const;
    uint32_t getTxOutcomeSequence() const;
    bool hasAckDiagnosticsSnapshot() const;
    Tp1AckDiagnosticsSnapshot getLatestAckDiagnosticsSnapshot() const;
    void setLocalBusy(bool busy);
    void setAckGroupAddresses(std::span<const uint16_t> addresses);

private:
    static void backendEventShim(const Tp1RxEvent& event, void* context);

    Tp1MediumBackend& _backend;
    Tp1EventCallback _eventObserver;
    void* _eventObserverContext;
    Tp1AckClass _lastTxAckResponse;
    bool _lastTxAckDeadlineMiss;
    uint32_t _txOutcomeSequence;
    bool _hasAckDiagnosticsSnapshot;
    Tp1AckDiagnosticsSnapshot _latestAckDiagnosticsSnapshot;
    uint32_t _diagnosticsSnapshotPeriodTicks;
    uint32_t _diagnosticsTickCounter;
    uint32_t _diagnosticsRetryBackoffTicks;
    uint32_t _diagnosticsRetryCooldownTicks;
};

} // namespace physical
} // namespace knx