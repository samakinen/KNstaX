// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bitbang_driver_timer_isr_espidf.hpp
 * @brief ESP-IDF bitbang driver using BitBangDriverTimerIsr + timer/gpio HAL
 */

#pragma once

#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/bitbang_driver_timer_isr.hpp"
#include "knx/physical/bitbang_driver_tp1_interface.hpp"
#include "knx/physical/link_monitor.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>

namespace knx {
namespace physical {

class BitBangDriverTimerIsrEspIdf : public BitBangDriverInterface, public BitBangDriverTp1Interface {
public:
    BitBangDriverTimerIsrEspIdf() = default;
    ~BitBangDriverTimerIsrEspIdf() override;

    util::Result<void> init(const BitBangConfig& config) override;
    void close() override;
    util::Result<void> send(std::span<const uint8_t> frame) override;
    void setRxCallback(RxCallback callback, void* context) override;
    void setQueueNotifyCallback(QueueNotifyCallback callback, void* context) override;
    DriverState getState() const override;
    void reset() override;
    bool isMediumIdle() const override;
    bool isCollisionDetected() const override;
    void abortTransmission() override;

    util::Result<void> setOwnAddress(uint16_t addressRaw) override;
    util::Result<void> setBusMonitorMode(bool enabled) override;
    void setAckGroupAddresses(std::span<const uint16_t> addresses) override;
    void setLocalBusy(bool busy) override;
    void pollTp1() override;
    bool popTp1Event(Tp1RxEvent& outEvent) override;
    Tp1AckDiagnosticsSnapshot getTp1AckDiagnostics() const override;
    bool hasTp1LinkStateIndication() const override;
    LinkState getTp1LinkState() const override;

    /**
     * @brief Register the pre-debounce power-fail handler
     *
     * Invoked from the link signal's edge interrupt the moment the transceiver
     * reports the bus supply gone, before any debouncing, when the driver was
     * configured with LinkSignalConfig::notifyPowerFailFromIsr.
     *
     * The handler runs in interrupt context: it must be IRAM-resident and must
     * not log, block or touch flash. Boards with seconds of hold-up have no
     * need for it and should react to the debounced LinkStateChanged event
     * from task context instead.
     */
    void setPowerFailIsrHandler(LinkPowerFailIsrHandler handler, void* context);

protected:
    void process() override;
    const char* getVersion() const override;

private:
    static constexpr size_t TP1_EVENT_QUEUE_CAPACITY = 128;

    void setState(DriverState newState);
    bool enqueueTp1Event(const Tp1RxEvent& event);
    bool enqueueMediumStateChanged();
    bool initLinkSignal();
    void closeLinkSignal();
    void pollLinkSignal();
    void scheduleLinkRepoll(uint64_t remainingUs);
    static void linkRepollTimerCb(void* arg);
    static void KNX_TIMER_GPIO_HAL_ISR_ATTR linkSignalEdgeIsr(void* context);
    void noteUnsupportedAckByte(uint8_t ackByte);
    void noteRxPartialError(uint8_t errorFlag, const char* label);
    void flushRxPartialErrorSummary();
    /// Open a receive-frame assembly slot on the first message that belongs to
    /// a telegram. The ISR no longer emits a per-character Start message — it
    /// cost a queue slot and a task wakeup for every octet — so the frame
    /// boundary is derived here from the first byte after the previous End.
    void beginActiveFrameIfNeeded();
    /// Recessive gap (µs) between the previous logged bus event and `startUs`,
    /// or -1 when no previous event is known. Unsigned wrap-around is the
    /// correct arithmetic: the timestamps are 32-bit truncations of a
    /// free-running µs timer, and every gap of interest is far below 2^32 µs.
    int64_t busIdleBefore(uint32_t startUs) const;

    DriverState _state{DriverState::Uninitialized};
    bool _initialized{false};
    bool _busMonitorMode{false};
    uint16_t _ownAddressRaw{0};
    RxCallback _rxCallback{nullptr};
    void* _rxContext{nullptr};

    BitBangConfig _config{};
    BitBangDriverTimerIsr _core{};
    BitBangDriverTimerIsr::QueueNotifyCallback _queueNotifyCallback{nullptr};
    void* _queueNotifyContext{nullptr};
    knx_timer_gpio_hal_t _hal{};
    bool _halCreated{false};
    bool _collisionDetected{false};
    uint32_t _collisionEventCount{0};
    uint32_t _observedDroppedCount{0};

    Tp1AckDiagnosticsSnapshot _diagnostics{};

    // Link health. The edge ISR only wakes the consumer (and optionally fires
    // the power-fail handler); the state itself is derived from levels sampled
    // in process(), which cannot desynchronise from a missed or bouncing edge.
    LinkMonitor _linkMonitor{};
    bool _linkSignalActive{false};
    std::atomic<LinkState> _linkState{LinkState::Unknown};
    LinkPowerFailIsrHandler _powerFailHandler{nullptr};
    void* _powerFailContext{nullptr};
    // esp_timer_handle_t, type-erased to keep ESP-IDF headers out of this one.
    // A debounce decision needs one sample after the window elapses; the RX
    // worker's own wake cadence is far longer than any sane window, so the
    // decision is scheduled rather than left to the next scheduled poll.
    void* _linkRepollTimer{nullptr};

    // Must hold a full L_Data_Extended telegram (8-byte header + 254-byte APDU
    // + FCS). At 256 the last seven octets of a maximum-length frame were
    // dropped on the floor and the frame handed upwards as if complete — the
    // receive-side twin of the uint8_t transmit length that truncated at 255.
    static constexpr size_t MAX_RX_TELEGRAM_SIZE = 263;
    std::array<uint8_t, MAX_RX_TELEGRAM_SIZE> _activeFrame{};
    // Octets discarded because a telegram exceeded the buffer. Non-zero means a
    // frame reached the upper layers truncated.
    uint32_t _activeFrameOverflowCount{0};
    size_t _activeFrameLength{0};
    bool _activeFrameStarted{false};
    uint8_t _activeFrameErrorFlags{0};

    std::array<uint8_t, 16> _lastRxErrorPartial{};
    size_t _lastRxErrorPartialLength{0};
    uint8_t _lastRxErrorFlag{0};
    uint32_t _lastRxErrorRepeatCount{0};

    // End timestamp (µs, free-running timer, 32-bit truncated to match the
    // per-message timestamps) of the most recent bus event, for logging the
    // idle gap before the next one.
    uint32_t _lastBusEndUs{0};

#ifdef CONFIG_KNX_TP1_BITBANG_TIMING_STATS
    // Bus-monitor: retain the last data frame handed to send() so the BUS TX
    // line can print its content alongside the timestamps the ISR attached to
    // the TxFrameEnd message.
    std::array<uint8_t, 264> _lastTxFrame{};
    size_t _lastTxFrameLen{0};
    // Byte from the DlAckTxStart message, consumed by the DlAckDone that
    // immediately follows it in the same ring.
    uint8_t _pendingDlAckByte{0};
#endif

    std::array<Tp1RxEvent, TP1_EVENT_QUEUE_CAPACITY> _eventQueue{};
    size_t _eventHead{0};
    size_t _eventTail{0};
    size_t _eventCount{0};

    // Serializes every task-context consumer of the ISR core: process() and
    // popTp1Event() are reached both from the datalink RX worker (pollReceive →
    // onTick → service) and from whichever task drives transmission (send /
    // pollTransmit) — two tasks draining the same message ring and mutating
    // _activeFrame/_eventQueue concurrently. Recursive because process()'s RX
    // callback chain re-enters popTp1Event() on the same thread.
    mutable std::recursive_mutex _serviceMutex{};
};

} // namespace physical
} // namespace knx
