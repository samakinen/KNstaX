// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tpuart_medium_backend_adapter.cpp
 * @brief Unit tests for TpuartMediumBackendAdapter
 */

#include "unity.h"

#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/physical/tp1_physical_layer.hpp"
#include "knx/physical/tpuart_medium_backend_adapter.hpp"

#include <memory>
#include <optional>
#include <vector>

using namespace knx;
using namespace knx::physical;

namespace {

class FakeTp1PhysicalLayer final {
public:
    using ProgressState = util::OperationProgressState;

    util::Result<void> init() {
        _open = true;
        _state = PhysicalLayerState::Idle;
        ++_initCalls;
        return util::Result<void>::ok();
    }

    void close() {
        _open = false;
        ++_closeCalls;
    }

    bool isOpen() const {
        return _open;
    }

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) {
        if (_sendError.has_value()) {
            return _sendError.value();
        }
        _lastSent.assign(frame.begin(), frame.end());
        return frame.size();
    }

    util::Result<uint32_t> beginTransmit(std::span<const uint8_t> frame) {
        auto sendResult = sendFrame(frame);
        if (sendResult.isError()) {
            return sendResult.error();
        }
        _progressTransmitPending = true;
        return 1u;
    }

    util::Result<ProgressState> pollTransmit(uint32_t) {
        if (_progressTransmitPending) {
            _progressTransmitPending = false;
            return ProgressState::Success;
        }
        return util::ErrorCode::OperationNotReady;
    }

    util::Result<std::vector<uint8_t>> receiveFrame(uint32_t timeoutMs) {
        _lastReceiveTimeoutMs = timeoutMs;
        ++_receiveFrameCalls;

        if (_receiveError.has_value()) {
            return _receiveError.value();
        }

        if (_hasQueuedFrame) {
            _hasQueuedFrame = false;
            return _queuedFrame;
        }

        return util::ErrorCode::Timeout;
    }

    util::Result<void> beginReceive(uint32_t timeoutMs) {
        _lastReceiveTimeoutMs = timeoutMs;
        _receivePending = true;
        return util::Result<void>::ok();
    }

    util::Result<ProgressState> pollReceive() {
        if (!_receivePending) {
            return util::ErrorCode::OperationNotReady;
        }
        if (_receiveError.has_value()) {
            _receivePending = false;
            return _receiveError.value();
        }
        if (!_hasQueuedFrame) {
            return ProgressState::Pending;
        }

        _receivePending = false;
        return ProgressState::Success;
    }

    util::Result<std::span<const uint8_t>> receivedFrameView() {
        if (!_hasQueuedFrame) {
            return util::ErrorCode::OperationNotReady;
        }
        return std::span<const uint8_t>(_queuedFrame);
    }

    util::Result<std::span<const uint8_t>> receiveFrameView(uint32_t timeoutMs) {
        _lastReceiveTimeoutMs = timeoutMs;
        ++_receiveFrameViewCalls;

        if (!_supportFrameView) {
            return util::ErrorCode::OperationNotSupported;
        }

        if (_receiveError.has_value()) {
            return _receiveError.value();
        }

        if (_hasQueuedFrame) {
            _hasQueuedFrame = false;
            return std::span<const uint8_t>(_queuedFrame);
        }

        return util::ErrorCode::Timeout;
    }

    void setReceiveCallback(ReceiveCallback callback, void* context) {
        _callback = callback;
        _callbackContext = context;
    }

    PhysicalLayerState getState() const {
        return _state;
    }

    util::Result<void> setBusMonitorMode(Toggle mode) {
        _lastBusMonitorMode = mode;
        ++_setBusMonitorCalls;
        return util::Result<void>::ok();
    }

    void setState(PhysicalLayerState state) {
        _state = state;
    }

    void setNextSendError(util::ErrorCode error) {
        _sendError = error;
    }

    void clearNextSendError() {
        _sendError.reset();
    }

    void setNextReceiveError(util::ErrorCode error) {
        _receiveError = error;
    }

    void clearNextReceiveError() {
        _receiveError.reset();
    }

    void queueReceiveFrame(std::span<const uint8_t> frame) {
        _queuedFrame.assign(frame.begin(), frame.end());
        _hasQueuedFrame = true;
    }

    void setSupportFrameView(bool enabled) {
        _supportFrameView = enabled;
    }

    Toggle lastBusMonitorMode() const {
        return _lastBusMonitorMode;
    }

    int setBusMonitorCalls() const {
        return _setBusMonitorCalls;
    }

    std::span<const uint8_t> lastSent() const {
        return std::span<const uint8_t>(_lastSent);
    }

    int initCalls() const {
        return _initCalls;
    }

    int closeCalls() const {
        return _closeCalls;
    }

    uint32_t lastReceiveTimeoutMs() const {
        return _lastReceiveTimeoutMs;
    }

    int receiveFrameCalls() const {
        return _receiveFrameCalls;
    }

    int receiveFrameViewCalls() const {
        return _receiveFrameViewCalls;
    }

private:
    bool _open{false};
    PhysicalLayerState _state{PhysicalLayerState::Idle};
    ReceiveCallback _callback{};
    void* _callbackContext{nullptr};

    std::optional<util::ErrorCode> _sendError{};
    std::optional<util::ErrorCode> _receiveError{};

    std::vector<uint8_t> _lastSent{};
    std::vector<uint8_t> _queuedFrame{};
    bool _hasQueuedFrame{false};
    bool _supportFrameView{true};

    Toggle _lastBusMonitorMode{Toggle::Disable};
    int _setBusMonitorCalls{0};
    int _initCalls{0};
    int _closeCalls{0};
    uint32_t _lastReceiveTimeoutMs{0};
    int _receiveFrameCalls{0};
    int _receiveFrameViewCalls{0};
    bool _progressTransmitPending{false};
    bool _receivePending{false};
};

struct EventCapture {
    int count{0};
    Tp1RxEventType lastType{Tp1RxEventType::MediumStateChanged};
    Tp1AckClass lastAckClass{Tp1AckClass::None};
    std::vector<uint8_t> lastFrame{};
};

FakeTp1PhysicalLayer fakePhysical;
std::unique_ptr<TpuartMediumBackendAdapter> adapter;
EventCapture capture;

void onEvent(const Tp1RxEvent& event, void* context) {
    auto* state = static_cast<EventCapture*>(context);
    if (!state) {
        return;
    }

    ++state->count;
    state->lastType = event.type;
    state->lastAckClass = event.ackClass;
    if (event.frame.empty()) {
        state->lastFrame.clear();
    } else {
        state->lastFrame.assign(event.frame.begin(), event.frame.end());
    }
}

Tp1MediumConfig defaultConfig() {
    Tp1MediumConfig config;
    config.ownIndividualAddressRaw = 0x1234;
    config.busMonitorMode = false;
    return config;
}

} // namespace

void setUp(void) {
    fakePhysical = FakeTp1PhysicalLayer{};
    adapter = std::make_unique<TpuartMediumBackendAdapter>(fakePhysical);
    capture = EventCapture{};
}

void tearDown(void) {
    if (adapter) {
        adapter->close();
        adapter.reset();
    }
}

void test_init_applies_monitor_mode_and_reports_idle_state() {
    auto config = defaultConfig();
    config.busMonitorMode = true;

    TEST_ASSERT_TRUE(adapter->init(config).isOk());
    TEST_ASSERT_EQUAL(1, fakePhysical.initCalls());
    TEST_ASSERT_EQUAL(1, fakePhysical.setBusMonitorCalls());
    TEST_ASSERT_EQUAL(static_cast<int>(Toggle::Enable), static_cast<int>(fakePhysical.lastBusMonitorMode()));

    TEST_ASSERT_EQUAL(static_cast<int>(Tp1MediumState::Idle), static_cast<int>(adapter->getState()));
}

void test_send_frame_emits_ack_event_on_success() {
    TEST_ASSERT_TRUE(adapter->init(defaultConfig()).isOk());
    adapter->setEventCallback(onEvent, &capture);

    const uint8_t frame[] = {0x11, 0x22, 0x33};
    TEST_ASSERT_TRUE(adapter->sendFrame(frame).isOk());

    TEST_ASSERT_EQUAL(1, capture.count);
    TEST_ASSERT_EQUAL(static_cast<int>(Tp1RxEventType::TxAckResponse), static_cast<int>(capture.lastType));
    TEST_ASSERT_EQUAL(static_cast<int>(Tp1AckClass::Ack), static_cast<int>(capture.lastAckClass));
    TEST_ASSERT_EQUAL(3, static_cast<int>(fakePhysical.lastSent().size()));
}

void test_send_frame_emits_busy_ack_on_busy_error() {
    TEST_ASSERT_TRUE(adapter->init(defaultConfig()).isOk());
    adapter->setEventCallback(onEvent, &capture);

    fakePhysical.setNextSendError(util::ErrorCode::Busy);
    const uint8_t frame[] = {0x44};
    auto result = adapter->sendFrame(frame);

    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Busy), static_cast<int>(result.error()));
    TEST_ASSERT_EQUAL(1, capture.count);
    TEST_ASSERT_EQUAL(static_cast<int>(Tp1AckClass::Busy), static_cast<int>(capture.lastAckClass));
}

void test_service_emits_telegram_end_with_frame_payload() {
    TEST_ASSERT_TRUE(adapter->init(defaultConfig()).isOk());
    adapter->setEventCallback(onEvent, &capture);

    fakePhysical.queueReceiveFrame(std::vector<uint8_t>{0xAA, 0xBB});
    TEST_ASSERT_TRUE(adapter->service().isOk());

    TEST_ASSERT_EQUAL(1, capture.count);
    TEST_ASSERT_EQUAL(static_cast<int>(Tp1RxEventType::TelegramEnd), static_cast<int>(capture.lastType));
    TEST_ASSERT_EQUAL(2, static_cast<int>(capture.lastFrame.size()));
    TEST_ASSERT_EQUAL_HEX8(0xAA, capture.lastFrame[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, capture.lastFrame[1]);
    TEST_ASSERT_EQUAL(0, static_cast<int>(fakePhysical.lastReceiveTimeoutMs()));
    TEST_ASSERT_EQUAL(1, fakePhysical.receiveFrameViewCalls());
}

void test_service_uses_bound_physical_view_when_no_frame_source_is_present() {
    adapter = std::make_unique<TpuartMediumBackendAdapter>(fakePhysical);
    TEST_ASSERT_TRUE(adapter->init(defaultConfig()).isOk());
    adapter->setEventCallback(onEvent, &capture);

    fakePhysical.queueReceiveFrame(std::vector<uint8_t>{0x10, 0x20, 0x30});
    TEST_ASSERT_TRUE(adapter->service().isOk());

    TEST_ASSERT_EQUAL(1, capture.count);
    TEST_ASSERT_EQUAL(1, fakePhysical.receiveFrameViewCalls());
    TEST_ASSERT_EQUAL(3, static_cast<int>(capture.lastFrame.size()));
}

void test_service_timeout_returns_ok_and_no_event() {
    TEST_ASSERT_TRUE(adapter->init(defaultConfig()).isOk());
    adapter->setEventCallback(onEvent, &capture);

    TEST_ASSERT_TRUE(adapter->service().isOk());
    TEST_ASSERT_EQUAL(0, capture.count);
}

void test_service_propagates_receive_error() {
    TEST_ASSERT_TRUE(adapter->init(defaultConfig()).isOk());
    fakePhysical.setNextReceiveError(util::ErrorCode::OperationFailed);

    auto result = adapter->service();
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::OperationFailed), static_cast<int>(result.error()));
}

void test_control_methods_and_capabilities_match_tpuart_profile() {
    TEST_ASSERT_TRUE(adapter->setBusMonitorMode(true).isOk());
    TEST_ASSERT_EQUAL(0, fakePhysical.setBusMonitorCalls());

    TEST_ASSERT_TRUE(adapter->init(defaultConfig()).isOk());
    TEST_ASSERT_TRUE(adapter->setBusMonitorMode(false).isOk());
    TEST_ASSERT_EQUAL(2, fakePhysical.setBusMonitorCalls());
    TEST_ASSERT_EQUAL(static_cast<int>(Toggle::Disable), static_cast<int>(fakePhysical.lastBusMonitorMode()));

    TEST_ASSERT_NULL(adapter->diagnostics());

    const auto caps = adapter->getCapabilities();
    TEST_ASSERT_TRUE(caps.supportsHardwareAutoAck);
    TEST_ASSERT_TRUE(caps.supportsAddressedFiltering);
    TEST_ASSERT_FALSE(caps.supportsByteEventStream);
    TEST_ASSERT_TRUE(caps.supportsDetailedTxConfirm);
    TEST_ASSERT_FALSE(caps.supportsCollisionIndication);
    TEST_ASSERT_FALSE(caps.supportsDiagnosticsSnapshot);
}

void test_not_initialized_guards_send_and_service() {
    const uint8_t frame[] = {0x01};

    auto sendResult = adapter->sendFrame(frame);
    TEST_ASSERT_TRUE(sendResult.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::NotInitialized), static_cast<int>(sendResult.error()));

    auto serviceResult = adapter->service();
    TEST_ASSERT_TRUE(serviceResult.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::NotInitialized), static_cast<int>(serviceResult.error()));

    TEST_ASSERT_EQUAL(static_cast<int>(Tp1MediumState::Uninitialized), static_cast<int>(adapter->getState()));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_init_applies_monitor_mode_and_reports_idle_state);
    RUN_TEST(test_send_frame_emits_ack_event_on_success);
    RUN_TEST(test_send_frame_emits_busy_ack_on_busy_error);
    RUN_TEST(test_service_emits_telegram_end_with_frame_payload);
    RUN_TEST(test_service_uses_bound_physical_view_when_no_frame_source_is_present);
    RUN_TEST(test_service_timeout_returns_ok_and_no_event);
    RUN_TEST(test_service_propagates_receive_error);
    RUN_TEST(test_control_methods_and_capabilities_match_tpuart_profile);
    RUN_TEST(test_not_initialized_guards_send_and_service);

    return UNITY_END();
}
