// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_physical_factory_tp1.cpp
 * @brief Unit tests for TP1 physical factory composition paths
 */

#include "unity.h"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/physical/physical_factory.hpp"
#include "knx/protocol/tpdu_codec.hpp"

#include <optional>
#include <span>
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
        return util::Result<void>::ok();
    }

    void close() {
        _open = false;
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
        _txPending = true;
        return 1u;
    }

    util::Result<ProgressState> pollTransmit(uint32_t) {
        if (!_txPending) {
            return util::ErrorCode::OperationNotReady;
        }
        _txPending = false;
        return ProgressState::Success;
    }

    util::Result<std::vector<uint8_t>> receiveFrame(uint32_t timeoutMs) {
        _lastReceiveTimeoutMs = timeoutMs;

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
        _rxPending = true;
        return util::Result<void>::ok();
    }

    util::Result<ProgressState> pollReceive() {
        if (!_rxPending) {
            return util::ErrorCode::OperationNotReady;
        }
        if (_receiveError.has_value()) {
            _rxPending = false;
            return _receiveError.value();
        }
        if (!_hasQueuedFrame) {
            return ProgressState::Pending;
        }
        _rxPending = false;
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
        if (_receiveError.has_value()) {
            return _receiveError.value();
        }
        if (_hasQueuedFrame) {
            return std::span<const uint8_t>(_queuedFrame);
        }
        return util::ErrorCode::Timeout;
    }

    void setReceiveCallback(ReceiveCallback, void*) {
    }

    PhysicalLayerState getState() const {
        return _state;
    }

    util::Result<void> setBusMonitorMode(Toggle mode) {
        _lastMonitorMode = mode;
        ++_monitorCalls;
        return util::Result<void>::ok();
    }

    void queueFrame(std::span<const uint8_t> frame) {
        _queuedFrame.assign(frame.begin(), frame.end());
        _hasQueuedFrame = true;
    }

    int monitorCalls() const {
        return _monitorCalls;
    }

    Toggle lastMonitorMode() const {
        return _lastMonitorMode;
    }

    uint32_t lastReceiveTimeoutMs() const {
        return _lastReceiveTimeoutMs;
    }

private:
    bool _open{false};
    PhysicalLayerState _state{PhysicalLayerState::Idle};
    std::optional<util::ErrorCode> _sendError{};
    std::optional<util::ErrorCode> _receiveError{};
    std::vector<uint8_t> _lastSent{};
    std::vector<uint8_t> _queuedFrame{};
    bool _hasQueuedFrame{false};
    Toggle _lastMonitorMode{Toggle::Disable};
    int _monitorCalls{0};
    uint32_t _lastReceiveTimeoutMs{0};
    bool _txPending{false};
    bool _rxPending{false};
};

std::vector<uint8_t> buildValidFrame() {
    datalink::LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = true;
    frame.confirmation = false;
    frame.source = IndividualAddress(1, 1, 1);
    frame.destination = GroupAddress(2, 2, 4);
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;
    frame.setTpdu(protocol::TPCI::UnnumberedData,
                  application::APCIService::GroupValueWrite,
                  {0x01u});

    uint8_t buffer[23]{};
    auto encoded = datalink::FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(encoded.isOk());
    return std::vector<uint8_t>(buffer, buffer + encoded.value());
}

} // namespace

void setUp(void) {
}

void tearDown(void) {
}

void test_create_tp1_tpuart_physical_wraps_mac_composed_behavior() {
    FakeTp1PhysicalLayer lowLevel;
    auto physical = createBorrowedTp1TpuartPhysical(lowLevel);

    TEST_ASSERT_NOT_NULL(physical.get());
    TEST_ASSERT_TRUE(physical->setBusMonitorMode(Toggle::Enable).isOk());
    TEST_ASSERT_TRUE(physical->init().isOk());

    auto frame = buildValidFrame();

    TEST_ASSERT_TRUE(physical->sendFrame(frame).isError());

    TEST_ASSERT_TRUE(physical->setBusMonitorMode(Toggle::Disable).isOk());
    TEST_ASSERT_EQUAL(2, lowLevel.monitorCalls());
    TEST_ASSERT_EQUAL(static_cast<int>(Toggle::Disable), static_cast<int>(lowLevel.lastMonitorMode()));

    TEST_ASSERT_TRUE(physical->sendFrame(frame).isOk());

    lowLevel.queueFrame(frame);
    auto rx = physical->receiveFrame(0);
    TEST_ASSERT_TRUE(rx.isOk());
    TEST_ASSERT_EQUAL(frame.size(), rx.value().size());
    TEST_ASSERT_EQUAL(0, static_cast<int>(lowLevel.lastReceiveTimeoutMs()));

    physical->close();
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_create_tp1_tpuart_physical_wraps_mac_composed_behavior);

    return UNITY_END();
}
