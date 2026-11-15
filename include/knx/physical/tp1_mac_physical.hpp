// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_mac_physical.hpp
 * @brief The TP1 physical layer: frame-oriented MAC over an owned Tp1MediumBackend
 */

#pragma once

#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/platform/platform.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include "knx/platform/mutex.hpp"
#include <span>

namespace knx {

// Forward declaration: only a pointer to it crosses this interface, and the
// physical layer must not pull in the data link header.
namespace datalink {
struct LDataFrame;
} // namespace datalink

namespace physical {

using Tp1TxOutcomeState = TxOutcomeState;

/**
 * @brief The TP1 physical layer the data link layer drives (Tp1PhysicalLike).
 *
 * Owns one medium backend and turns its events into frame-oriented
 * behaviour: an RX queue with BUSY back-pressure, and TX outcomes built from
 * the backend's TX-ACK events.
 *
 * Note on acknowledgement: there is deliberately NO per-telegram ACK decision
 * here. The DL-ACK decision must be made within t_ack (15 bit-times,
 * ~1.56 ms at 9600 baud) of the frame end — a deadline only the medium's own
 * timing domain (bitbang ISR, TPUART silicon) can reliably meet. This class
 * forwards the decision INPUTS (own address, subscribed group addresses, busy
 * state) to the backend ahead of time and tracks TX-side ACK outcomes.
 */
class Tp1MacPhysical {
public:
    using ProgressState = util::OperationProgressState;
    using QueueNotifyCallback = Tp1MediumBackend::QueueNotifyCallback;

    static constexpr size_t RX_QUEUE_CAPACITY = 32;
    /// Free slots kept back when the queue fills: once fewer remain, the ISR
    /// answers new frames with BUSY instead of ACK. They absorb frames that
    /// were acknowledged before the busy state reached the ISR.
    static constexpr size_t RX_BUSY_RESERVE = 4;
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
    /// Publish the Individual Addresses the medium should L_ACK besides the
    /// own one (tunnel addresses of a KNXnet/IP interface).
    void setAckIndividualAddresses(std::span<const uint16_t> addresses);
#if KNX_FEATURE_COUPLER
    void setGroupAckFilter(const GroupAckFilter* filter);
#endif
    util::Result<std::vector<uint8_t>> receiveFrame(uint32_t timeoutMs);
    util::Result<void> beginReceive(uint32_t timeoutMs);
    util::Result<ProgressState> pollReceive();
    /// The frame the last Complete pollReceive() handed out. The span points
    /// into the receive queue and stays valid until the next beginReceive()
    /// or pollReceive(); call those from the same consumer task only.
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
    void latchTxOutcome(const Tp1RxEvent& event);
    void handleLinkStateEvent(const LinkStatus& status);
    /// Free the slot receivedFrameView() points into, then re-evaluate busy.
    void releaseHeldFrameLocked();
    /// Tell the ISR to answer BUSY while the queue is nearly full.
    void updateLocalBusyLocked();

    static PhysicalLayerState mapState(Tp1MediumState state);
    /// @param decoded Optional out-parameter receiving the decoded frame, so
    ///                callers do not have to decode a second time.
    util::Result<void> validateOutgoingFrame(std::span<const uint8_t> frame,
                                             datalink::LDataFrame* decoded = nullptr) const;
    util::Result<void> waitForTxOutcome(uint32_t initialSequence);

    bool _initialized;
    bool _busMonitorMode;
    std::unique_ptr<Tp1MediumBackend> _backend;
    // TX outcome of the last frame, latched from the backend's TX-ACK events
    // before anything else looks at the event. The sequence advances once
    // per outcome, so a transmitter can tell "no answer yet" from "answered".
    Tp1AckClass _lastTxAckResponse{Tp1AckClass::None};
    bool _lastTxAckDeadlineMiss{false};
    uint32_t _txOutcomeSequence{0};
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
    platform::LocalMutex _rxMutex;
    std::array<BufferedFrame, RX_QUEUE_CAPACITY> _rxFrames;
    size_t _rxHead;
    size_t _rxCount;
    // The slot at _rxHead is lent out through receivedFrameView(); it is
    // still counted in _rxCount so the producer cannot overwrite it.
    bool _rxHeld{false};
    size_t _rxOverflowDrops{0};
    bool _localBusyAsserted{false};
    bool _receiveActive{false};
    uint64_t _receiveDeadlineMs{0};
    size_t _lastReceivedFrameLength{0};
};

} // namespace physical
} // namespace knx