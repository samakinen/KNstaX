// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/platform/virtual_platform.hpp"
#include "../common/virtual_platform_test_utils.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::datalink;
using TaskSleepReason = knx::platform::VirtualPlatform::TaskSleepReason;
using TaskState = knx::platform::VirtualPlatform::TaskState;

namespace {

class ManualQueuePhysicalLayer final {
public:
    using ProgressState = util::OperationProgressState;

    util::Result<void> init() {
        _open = true;
        return util::Result<void>::ok();
    }

    void close() {
        _open = false;
    }

    bool isOpen() const {
        return _open;
    }

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) {
        std::lock_guard<std::mutex> lock(_mutex);
        _sentFrames.emplace(frame.begin(), frame.end());
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

    util::Result<std::vector<uint8_t>> receiveFrame(uint32_t /*timeoutMs*/) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_rxFrames.empty()) {
            return util::ErrorCode::Timeout;
        }
        auto frame = _rxFrames.front();
        _rxFrames.pop();
        return frame;
    }

    util::Result<void> beginReceive(uint32_t /*timeoutMs*/) {
        _rxPending = true;
        return util::Result<void>::ok();
    }

    util::Result<ProgressState> pollReceive() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_rxPending) {
            return util::ErrorCode::OperationNotReady;
        }
        if (_rxFrames.empty()) {
            return util::ErrorCode::Timeout;
        }
        _lastReceived = _rxFrames.front();
        _rxFrames.pop();
        _rxPending = false;
        return ProgressState::Success;
    }

    util::Result<std::span<const uint8_t>> receivedFrameView() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_lastReceived.empty()) {
            return util::ErrorCode::OperationNotReady;
        }
        return std::span<const uint8_t>(_lastReceived);
    }

    void setReceiveCallback(physical::ReceiveCallback callback, void* context) {
        _rxCallback = callback;
        _rxContext = context;
    }

    physical::PhysicalLayerState getState() const {
        return physical::PhysicalLayerState::Idle;
    }

    util::Result<void> setBusMonitorMode(Toggle /*mode*/) {
        return util::Result<void>::ok();
    }

    void queueFrame(std::span<const uint8_t> frame, bool notify) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _rxFrames.push(std::vector<uint8_t>(frame.begin(), frame.end()));
        }
        if (notify && _rxCallback) {
            _rxCallback(_rxContext);
        }
    }

    size_t sentFrameCount() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _sentFrames.size();
    }

private:
    bool _open{false};
    physical::ReceiveCallback _rxCallback{nullptr};
    void* _rxContext{nullptr};
    mutable std::mutex _mutex;
    std::queue<std::vector<uint8_t>> _rxFrames;
    std::queue<std::vector<uint8_t>> _sentFrames;
    std::vector<uint8_t> _lastReceived;
    bool _txPending{false};
    bool _rxPending{false};
};

platform::VirtualTestClock testClock;
std::unique_ptr<platform::VirtualPlatform> platformInstance;
std::unique_ptr<ManualQueuePhysicalLayer> physicalLayer;
std::unique_ptr<Tp1DataLinkLayer> dlLayer;
std::vector<LDataFrame> receivedFrames;
std::mutex receivedMutex;

std::vector<uint8_t> encodeTp1(const LDataFrame& frame) {
    uint8_t buffer[23];
    auto res = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(res.isOk());
    return std::vector<uint8_t>(buffer, buffer + res.value());
}

LDataFrame makeGroupWriteFrame(const GroupAddress& destination, uint8_t payloadByte) {
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = false;
    frame.confirmation = false;
    frame.source = IndividualAddress(1, 1, 20);
    frame.destination = destination;
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;
    frame.setTpdu(protocol::TPCI::UnnumberedData,
                  application::APCIService::GroupValueWrite,
                  {payloadByte});
    return frame;
}

} // namespace

void setUp() {
    testClock.reset();
    platformInstance = std::make_unique<platform::VirtualPlatform>(testClock);
    physicalLayer = std::make_unique<ManualQueuePhysicalLayer>();

    Tp1DataLinkConfig config = Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;

    dlLayer = std::make_unique<Tp1DataLinkLayer>(*platformInstance, *physicalLayer, config);
    receivedFrames.clear();

    dlLayer->setReceiveCallback([](const LDataFrame& frame) {
        std::lock_guard<std::mutex> lock(receivedMutex);
        receivedFrames.push_back(frame);
    });

    TEST_ASSERT_TRUE(dlLayer->init(IndividualAddress(1, 1, 10)).isOk());
}

void tearDown() {
    if (dlLayer) {
        dlLayer->close();
        dlLayer.reset();
    }
    physicalLayer.reset();
    platformInstance.reset();
    receivedFrames.clear();
}

void test_VDLVIRT_001_callback_path_processes_rx_without_background_task() {
    const GroupAddress group(3, 3, 3);
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(group).isOk());

    const auto raw = encodeTp1(makeGroupWriteFrame(group, 0x42));
    physicalLayer->queueFrame(raw, true);

    std::lock_guard<std::mutex> lock(receivedMutex);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(receivedFrames.size()));
    TEST_ASSERT_EQUAL_UINT16(group.raw, receivedFrames[0].destination.raw);
    TEST_ASSERT_EQUAL_UINT8(0x42u, receivedFrames[0].payload()[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, dlLayer->getStatistics().rxFrames);
}

void test_VDLVIRT_002_manual_progression_hook_drains_pending_rx() {
    const GroupAddress group(4, 4, 4);
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(group).isOk());

    const auto raw = encodeTp1(makeGroupWriteFrame(group, 0x55));
    physicalLayer->queueFrame(raw, false);

    const size_t delivered = dlLayer->processRxAvailable(0);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(delivered));

    std::lock_guard<std::mutex> lock(receivedMutex);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(receivedFrames.size()));
    TEST_ASSERT_EQUAL_UINT8(0x55u, receivedFrames[0].payload()[0]);
}

void test_VDLVIRT_003_manual_progression_hook_reports_no_work_cleanly() {
    const size_t delivered = dlLayer->processRxAvailable(0);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(delivered));
    TEST_ASSERT_EQUAL_UINT32(0u, dlLayer->getStatistics().rxFrames);
    TEST_ASSERT_EQUAL_UINT32(0u, physicalLayer->sentFrameCount());
}

void test_VDLVIRT_004_rx_task_lifecycle_is_observable_via_platform_snapshots() {
    if (dlLayer) {
        dlLayer->close();
        dlLayer.reset();
    }

    Tp1DataLinkConfig config = Tp1DataLinkConfig::defaults();
    config.enableRxTask = true;
    dlLayer = std::make_unique<Tp1DataLinkLayer>(*platformInstance, *physicalLayer, config);
    receivedFrames.clear();

    dlLayer->setReceiveCallback([](const LDataFrame& frame) {
        std::lock_guard<std::mutex> lock(receivedMutex);
        receivedFrames.push_back(frame);
    });

    TEST_ASSERT_TRUE(dlLayer->init(IndividualAddress(1, 1, 10)).isOk());

    const auto initialSnapshots = platformInstance->taskSnapshots();
    const auto* rxTask = knx::testsupport::findTaskByName(initialSnapshots, "knx_tp1_rx");
    TEST_ASSERT_NOT_NULL(rxTask);
    TEST_ASSERT_TRUE(rxTask->running);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TaskState::Sleeping), static_cast<uint8_t>(rxTask->state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TaskSleepReason::PredicateWait), static_cast<uint8_t>(rxTask->sleepReason));

    const GroupAddress group(5, 5, 5);
    TEST_ASSERT_TRUE(dlLayer->addGroupAddress(group).isOk());
    const auto raw = encodeTp1(makeGroupWriteFrame(group, 0x66));
    // Notify so the RX task (blocked in taskNotifyTake) wakes; without a
    // notify this physical is only drained on the fallback poll interval.
    physicalLayer->queueFrame(raw, true);

    platformInstance->advanceTimeMs(1u);

    {
        std::lock_guard<std::mutex> lock(receivedMutex);
        TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(receivedFrames.size()));
        TEST_ASSERT_EQUAL_UINT8(0x66u, receivedFrames[0].payload()[0]);
    }

    const auto afterAdvanceSnapshots = platformInstance->taskSnapshots();
    const auto* rxTaskAfterAdvance = knx::testsupport::findTaskByName(afterAdvanceSnapshots, "knx_tp1_rx");
    TEST_ASSERT_NOT_NULL(rxTaskAfterAdvance);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TaskState::Sleeping), static_cast<uint8_t>(rxTaskAfterAdvance->state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TaskSleepReason::PredicateWait), static_cast<uint8_t>(rxTaskAfterAdvance->sleepReason));

    dlLayer->close();
    const auto afterCloseSnapshots = platformInstance->taskSnapshots();
    TEST_ASSERT_NULL(knx::testsupport::findTaskByName(afterCloseSnapshots, "knx_tp1_rx"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_VDLVIRT_001_callback_path_processes_rx_without_background_task);
    RUN_TEST(test_VDLVIRT_002_manual_progression_hook_drains_pending_rx);
    RUN_TEST(test_VDLVIRT_003_manual_progression_hook_reports_no_work_cleanly);
    RUN_TEST(test_VDLVIRT_004_rx_task_lifecycle_is_observable_via_platform_snapshots);
    return UNITY_END();
}
