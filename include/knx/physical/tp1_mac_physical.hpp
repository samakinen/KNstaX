// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_mac_physical.hpp
 * @brief Canonical TP1 physical facade using owned Tp1MediumBackend + Tp1MacController composition
 */

#pragma once

#include "knx/physical/tp1_mac_controller.hpp"
#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/platform/platform.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>

namespace knx {

// Forward declaration: only a pointer to it crosses this interface, and the
// physical layer must not pull in the data link header.
namespace datalink {
struct LDataFrame;
} // namespace datalink

namespace physical {

using Tp1TxOutcomeState = TxOutcomeState;

class Tp1MacPhysical {
public:
    using ProgressState = util::OperationProgressState;
    using QueueNotifyCallback = Tp1MediumBackend::QueueNotifyCallback;

    static constexpr size_t RX_QUEUE_CAPACITY = 32;
    static constexpr size_t MAX_RX_FRAME_SIZE = 256;
    static constexpr uint32_t DEFAULT_TX_OUTCOME_TIMEOUT_MS = 20u;

    explicit Tp1MacPhysical(std::unique_ptr<Tp1MediumBackend> backend);
    ~Tp1MacPhysical();

    util::Result<void> init();
    void close();
    bool isOpen() const;

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame);
    util::Result<uint32_t> beginTransmit(std::span<const uint8_t> frame);
    util::Result<ProgressState> pollTransmit(uint32_t initialSequence);
    util::Result<void> setOwnAddress(uint16_t addressRaw);
    /// Publish the subscribed group addresses the medium should L_ACK.
    /// Forwarded to the backend's timing domain (ISR/hardware), where the
    /// per-telegram ACK decision is made.
    void setAckGroupAddresses(std::span<const uint16_t> addresses);
    util::Result<std::vector<uint8_t>> receiveFrame(uint32_t timeoutMs);
    util::Result<void> beginReceive(uint32_t timeoutMs);
    util::Result<ProgressState> pollReceive();
    util::Result<std::span<const uint8_t>> receivedFrameView();

    void setReceiveCallback(ReceiveCallback callback, void* context);
    void setQueueNotifyCallback(QueueNotifyCallback callback, void* context);
    void setTimingPlatform(platform::TimingPlatform* timingPlatform);
    void setTxOutcomeTimeoutMs(uint32_t timeoutMs);
    uint32_t txOutcomeTimeoutMs() const;

    PhysicalLayerState getState() const;
    util::Result<void> setBusMonitorMode(Toggle mode);

    /**
     * @brief Current health of the medium underneath this physical
     *
     * LinkState::Unknown means the backend has no way to tell (no bus-health
     * signal wired, or a backend that does not implement the indication).
     */
    LinkState getLinkState() const;

    /**
     * @brief Observe debounced link transitions
     *
     * Invoked from whichever task drives the medium's event pump, so the
     * handler may log and take its time — it is not an interrupt callback.
     */
    void setLinkStateCallback(LinkStateCallback callback, void* context);

    /**
     * @brief Refuse transmission while the link is known Down (default on)
     *
     * With a known-dead bus, every frame would otherwise run its full CSMA and
     * retransmission budget before failing. Gating turns that into an immediate
     * ResourceUnavailable. LinkState::Unknown never gates, so backends without
     * a health signal are unaffected.
     */
    void setTxGatedByLinkDown(bool enabled);

private:
    struct BufferedFrame {
        std::array<uint8_t, MAX_RX_FRAME_SIZE> data{};
        size_t length{0};
    };

    static void onTp1Event(const Tp1RxEvent& event, void* context);
    void handleTp1Event(const Tp1RxEvent& event);
    void handleLinkStateEvent(const LinkStatus& status);

    static PhysicalLayerState mapState(Tp1MediumState state);
    /// @param decoded Optional out-parameter receiving the decoded frame, so
    ///                callers do not have to decode a second time.
    util::Result<void> validateOutgoingFrame(std::span<const uint8_t> frame,
                                             datalink::LDataFrame* decoded = nullptr) const;
    util::Result<void> waitForTxOutcome(uint32_t initialSequence);

    bool _initialized;
    bool _busMonitorMode;
    std::unique_ptr<Tp1MediumBackend> _backend;
    Tp1MacController _controller;
    Tp1MediumConfig _config;
    platform::TimingPlatform* _timingPlatform;
    ReceiveCallback _rxCallback;
    void* _rxCallbackContext;
    uint32_t _txOutcomeTimeoutMs;
    bool _txAckRequested{false};
    // Written by the medium's event pump, read by whichever task transmits.
    std::atomic<LinkState> _linkState{LinkState::Unknown};
    bool _gateTxOnLinkDown{true};
    LinkStateCallback _linkStateCallback{nullptr};
    void* _linkStateCallbackContext{nullptr};
    std::mutex _rxMutex;
    std::array<BufferedFrame, RX_QUEUE_CAPACITY> _rxFrames;
    size_t _rxHead;
    size_t _rxCount;
    size_t _rxOverflowDrops{0};
    bool _receiveActive{false};
    uint64_t _receiveDeadlineMs{0};
    std::array<uint8_t, MAX_RX_FRAME_SIZE> _lastReceivedFrame{};
    size_t _lastReceivedFrameLength{0};
};

} // namespace physical
} // namespace knx