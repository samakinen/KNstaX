// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

// A TP1 bit-bang driver on the virtual bus: the portable timer-ISR core
// (BitBangDriverTimerIsr) bound to TimerGpioHalVirtualBus, with the message
// pump of BitBangDriverTimerIsrEspIdf in front of it. Everything above it --
// Tp1MacPhysical, the data link layer, a whole stack port -- is the production
// code, so a test built on it exercises the TP1 path down to the bit cells.
// Shared by the data-link conformance test and the Data Secure end-to-end test.

#include "virtual_tp1_test_scheduler.hpp"

#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/bitbang_driver_timer_isr.hpp"
#include "knx/physical/bitbang_driver_tp1_interface.hpp"
#include "knx/physical/timer_gpio_hal_virtual.hpp"
#include "knx/physical/tp1_ack_utils.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace knx {
namespace testsupport {

// The driver is written against the physical layer's vocabulary throughout.
using namespace knx::physical;

class VirtualBitBangTp1Driver : public BitBangDriverInterface,
                                public BitBangDriverTp1Interface,
                                public VirtualTp1SentFrameSource {
public:
    explicit VirtualBitBangTp1Driver(TimerGpioHalVirtualBus& bus)
        : _bus(bus)
    {
    }

    util::Result<void> init(const BitBangConfig& config) override {
        if (_initialized) {
            return util::Result<void>::ok();
        }

        _config = config;
        _hal = {};
        if (!_bus.bind(_hal)) {
            return util::ErrorCode::OperationFailed;
        }
        if (!_core.init(_hal, _config)) {
            return util::ErrorCode::OperationFailed;
        }

        _state = DriverState::Idle;
        _initialized = true;
        _collisionDetected = false;
        _observedDroppedCount = _core.droppedMessageCount();
        _diagnostics = Tp1AckDiagnosticsSnapshot{};
        _activeFrameLength = 0;
        _activeFrameStarted = false;
        _activeFrameErrorFlags = 0;
        _eventHead = 0;
        _eventTail = 0;
        _eventCount = 0;
        _sentFrames.clear();
        return util::Result<void>::ok();
    }

    void close() override {
        if (_initialized) {
            _core.shutdown();
        }
        _initialized = false;
        _state = DriverState::Uninitialized;
        _collisionDetected = false;
        _activeFrameLength = 0;
        _activeFrameStarted = false;
        _activeFrameErrorFlags = 0;
        _eventHead = 0;
        _eventTail = 0;
        _eventCount = 0;
    }

    util::Result<void> send(std::span<const uint8_t> frame) override {
        if (!_initialized) {
            return util::ErrorCode::NotInitialized;
        }
        if (frame.empty()) {
            return util::ErrorCode::InvalidParameter;
        }
        if (_busMonitorMode) {
            return util::ErrorCode::OperationNotSupported;
        }

        _collisionDetected = false;
        setState(DriverState::Transmitting);
        if (!_core.send(frame)) {
            setState(DriverState::Idle);
            return util::ErrorCode::Busy;
        }

        {
            std::lock_guard<std::mutex> lock(_sentFramesMutex);
            _sentFrames.emplace_back(frame.begin(), frame.end());
        }
        return util::Result<void>::ok();
    }

    void setRxCallback(RxCallback callback, void* context) override {
        _rxCallback = callback;
        _rxContext = context;
    }

    void setQueueNotifyCallback(QueueNotifyCallback callback, void* context) override {
        if (_callLog != nullptr) {
            _callLog->push_back(callback == nullptr ? "notify-cleared" : "notify-set");
        }
        // As BitBangDriverTimerIsrEspIdf: the ISR core wakes the data link RX
        // task when it queues a message. Without it an RX task on a
        // VirtualPlatform fibre would only look at the bus on its 10 s poll.
        _core.setQueueNotifyCallback(callback, context);
    }

    /// Order-of-teardown probe for Tp1DataLinkLayer::close().
    void setCallLog(std::vector<std::string>* log) {
        _callLog = log;
    }

    DriverState getState() const override {
        return _state;
    }

    void reset() override {
        if (!_initialized) {
            return;
        }
        _collisionDetected = false;
        setState(DriverState::Idle);
    }

    bool isMediumIdle() const override {
        return _state != DriverState::Transmitting;
    }

    bool isCollisionDetected() const override {
        return _collisionDetected;
    }

    void abortTransmission() override {
        if (_state == DriverState::Transmitting) {
            _collisionDetected = false;
            setState(DriverState::Idle);
        }
    }

    util::Result<void> setOwnAddress(uint16_t addressRaw) override {
        _ownAddressRaw = addressRaw;
        // The ISR core makes the DL-ACK decision autonomously — it needs the
        // own address for its in-frame destination match (mirrors the espidf
        // wrapper's behavior).
        _core.setOwnAddress(addressRaw);
        return util::Result<void>::ok();
    }

    util::Result<void> setBusMonitorMode(bool enabled) override {
        _busMonitorMode = enabled;
        return util::Result<void>::ok();
    }

    void setAckGroupAddresses(std::span<const uint16_t> addresses) override {
        _core.setAckGroupAddresses(addresses.data(),
                                   static_cast<uint8_t>(addresses.size()));
    }

    void setAckIndividualAddresses(std::span<const uint16_t> addresses) override {
        _core.setAckIndividualAddresses(addresses.data(), addresses.size());
    }

    void setLocalBusy(bool busy) override {
        _core.setLocalBusy(busy);
    }

    void pollTp1() override {
        process();
    }

    bool popTp1Event(Tp1RxEvent& outEvent) override {
        if (_eventCount == 0) {
            return false;
        }

        outEvent = _eventQueue[_eventHead];
        _eventHead = (_eventHead + 1u) % TP1_EVENT_QUEUE_CAPACITY;
        --_eventCount;
        return true;
    }

    Tp1AckDiagnosticsSnapshot getTp1AckDiagnostics() const override {
        return _diagnostics;
    }

    const BitBangConfig& config() const {
        return _config;
    }

    void clearSentFrames() {
        std::lock_guard<std::mutex> lock(_sentFramesMutex);
        _sentFrames.clear();
    }

    size_t sentFrameCount() const {
        std::lock_guard<std::mutex> lock(_sentFramesMutex);
        return _sentFrames.size();
    }

    std::vector<uint8_t> sentFrame(size_t index) const {
        std::lock_guard<std::mutex> lock(_sentFramesMutex);
        return _sentFrames[index];
    }

    const BitBangConfig& sentFrameConfig() const override {
        return _config;
    }

protected:
    void process() override {
        if (!_initialized) {
            return;
        }

        BitBangDriverTimerIsr::Message message{};
        while (_core.popMessage(message)) {
            switch (message.type) {
                case BitBangDriverTimerIsr::MessageType::Data: {
                    beginActiveFrameIfNeeded();
                    if (_activeFrameLength < _activeFrame.size()) {
                        _activeFrame[_activeFrameLength++] = message.data;
                    }

                    Tp1RxEvent event;
                    event.type = Tp1RxEventType::DataByte;
                    event.dataByte = message.data;
                    (void)enqueueTp1Event(event);
                    break;
                }

                case BitBangDriverTimerIsr::MessageType::ParityError: {
                    beginActiveFrameIfNeeded();
                    _activeFrameErrorFlags |= 0x01u;
                    Tp1RxEvent event;
                    event.type = Tp1RxEventType::ReceiveError;
                    event.errorFlags = 0x01u;
                    (void)enqueueTp1Event(event);
                    break;
                }

                case BitBangDriverTimerIsr::MessageType::FramingError: {
                    _activeFrameErrorFlags |= 0x02u;
                    Tp1RxEvent event;
                    event.type = Tp1RxEventType::ReceiveError;
                    event.errorFlags = 0x02u;
                    (void)enqueueTp1Event(event);
                    break;
                }

                case BitBangDriverTimerIsr::MessageType::End: {
                    if (_state != DriverState::Transmitting) {
                        setState(DriverState::Idle);
                    }

                    // Mirrors BitBangDriverTimerIsrEspIdf: the BUSY marker is
                    // not an error, and a frame refused with BUSY is not
                    // delivered (the sender repeats it).
                    const uint8_t errorFlags = static_cast<uint8_t>(
                        message.data & ~BitBangDriverTimerIsr::END_FLAG_ANSWERED_BUSY);
                    const bool answeredBusy =
                        (message.data & BitBangDriverTimerIsr::END_FLAG_ANSWERED_BUSY) != 0u;

                    if (errorFlags != 0u) {
                        Tp1RxEvent errorEvent;
                        errorEvent.type = Tp1RxEventType::ReceiveError;
                        errorEvent.errorFlags = errorFlags;
                        (void)enqueueTp1Event(errorEvent);
                    }

                    Tp1RxEvent endEvent;
                    endEvent.type = Tp1RxEventType::TelegramEnd;
                    (void)enqueueTp1Event(endEvent);

                    if (_rxCallback && _activeFrameStarted && _activeFrameLength > 0u && !answeredBusy) {
                        _rxCallback(std::span<const uint8_t>(_activeFrame).first(_activeFrameLength), _rxContext);
                    }

                    _activeFrameStarted = false;
                    _activeFrameLength = 0;
                    _activeFrameErrorFlags = 0;
                    break;
                }

                case BitBangDriverTimerIsr::MessageType::TxAckResponse: {
                    if (message.data == BitBangDriverTimerIsr::ACK_BYTE_NONE) {
                        Tp1RxEvent event;
                        event.type = Tp1RxEventType::TxAckDeadlineMiss;
                        ++_diagnostics.deadlineMissCount;
                        (void)enqueueTp1Event(event);
                    } else {
                        Tp1RxEvent event;
                        event.type = Tp1RxEventType::TxAckResponse;
                        event.ackClass = tp1AckClassFromByte(message.data);
                        ++_diagnostics.responseEmittedCount;
                        noteUnsupportedAckByte(message.data);
                        (void)enqueueTp1Event(event);
                    }
                    setState(DriverState::Idle);
                    break;
                }

                case BitBangDriverTimerIsr::MessageType::TxComplete: {
                    // Frames sent without the CTRL A-bit finish via TxComplete
                    // (no DL-ACK wait) — return to idle like the espidf wrapper.
                    setState(DriverState::Idle);
                    break;
                }

                case BitBangDriverTimerIsr::MessageType::TxArbitrationLost: {
                    Tp1RxEvent event;
                    event.type = Tp1RxEventType::TxAckResponse;
                    event.ackClass = Tp1AckClass::None;
                    (void)enqueueTp1Event(event);
                    setState(DriverState::Idle);
                    break;
                }

                case BitBangDriverTimerIsr::MessageType::Collision:
                    _collisionDetected = true;
                    (void)enqueueMediumStateChanged();
                    break;

                default:
                    break;
            }
        }

        const uint32_t droppedNow = _core.droppedMessageCount();
        if (droppedNow >= _observedDroppedCount) {
            _diagnostics.overflowErrorCount += (droppedNow - _observedDroppedCount);
        }
        _observedDroppedCount = droppedNow;
    }

    const char* getVersion() const override {
        return "knstax-timer-isr-virtual-test";
    }

private:
    static constexpr size_t TP1_EVENT_QUEUE_CAPACITY = 128u;

    bool enqueueTp1Event(const Tp1RxEvent& event) {
        if (_eventCount >= TP1_EVENT_QUEUE_CAPACITY) {
            ++_diagnostics.overflowErrorCount;
            return false;
        }

        _eventQueue[_eventTail] = event;
        _eventTail = (_eventTail + 1u) % TP1_EVENT_QUEUE_CAPACITY;
        ++_eventCount;
        return true;
    }

    void setState(DriverState newState) {
        if (_state == newState) {
            return;
        }

        _state = newState;
        (void)enqueueMediumStateChanged();
    }

    bool enqueueMediumStateChanged() {
        Tp1RxEvent event;
        event.type = Tp1RxEventType::MediumStateChanged;
        return enqueueTp1Event(event);
    }

    void noteUnsupportedAckByte(uint8_t ackByte) {
        if (!isTp1AckByte(ackByte)) {
            ++_diagnostics.unsupportedRawIngressCount;
        }
    }

    // Mirrors BitBangDriverTimerIsrEspIdf: the ISR no longer emits a per-
    // character Start message, so a receive-frame slot opens on the first
    // message that belongs to a telegram.
    void beginActiveFrameIfNeeded() {
        if (_activeFrameStarted) {
            return;
        }

        _activeFrameStarted = true;
        _activeFrameLength = 0;
        _activeFrameErrorFlags = 0;

        if (_state != DriverState::Transmitting) {
            setState(DriverState::Receiving);
        }

        Tp1RxEvent event;
        event.type = Tp1RxEventType::TelegramStart;
        (void)enqueueTp1Event(event);
    }

    TimerGpioHalVirtualBus& _bus;
    DriverState _state{DriverState::Uninitialized};
    bool _initialized{false};
    bool _busMonitorMode{false};
    uint16_t _ownAddressRaw{0};
    RxCallback _rxCallback{};
    void* _rxContext{nullptr};

    BitBangConfig _config{};
    BitBangDriverTimerIsr _core;
    knx_timer_gpio_hal_t _hal{};
    bool _collisionDetected{false};
    uint32_t _observedDroppedCount{0};
    Tp1AckDiagnosticsSnapshot _diagnostics{};

    // Mirrors BitBangDriverTimerIsrEspIdf: a full L_Data_Extended telegram.
    std::array<uint8_t, 263> _activeFrame{};
    size_t _activeFrameLength{0};
    bool _activeFrameStarted{false};
    uint8_t _activeFrameErrorFlags{0};

    std::array<Tp1RxEvent, TP1_EVENT_QUEUE_CAPACITY> _eventQueue{};
    size_t _eventHead{0};
    size_t _eventTail{0};
    size_t _eventCount{0};

    mutable std::mutex _sentFramesMutex;
    std::vector<std::vector<uint8_t>> _sentFrames;
    std::vector<std::string>* _callLog{nullptr};
};

} // namespace testsupport
} // namespace knx
