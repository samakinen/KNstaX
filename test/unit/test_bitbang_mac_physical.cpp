// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_bitbang_mac_physical.cpp
 * @brief Unit tests for generic TP1 MAC physical composition over bitbang backend adapter
 */

#include "unity.h"

#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/physical/bitbang_medium_backend_adapter.hpp"
#include "knx/platform/virtual_platform.hpp"
#include "../common/virtual_platform_test_utils.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <chrono>
#include <vector>

using namespace knx;
using namespace knx::physical;

namespace {

class MockBitBangTp1Driver : public BitBangDriverInterface, public BitBangDriverTp1Interface {
public:
    static constexpr auto kWaitTimeout = std::chrono::seconds(5);

    void setVirtualClock(platform::VirtualTestClock* clock) {
        _virtualClock = clock;
    }

    util::Result<void> init(const BitBangConfig&) override {
        _initialized = true;
        _state = DriverState::Idle;
        _pendingTxDeadlineUs.reset();
        return util::Result<void>::ok();
    }

    void close() override {
        _initialized = false;
        _state = DriverState::Uninitialized;
        _pendingTxDeadlineUs.reset();
    }

    util::Result<void> send(std::span<const uint8_t> frame) override {
        if (!_initialized) {
            return util::ErrorCode::NotInitialized;
        }
        _sendCalls.fetch_add(1u, std::memory_order_relaxed);
        _sendCv.notify_all();
        if (_sendError.has_value()) {
            return _sendError.value();
        }

        _lastSent.assign(frame.begin(), frame.end());

        if (_txAckResponse.has_value()) {
            Tp1RxEvent event;
            event.type = Tp1RxEventType::TxAckResponse;
            event.ackClass = _txAckResponse.value();
            _events.push_back(event);
        } else if (_virtualClock != nullptr) {
            _pendingTxDeadlineUs = _virtualClock->nowUs() + 20000u;
        }

        return util::Result<void>::ok();
    }

    void setRxCallback(RxCallback callback, void* context) override {
        _rxCallback = callback;
        _rxContext = context;
    }

    void setQueueNotifyCallback(QueueNotifyCallback callback, void* context) override {
        (void)callback;
        (void)context;
    }

    DriverState getState() const override {
        return _state;
    }

    void reset() override {
        _state = DriverState::Idle;
    }

    bool isMediumIdle() const override {
        return true;
    }

    bool isCollisionDetected() const override {
        return false;
    }

    void abortTransmission() override {
        _state = DriverState::Error;
    }

    util::Result<void> setOwnAddress(uint16_t addressRaw) override {
        _ownAddressRaw = addressRaw;
        return util::Result<void>::ok();
    }

    util::Result<void> setBusMonitorMode(bool enabled) override {
        _busMonitorMode = enabled;
        ++_monitorCalls;
        return util::Result<void>::ok();
    }

    void setAckGroupAddresses(std::span<const uint16_t> addresses) override {
        _ackGroupAddresses.assign(addresses.begin(), addresses.end());
    }

    void setLocalBusy(bool busy) override {
        _localBusy = busy;
    }

    void pollTp1() override {
        if (_pendingTxDeadlineUs.has_value() &&
            _virtualClock != nullptr &&
            _virtualClock->nowUs() >= _pendingTxDeadlineUs.value()) {
            Tp1RxEvent event;
            event.type = Tp1RxEventType::TxAckDeadlineMiss;
            _events.push_back(event);
            _pendingTxDeadlineUs.reset();
        }
    }

    bool popTp1Event(Tp1RxEvent& outEvent) override {
        if (_events.empty()) {
            return false;
        }

        outEvent = _events.front();
        _events.erase(_events.begin());
        return true;
    }

    Tp1AckDiagnosticsSnapshot getTp1AckDiagnostics() const override {
        return Tp1AckDiagnosticsSnapshot{};
    }

    void simulateReceive(std::span<const uint8_t> frame) {
        if (_rxCallback) {
            _rxCallback(frame, _rxContext);
        }
    }

    void setSendError(util::ErrorCode error) {
        _sendError = error;
    }

    void setTxAckResponse(Tp1AckClass ackClass) {
        _txAckResponse = ackClass;
    }

    void clearTxAckResponse() {
        _txAckResponse.reset();
    }

    std::span<const uint8_t> lastSent() const {
        return std::span<const uint8_t>{_lastSent};
    }

    int monitorCalls() const {
        return _monitorCalls;
    }

    uint32_t sendCalls() const {
        return _sendCalls.load(std::memory_order_relaxed);
    }

    bool waitForSendCall() {
        std::unique_lock<std::mutex> lock(_sendMutex);
        return _sendCv.wait_for(lock, kWaitTimeout, [this]() {
            return _sendCalls.load(std::memory_order_relaxed) > 0u;
        });
    }

protected:
    void process() override {
    }

    const char* getVersion() const override {
        return "mock-bitbang-tp1";
    }

private:
    bool _initialized{false};
    DriverState _state{DriverState::Uninitialized};
    RxCallback _rxCallback;
    void* _rxContext{nullptr};
    uint16_t _ownAddressRaw{0};
    bool _busMonitorMode{false};
    int _monitorCalls{0};
    std::atomic<uint32_t> _sendCalls{0};
    mutable std::mutex _sendMutex{};
    std::condition_variable _sendCv{};
    platform::VirtualTestClock* _virtualClock{nullptr};
    std::optional<uint64_t> _pendingTxDeadlineUs{};
    std::vector<uint16_t> _ackGroupAddresses{};
    bool _localBusy{false};
    std::optional<util::ErrorCode> _sendError{};
    std::optional<Tp1AckClass> _txAckResponse{Tp1AckClass::Ack};
    std::vector<uint8_t> _lastSent{};
    std::vector<Tp1RxEvent> _events{};
};

std::unique_ptr<MockBitBangTp1Driver> mockDriver;
std::unique_ptr<Tp1MacPhysical> macPhysical;
std::unique_ptr<platform::VirtualTestClock> virtualClock;
std::unique_ptr<platform::VirtualPlatform> virtualPlatform;

class ThreadSignal {
public:
    static constexpr auto kWaitTimeout = std::chrono::seconds(5);

    void notify() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _signaled = true;
        }
        _cv.notify_all();
    }

    bool wait() {
        std::unique_lock<std::mutex> lock(_mutex);
        return _cv.wait_for(lock, kWaitTimeout, [this]() { return _signaled; });
    }

private:
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _signaled{false};
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

std::vector<uint8_t> buildValidFrameWithoutAckRequest() {
    datalink::LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = false;
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
    mockDriver = std::make_unique<MockBitBangTp1Driver>();
    virtualClock = std::make_unique<platform::VirtualTestClock>();
    virtualPlatform = std::make_unique<platform::VirtualPlatform>(*virtualClock);
    mockDriver->setVirtualClock(virtualClock.get());
    macPhysical = std::make_unique<Tp1MacPhysical>(std::make_unique<BitBangMediumBackendAdapter>(*mockDriver, *mockDriver));
}

void tearDown(void) {
    if (macPhysical) {
        macPhysical->close();
        macPhysical.reset();
    }

    mockDriver.reset();
    virtualPlatform.reset();
    virtualClock.reset();
}

void test_init_send_receive_and_monitor_mode() {
    TEST_ASSERT_TRUE(macPhysical->setBusMonitorMode(Toggle::Enable).isOk());
    TEST_ASSERT_TRUE(macPhysical->init().isOk());
    TEST_ASSERT_TRUE(macPhysical->isOpen());

    auto frame = buildValidFrame();

    TEST_ASSERT_TRUE(macPhysical->sendFrame(frame).isError());

    TEST_ASSERT_TRUE(macPhysical->setBusMonitorMode(Toggle::Disable).isOk());
    TEST_ASSERT_EQUAL(2, mockDriver->monitorCalls());

    TEST_ASSERT_TRUE(macPhysical->sendFrame(frame).isOk());
    TEST_ASSERT_EQUAL(frame.size(), mockDriver->lastSent().size());

    mockDriver->simulateReceive(frame);
    auto rx = macPhysical->receiveFrame(0);
    TEST_ASSERT_TRUE(rx.isOk());
    TEST_ASSERT_EQUAL(frame.size(), rx.value().size());
}

void test_send_requires_init() {
    auto frame = buildValidFrame();
    auto send = macPhysical->sendFrame(frame);
    TEST_ASSERT_TRUE(send.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::NotInitialized), static_cast<int>(send.error()));
}

void test_receive_queue_is_bounded_and_keeps_latest_frames() {
    TEST_ASSERT_TRUE(macPhysical->init().isOk());

    for (size_t i = 0; i < Tp1MacPhysical::RX_QUEUE_CAPACITY + 2; ++i) {
        auto frame = buildValidFrame();
        frame[1] = static_cast<uint8_t>(0x11 + i);
        mockDriver->simulateReceive(frame);
    }

    for (size_t i = 0; i < Tp1MacPhysical::RX_QUEUE_CAPACITY; ++i) {
        auto rx = macPhysical->receiveFrame(0);
        TEST_ASSERT_TRUE(rx.isOk());
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(0x11 + i + 2), rx.value()[1]);
    }

    auto none = macPhysical->receiveFrame(0);
    TEST_ASSERT_TRUE(none.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Timeout), static_cast<int>(none.error()));
}

void test_receive_timeout_can_follow_virtual_time() {
    TEST_ASSERT_TRUE(macPhysical->init().isOk());
    macPhysical->setTimingPlatform(virtualPlatform.get());

    ThreadSignal finishedSignal;
    std::atomic<bool> finished{false};
    util::Result<std::vector<uint8_t>> result = util::ErrorCode::Timeout;

    knx::testsupport::VirtualTimeWorker worker(*virtualClock, [&]() {
        result = macPhysical->receiveFrame(5u);
        finished.store(true);
        finishedSignal.notify();
    });

    // Wait until the worker is actually blocked inside delayMicroseconds.
    // This eliminates the lost-wakeup race: previously started.notify() fired
    // before beginReceive(), so the main thread could advance virtual time
    // before the deadline was computed, pushing it out of reach.
    TEST_ASSERT_TRUE(virtualPlatform->waitUntilActiveDelayCalls(1u));
    TEST_ASSERT_FALSE(finished.load());

    virtualClock->advanceMs(4u);
    TEST_ASSERT_FALSE(finished.load());

    virtualClock->advanceMs(1u);
    TEST_ASSERT_TRUE(finishedSignal.wait());
    worker.join();

    TEST_ASSERT_TRUE(finished.load());
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Timeout), static_cast<int>(result.error()));
}

void test_receive_success_can_follow_virtual_time_without_wall_clock_sleep() {
    TEST_ASSERT_TRUE(macPhysical->init().isOk());
    macPhysical->setTimingPlatform(virtualPlatform.get());

    const auto frame = buildValidFrame();

    ThreadSignal started;
    ThreadSignal finishedSignal;
    std::atomic<bool> finished{false};
    util::Result<std::vector<uint8_t>> result = util::ErrorCode::Timeout;

    knx::testsupport::VirtualTimeWorker worker(*virtualClock, [&]() {
        started.notify();
        result = macPhysical->receiveFrame(20u);
        finished.store(true);
        finishedSignal.notify();
    });

    TEST_ASSERT_TRUE(started.wait());

    // started.notify() only proves the worker was spawned, not that it reached
    // receiveFrame(). Park it in its 1 ms poll delay first, then buffer the
    // frame, then advance the clock to wake it. Simulating the receive while
    // the worker is running (rather than parked) loses the race: it re-enters
    // the delay afterwards and nothing advances virtual time again, so it
    // sleeps until the ThreadSignal times out.
    TEST_ASSERT_TRUE(virtualPlatform->waitUntilActiveDelayCalls(1u));
    mockDriver->simulateReceive(frame);
    virtualClock->advanceMs(2u);

    TEST_ASSERT_TRUE(finishedSignal.wait());
    worker.join();

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(frame.size(), result.value().size());
    TEST_ASSERT_EQUAL_UINT8(frame[1], result.value()[1]);
}

void test_raw_ack_byte_is_not_buffered_as_received_frame() {
    TEST_ASSERT_TRUE(macPhysical->init().isOk());

    const uint8_t ackByte = 0xCC;
    mockDriver->simulateReceive(std::span<const uint8_t>(&ackByte, 1u));

    auto rx = macPhysical->receiveFrame(0);
    TEST_ASSERT_TRUE(rx.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Timeout), static_cast<int>(rx.error()));
}

void test_send_timeout_can_follow_virtual_time_without_wall_clock_sleep() {
    TEST_ASSERT_TRUE(macPhysical->init().isOk());
    macPhysical->setTimingPlatform(virtualPlatform.get());
    mockDriver->clearTxAckResponse();

    const auto frame = buildValidFrame();

    ThreadSignal started;
    ThreadSignal finishedSignal;
    std::atomic<bool> finished{false};
    util::Result<size_t> result = util::ErrorCode::Timeout;

    knx::testsupport::VirtualTimeWorker worker(*virtualClock, [&]() {
        started.notify();
        result = macPhysical->sendFrame(frame);
        finished.store(true);
        finishedSignal.notify();
    });

    TEST_ASSERT_TRUE(started.wait());

    TEST_ASSERT_TRUE(mockDriver->waitForSendCall());
    TEST_ASSERT_TRUE(mockDriver->sendCalls() > 0u);

    // waitForSendCall() only proves the driver's send() ran, which happens in
    // beginTransmit() — waitForTxOutcome() samples the clock for its deadline
    // *after* that. Advancing virtual time in between would compute the
    // deadline from the already-advanced clock, putting it 20 ms beyond any
    // value the clock will ever reach and hanging the worker. Sync on the
    // worker actually being parked in its poll delay instead.
    TEST_ASSERT_TRUE(virtualPlatform->waitUntilActiveDelayCalls(1u));
    TEST_ASSERT_FALSE(finished.load());

    virtualClock->advanceMs(19u);
    TEST_ASSERT_FALSE(finished.load());

    virtualClock->advanceMs(1u);
    TEST_ASSERT_TRUE(finishedSignal.wait());
    worker.join();

    TEST_ASSERT_TRUE(finished.load());
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Timeout), static_cast<int>(result.error()));
}

void test_send_timeout_uses_configured_tx_outcome_timeout() {
    TEST_ASSERT_TRUE(macPhysical->init().isOk());
    macPhysical->setTimingPlatform(virtualPlatform.get());
    macPhysical->setTxOutcomeTimeoutMs(7u);
    TEST_ASSERT_EQUAL_UINT32(7u, macPhysical->txOutcomeTimeoutMs());
    mockDriver->clearTxAckResponse();

    const auto frame = buildValidFrame();

    ThreadSignal started;
    ThreadSignal finishedSignal;
    std::atomic<bool> finished{false};
    util::Result<size_t> result = util::ErrorCode::Timeout;

    knx::testsupport::VirtualTimeWorker worker(*virtualClock, [&]() {
        started.notify();
        result = macPhysical->sendFrame(frame);
        finished.store(true);
        finishedSignal.notify();
    });

    TEST_ASSERT_TRUE(started.wait());
    TEST_ASSERT_TRUE(mockDriver->waitForSendCall());

    // See the note in the test above: the TX-outcome deadline is only computed
    // once the worker is inside waitForTxOutcome(), so wait for it to park in
    // the poll delay before touching the clock.
    TEST_ASSERT_TRUE(virtualPlatform->waitUntilActiveDelayCalls(1u));

    virtualClock->advanceMs(6u);
    TEST_ASSERT_FALSE(finished.load());

    virtualClock->advanceMs(1u);
    TEST_ASSERT_TRUE(finishedSignal.wait());
    worker.join();

    TEST_ASSERT_TRUE(finished.load());
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Timeout), static_cast<int>(result.error()));
}

void test_send_frame_without_ack_request_completes_without_tx_ack_response() {
    TEST_ASSERT_TRUE(macPhysical->init().isOk());
    mockDriver->clearTxAckResponse();

    const auto frame = buildValidFrameWithoutAckRequest();

    auto result = macPhysical->sendFrame(frame);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL(frame.size(), result.value());
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_init_send_receive_and_monitor_mode);
    RUN_TEST(test_send_requires_init);
    RUN_TEST(test_receive_queue_is_bounded_and_keeps_latest_frames);
    RUN_TEST(test_receive_timeout_can_follow_virtual_time);
    RUN_TEST(test_receive_success_can_follow_virtual_time_without_wall_clock_sleep);
    RUN_TEST(test_raw_ack_byte_is_not_buffered_as_received_frame);
    RUN_TEST(test_send_timeout_can_follow_virtual_time_without_wall_clock_sleep);
    RUN_TEST(test_send_timeout_uses_configured_tx_outcome_timeout);
    RUN_TEST(test_send_frame_without_ack_request_completes_without_tx_ack_response);

    return UNITY_END();
}