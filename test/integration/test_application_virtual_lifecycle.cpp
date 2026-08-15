// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/application/application_layer.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/network/network_layer.hpp"
#include "knx/platform/virtual_platform.hpp"
#include "knx/transport/transport_layer.hpp"
#include "unity.h"

#include "../common/virtual_platform_test_utils.hpp"

#include <memory>
#include <mutex>
#include <queue>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::application;

namespace {

class ManualQueuePhysicalLayer final {
public:
    using ProgressState = util::OperationProgressState;

    util::Result<void> init()
    {
        open_ = true;
        return util::Result<void>::ok();
    }

    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sentFrames_.emplace(frame.begin(), frame.end());
        return frame.size();
    }

    util::Result<uint32_t> beginTransmit(std::span<const uint8_t> frame)
    {
        auto sendResult = sendFrame(frame);
        if (sendResult.isError()) {
            return sendResult.error();
        }
        txPending_ = true;
        return 1u;
    }

    util::Result<ProgressState> pollTransmit(uint32_t)
    {
        if (!txPending_) {
            return util::ErrorCode::OperationNotReady;
        }
        txPending_ = false;
        return ProgressState::Success;
    }

    util::Result<void> beginReceive(uint32_t)
    {
        rxPending_ = true;
        return util::Result<void>::ok();
    }

    util::Result<ProgressState> pollReceive()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!rxPending_) {
            return util::ErrorCode::OperationNotReady;
        }
        if (rxFrames_.empty()) {
            return util::ErrorCode::Timeout;
        }

        lastReceived_ = std::move(rxFrames_.front());
        rxFrames_.pop();
        rxPending_ = false;
        return ProgressState::Success;
    }

    util::Result<std::span<const uint8_t>> receivedFrameView()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lastReceived_.empty()) {
            return util::ErrorCode::OperationNotReady;
        }
        return std::span<const uint8_t>(lastReceived_);
    }

    void setReceiveCallback(physical::ReceiveCallback callback, void* context)
    {
        rxCallback_ = callback;
        rxContext_ = context;
    }

    physical::PhysicalLayerState getState() const
    {
        return physical::PhysicalLayerState::Idle;
    }

    util::Result<void> setBusMonitorMode(Toggle)
    {
        return util::Result<void>::ok();
    }

    void queueFrame(std::span<const uint8_t> frame)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            rxFrames_.push(std::vector<uint8_t>(frame.begin(), frame.end()));
        }
        // Wake the data link RX task (blocked in taskNotifyTake); real
        // backends signal frame arrival the same way.
        if (rxCallback_) {
            rxCallback_(rxContext_);
        }
    }

private:
    bool open_{false};
    mutable std::mutex mutex_;
    std::queue<std::vector<uint8_t>> rxFrames_;
    std::queue<std::vector<uint8_t>> sentFrames_;
    std::vector<uint8_t> lastReceived_;
    physical::ReceiveCallback rxCallback_{nullptr};
    void* rxContext_{nullptr};
    bool txPending_{false};
    bool rxPending_{false};
};

platform::VirtualTestClock testClock;
std::unique_ptr<platform::VirtualPlatform> platformInstance;
std::unique_ptr<ManualQueuePhysicalLayer> physicalLayer;
std::unique_ptr<datalink::Tp1DataLinkLayer> dlLayer;
std::unique_ptr<network::NetworkLayer> nwLayer;
std::unique_ptr<transport::TransportLayer> tpLayer;
std::unique_ptr<ApplicationLayer> appLayer;

constexpr IndividualAddress kOwnAddress(1, 1, 1);

std::vector<uint8_t> encodeTp1(const datalink::LDataFrame& frame)
{
    uint8_t buffer[23]{};
    const auto enc = datalink::FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(enc.isOk());
    return std::vector<uint8_t>(buffer, buffer + enc.value());
}

void localSetUp()
{
    testClock.reset();
    platformInstance = std::make_unique<platform::VirtualPlatform>(testClock);
    physicalLayer = std::make_unique<ManualQueuePhysicalLayer>();

    auto config = datalink::Tp1DataLinkConfig::defaults();
    config.enableRxTask = true;

    dlLayer = std::make_unique<datalink::Tp1DataLinkLayer>(*platformInstance, *physicalLayer, config);
    nwLayer = std::make_unique<network::NetworkLayer>(*dlLayer);
    tpLayer = std::make_unique<transport::TransportLayer>(*nwLayer);
    appLayer = std::make_unique<ApplicationLayer>(*tpLayer);

    TEST_ASSERT_TRUE(dlLayer->init(kOwnAddress).isOk());
    TEST_ASSERT_TRUE(nwLayer->init(kOwnAddress).isOk());
    TEST_ASSERT_TRUE(tpLayer->init(kOwnAddress).isOk());
    TEST_ASSERT_TRUE(appLayer->init(kOwnAddress).isOk());
}

void localTearDown()
{
    appLayer.reset();
    tpLayer.reset();
    nwLayer.reset();
    dlLayer.reset();
    physicalLayer.reset();
    platformInstance.reset();
}

void test_APPVIRT_001_restart_request_preserves_scheduler_quiescence()
{
    bool restartCalled = false;
    bool cleanupCalled = false;

    appLayer->restartService().setCleanupCallback([&]() noexcept {
        cleanupCalled = true;
    });
    appLayer->restartService().setRestartCallback([&](RestartType type) -> util::Result<void> {
        restartCalled = true;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(RestartType::Basic), static_cast<uint8_t>(type));
        return util::Result<void>::ok();
    });

    const auto initialTasks = platformInstance->taskSnapshots();
    const auto* rxTask = testsupport::findTaskByName(initialTasks, "knx_tp1_rx");
    TEST_ASSERT_NOT_NULL(rxTask);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(platform::VirtualPlatform::TaskState::Sleeping),
                            static_cast<uint8_t>(rxTask->state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(platform::VirtualPlatform::TaskSleepReason::PredicateWait),
                            static_cast<uint8_t>(rxTask->sleepReason));
    TEST_ASSERT_TRUE(testsupport::isSchedulerQuiescent(platformInstance->schedulerSnapshot()));

    datalink::LDataFrame rx;
    rx.source = IndividualAddress(1, 1, 2);
    rx.destination = GroupAddress(kOwnAddress.raw);
    rx.destinationType = AddressType::Individual;
    rx.ackRequested = false;
    rx.setTpdu(protocol::TPCI::UnnumberedData,
               APCIField::create(APCIService::Restart),
               {static_cast<uint8_t>(RestartType::Basic)});

    // The queue notify wakes the RX task, which may deliver and execute the
    // restart request immediately; advance time to let everything settle.
    physicalLayer->queueFrame(encodeTp1(rx));
    platformInstance->advanceTimeMs(1u);

    TEST_ASSERT_TRUE(cleanupCalled);
    TEST_ASSERT_TRUE(restartCalled);
    TEST_ASSERT_FALSE(appLayer->restartService().isRestartPending());

    const auto postRestartTasks = platformInstance->taskSnapshots();
    const auto* rxTaskAfter = testsupport::findTaskByName(postRestartTasks, "knx_tp1_rx");
    TEST_ASSERT_NOT_NULL(rxTaskAfter);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(platform::VirtualPlatform::TaskState::Sleeping),
                            static_cast<uint8_t>(rxTaskAfter->state));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(platform::VirtualPlatform::TaskSleepReason::PredicateWait),
                            static_cast<uint8_t>(rxTaskAfter->sleepReason));
    TEST_ASSERT_TRUE(testsupport::isSchedulerQuiescent(platformInstance->schedulerSnapshot()));
}

} // namespace

void setUp()
{
    localSetUp();
}

void tearDown()
{
    localTearDown();
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_APPVIRT_001_restart_request_preserves_scheduler_quiescence);
    return UNITY_END();
}