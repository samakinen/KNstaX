// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_medium_backend.hpp
 * @brief Medium-neutral TP1 backend contract for Bitbang and TPUART-class backends
 */

#pragma once

#include "knx/physical/link_state.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"
#include <cstdint>
#include <cstddef>
#include <span>

namespace knx {
namespace physical {

enum class Tp1MediumState : uint8_t {
    Uninitialized,
    Idle,
    Receiving,
    Transmitting,
    Error
};

enum class Tp1AckClass : uint8_t {
    None,
    Ack,
    Nack,
    Busy,
    NackBusy
};

enum class Tp1RxEventType : uint8_t {
    TelegramStart,
    DataByte,
    TelegramEnd,
    ReceiveError,
    RxAckObserved,
    TxAckResponse,
    TxAckDeadlineMiss,
    AckDiagnosticsSnapshot,
    MediumStateChanged,
    /// Debounced change of medium health (bus power, interface/tunnel up-down).
    /// Carries Tp1RxEvent::linkStatus; the frame span is empty.
    LinkStateChanged
};

struct Tp1AckDiagnosticsSnapshot {
    uint32_t windowOpenedNoDecisionCount{0};
    uint32_t decisionLatchedLateCount{0};
    uint32_t responseEmittedCount{0};
    uint32_t deadlineMissCount{0};
    uint32_t overflowErrorCount{0};
    uint32_t rxAckObservedCount{0};
    uint32_t unsupportedRawIngressCount{0};
};

struct Tp1CapabilityProfile {
    bool supportsHardwareAutoAck{false};
    bool supportsAddressedFiltering{false};
    bool supportsByteEventStream{false};
    bool supportsDetailedTxConfirm{false};
    bool supportsCollisionIndication{false};
    bool supportsDiagnosticsSnapshot{false};
    /// Backend can report medium health independently of bus traffic.
    bool supportsLinkStateIndication{false};
};

struct Tp1MediumConfig {
    uint16_t ownIndividualAddressRaw{0};
    bool busMonitorMode{false};
};

struct Tp1RxEvent {
    Tp1RxEventType type{Tp1RxEventType::MediumStateChanged};
    uint8_t dataByte{0};
    uint8_t errorFlags{0};
    Tp1AckClass ackClass{Tp1AckClass::None};
    Tp1AckDiagnosticsSnapshot ackDiagnostics{};
    LinkStatus linkStatus{};
    // Valid only for the duration of the callback that receives this event.
    std::span<const uint8_t> frame{};
};

using Tp1EventCallback = util::InplaceFunction<void(const Tp1RxEvent&, void* context), 64>;

class Tp1MediumDiagnostics {
public:
    virtual ~Tp1MediumDiagnostics() = default;
    virtual util::Result<void> requestDiagnosticsSnapshot() = 0;
};

class Tp1MediumBackend {
public:
    using QueueNotifyCallback = void(*)(void* context);

    virtual ~Tp1MediumBackend() = default;

    virtual util::Result<void> init(const Tp1MediumConfig& config) = 0;
    virtual void close() = 0;

    virtual util::Result<size_t> sendFrame(std::span<const uint8_t> frame) = 0;

    virtual void setEventCallback(Tp1EventCallback callback, void* context) = 0;

    virtual Tp1MediumState getState() const = 0;
    virtual Tp1CapabilityProfile getCapabilities() const = 0;

    /// Current medium health. The default reports Unknown, which the layers
    /// above treat as "assume usable" — a backend without a health signal
    /// therefore needs no changes and causes no behaviour change.
    virtual LinkState getLinkState() const { return LinkState::Unknown; }

    virtual util::Result<void> setBusMonitorMode(bool enabled) = 0;
    virtual util::Result<void> service() = 0;
    virtual util::Result<void> setOwnAddress(uint16_t addressRaw) { return util::Result<void>::ok(); }

    virtual void setQueueNotifyCallback(QueueNotifyCallback callback, void* context) {
        (void)callback;
        (void)context;
    }

    // ── ACK decision inputs (task context, ahead of time) ──────────────────
    // The DL-ACK decision itself belongs to the medium's own timing domain
    // (bitbang ISR, TPUART silicon): a task-scheduled per-telegram decision
    // can never reliably meet the 15-bit-time t_ack deadline, so no such API
    // exists. Backends that decide autonomously consume these inputs; the
    // defaults are no-ops for media that manage them elsewhere (e.g. a TPUART
    // configured through its own address services).
    virtual void setAckGroupAddresses(std::span<const uint16_t> addresses) {
        (void)addresses;
    }
    virtual void setLocalBusy(bool busy) { (void)busy; }

    virtual Tp1MediumDiagnostics* diagnostics() { return nullptr; }
    virtual const Tp1MediumDiagnostics* diagnostics() const { return nullptr; }
};

} // namespace physical
} // namespace knx
