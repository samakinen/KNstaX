// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tp1_mac_controller.cpp
 * @brief Unit tests for the TP1 MAC controller: TX-ACK outcome latching,
 *        diagnostics forwarding, and ACK-input forwarding to the backend.
 *
 * The DL-ACK decision itself is made inside the medium backend's timing
 * domain (bitbang ISR / TPUART silicon); the controller only forwards the
 * decision inputs. See test_bitbang_driver_timer_isr.cpp for the decision
 * logic tests.
 */

#include "unity.h"
#include "knx/physical/tp1_mac_controller.hpp"
#include "knx/datalink/tp1_dl_common.hpp"
#include "knx/types.hpp"

#include <vector>

using namespace knx::physical;
using namespace knx;

namespace {

class FakeTp1Backend : public Tp1MediumBackend,
                       public Tp1MediumDiagnostics {
public:
    util::Result<void> init(const Tp1MediumConfig& config) override {
        _config = config;
        _state = Tp1MediumState::Idle;
        _diagnosticsSnapshotRequestCount = 0;
        _serviceCallCount = 0;
        _supportsDiagnosticsSnapshot = true;
        _failDiagnosticsSnapshotRequest = false;
        _failService = false;
        _localBusy = false;
        _ackGroupAddresses.clear();
        return util::Result<void>::ok();
    }

    void close() override {
        _state = Tp1MediumState::Uninitialized;
    }

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) override {
        (void)frame;
        return frame.size();
    }

    void setEventCallback(Tp1EventCallback callback, void* context) override {
        _callback = callback;
        _context = context;
    }

    Tp1MediumState getState() const override {
        return _state;
    }

    Tp1CapabilityProfile getCapabilities() const override {
        Tp1CapabilityProfile caps;
        caps.supportsByteEventStream = true;
        caps.supportsDiagnosticsSnapshot = _supportsDiagnosticsSnapshot;
        return caps;
    }

    util::Result<void> setBusMonitorMode(bool enabled) override {
        _config.busMonitorMode = enabled;
        return util::Result<void>::ok();
    }

    void setAckGroupAddresses(std::span<const uint16_t> addresses) override {
        _ackGroupAddresses.assign(addresses.begin(), addresses.end());
    }

    void setLocalBusy(bool busy) override {
        _localBusy = busy;
    }

    util::Result<void> requestDiagnosticsSnapshot() override {
        ++_diagnosticsSnapshotRequestCount;
        if (_failDiagnosticsSnapshotRequest) {
            return util::ErrorCode::Busy;
        }
        return util::Result<void>::ok();
    }

    util::Result<void> service() override {
        ++_serviceCallCount;
        if (_failService) {
            return util::ErrorCode::OperationFailed;
        }
        return util::Result<void>::ok();
    }

    Tp1MediumDiagnostics* diagnostics() override {
        return this;
    }

    const Tp1MediumDiagnostics* diagnostics() const override {
        return this;
    }

    void emit(const Tp1RxEvent& event) {
        if (_callback) {
            _callback(event, _context);
        }
    }

    bool localBusy() const {
        return _localBusy;
    }

    const std::vector<uint16_t>& ackGroupAddresses() const {
        return _ackGroupAddresses;
    }

    int getDiagnosticsSnapshotRequestCount() const {
        return _diagnosticsSnapshotRequestCount;
    }

    int getServiceCallCount() const {
        return _serviceCallCount;
    }

    void setSupportsDiagnosticsSnapshot(bool supported) {
        _supportsDiagnosticsSnapshot = supported;
    }

    void setFailDiagnosticsSnapshotRequest(bool shouldFail) {
        _failDiagnosticsSnapshotRequest = shouldFail;
    }

    void setFailService(bool shouldFail) {
        _failService = shouldFail;
    }

private:
    Tp1MediumConfig _config{};
    Tp1MediumState _state{Tp1MediumState::Uninitialized};
    Tp1EventCallback _callback{};
    void* _context{nullptr};
    bool _localBusy{false};
    std::vector<uint16_t> _ackGroupAddresses{};
    int _diagnosticsSnapshotRequestCount{0};
    int _serviceCallCount{0};
    bool _supportsDiagnosticsSnapshot{true};
    bool _failDiagnosticsSnapshotRequest{false};
    bool _failService{false};
};

FakeTp1Backend backend;
Tp1MacController controller(backend);

} // namespace

void setUp(void) {
    Tp1MediumConfig config;
    config.ownIndividualAddressRaw = 0x1234;
    config.busMonitorMode = false;
    TEST_ASSERT_TRUE(controller.init(config).isOk());
    controller.setLocalBusy(false);
}

void tearDown(void) {
    controller.close();
}

void test_local_busy_is_forwarded_to_backend() {
    TEST_ASSERT_FALSE(backend.localBusy());

    controller.setLocalBusy(true);
    TEST_ASSERT_TRUE(backend.localBusy());

    controller.setLocalBusy(false);
    TEST_ASSERT_FALSE(backend.localBusy());
}

void test_ack_group_addresses_are_forwarded_to_backend() {
    const uint16_t addresses[] = {0x0A0A, 0x1203};
    controller.setAckGroupAddresses(std::span<const uint16_t>(addresses, 2));

    TEST_ASSERT_EQUAL(2, static_cast<int>(backend.ackGroupAddresses().size()));
    TEST_ASSERT_EQUAL_UINT16(0x0A0A, backend.ackGroupAddresses()[0]);
    TEST_ASSERT_EQUAL_UINT16(0x1203, backend.ackGroupAddresses()[1]);

    controller.setAckGroupAddresses(std::span<const uint16_t>());
    TEST_ASSERT_EQUAL(0, static_cast<int>(backend.ackGroupAddresses().size()));
}

void test_tx_ack_response_latching_updates_from_backend_event() {
    TEST_ASSERT_EQUAL(static_cast<int>(Tp1AckClass::None),
                      static_cast<int>(controller.getLastTxAckResponse()));

    Tp1RxEvent event;
    event.type = Tp1RxEventType::TxAckResponse;
    event.ackClass = Tp1AckClass::Busy;
    backend.emit(event);

    TEST_ASSERT_EQUAL(static_cast<int>(Tp1AckClass::Busy),
                      static_cast<int>(controller.getLastTxAckResponse()));

    event.ackClass = Tp1AckClass::Ack;
    backend.emit(event);

    TEST_ASSERT_EQUAL(static_cast<int>(Tp1AckClass::Ack),
                      static_cast<int>(controller.getLastTxAckResponse()));
}

void test_tx_ack_deadline_miss_flag_updates_from_backend_event() {
    Tp1RxEvent event;
    event.type = Tp1RxEventType::TxAckResponse;
    event.ackClass = Tp1AckClass::Busy;
    backend.emit(event);

    TEST_ASSERT_FALSE(controller.didLastTxAckMissDeadline());
    TEST_ASSERT_EQUAL(static_cast<int>(Tp1AckClass::Busy),
                      static_cast<int>(controller.getLastTxAckResponse()));

    event.type = Tp1RxEventType::TxAckDeadlineMiss;
    event.ackClass = Tp1AckClass::Busy;
    backend.emit(event);

    TEST_ASSERT_TRUE(controller.didLastTxAckMissDeadline());
    TEST_ASSERT_EQUAL(static_cast<int>(Tp1AckClass::None),
                      static_cast<int>(controller.getLastTxAckResponse()));
}

void test_rx_ack_observed_does_not_mutate_tx_ack_state() {
    Tp1RxEvent event;
    event.type = Tp1RxEventType::TxAckResponse;
    event.ackClass = Tp1AckClass::Busy;
    backend.emit(event);

    TEST_ASSERT_FALSE(controller.didLastTxAckMissDeadline());
    TEST_ASSERT_EQUAL(static_cast<int>(Tp1AckClass::Busy),
                      static_cast<int>(controller.getLastTxAckResponse()));

    event.type = Tp1RxEventType::RxAckObserved;
    event.ackClass = Tp1AckClass::Ack;
    backend.emit(event);

    TEST_ASSERT_FALSE(controller.didLastTxAckMissDeadline());
    TEST_ASSERT_EQUAL(static_cast<int>(Tp1AckClass::Busy),
                      static_cast<int>(controller.getLastTxAckResponse()));
}

void test_request_diagnostics_snapshot_is_forwarded_to_backend() {
    TEST_ASSERT_EQUAL(0, backend.getDiagnosticsSnapshotRequestCount());

    TEST_ASSERT_TRUE(controller.requestDiagnosticsSnapshot().isOk());

    TEST_ASSERT_EQUAL(1, backend.getDiagnosticsSnapshotRequestCount());
}

void test_supports_diagnostics_snapshot_reflects_backend_capability() {
    backend.setSupportsDiagnosticsSnapshot(true);
    TEST_ASSERT_TRUE(controller.supportsDiagnosticsSnapshot());

    backend.setSupportsDiagnosticsSnapshot(false);
    TEST_ASSERT_FALSE(controller.supportsDiagnosticsSnapshot());
}

void test_on_tick_triggers_periodic_diagnostics_snapshot() {
    controller.setDiagnosticsSnapshotPeriod(3);
    TEST_ASSERT_EQUAL(0, backend.getDiagnosticsSnapshotRequestCount());

    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_EQUAL(0, backend.getDiagnosticsSnapshotRequestCount());

    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_EQUAL(1, backend.getDiagnosticsSnapshotRequestCount());

    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_EQUAL(2, backend.getDiagnosticsSnapshotRequestCount());
}

void test_on_tick_noop_when_period_disabled() {
    controller.setDiagnosticsSnapshotPeriod(0);
    const int beforeServiceCalls = backend.getServiceCallCount();
    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_EQUAL(0, backend.getDiagnosticsSnapshotRequestCount());
    TEST_ASSERT_EQUAL(beforeServiceCalls + 2, backend.getServiceCallCount());
}

void test_on_tick_propagates_backend_service_error() {
    backend.setFailService(true);
    TEST_ASSERT_TRUE(controller.onTick().isError());
}

void test_on_tick_skips_snapshot_when_capability_not_supported() {
    backend.setSupportsDiagnosticsSnapshot(false);
    controller.setDiagnosticsSnapshotPeriod(1);

    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_EQUAL(0, backend.getDiagnosticsSnapshotRequestCount());
}

void test_on_tick_applies_retry_backoff_after_snapshot_failure() {
    backend.setFailDiagnosticsSnapshotRequest(true);
    controller.setDiagnosticsSnapshotPeriod(1);
    controller.setDiagnosticsRetryBackoffTicks(2);

    TEST_ASSERT_TRUE(controller.onTick().isError());
    TEST_ASSERT_EQUAL(1, backend.getDiagnosticsSnapshotRequestCount());

    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_EQUAL(1, backend.getDiagnosticsSnapshotRequestCount());

    TEST_ASSERT_TRUE(controller.onTick().isError());
    TEST_ASSERT_EQUAL(2, backend.getDiagnosticsSnapshotRequestCount());
}

void test_on_tick_retry_cooldown_progresses_while_capability_disabled() {
    backend.setFailDiagnosticsSnapshotRequest(true);
    controller.setDiagnosticsSnapshotPeriod(1);
    controller.setDiagnosticsRetryBackoffTicks(2);

    TEST_ASSERT_TRUE(controller.onTick().isError());
    TEST_ASSERT_EQUAL(1, backend.getDiagnosticsSnapshotRequestCount());

    backend.setSupportsDiagnosticsSnapshot(false);
    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_TRUE(controller.onTick().isOk());
    TEST_ASSERT_EQUAL(1, backend.getDiagnosticsSnapshotRequestCount());

    backend.setSupportsDiagnosticsSnapshot(true);
    TEST_ASSERT_TRUE(controller.onTick().isError());
    TEST_ASSERT_EQUAL(2, backend.getDiagnosticsSnapshotRequestCount());
}

void test_ack_diagnostics_snapshot_is_cached_from_backend_event() {
    TEST_ASSERT_FALSE(controller.hasAckDiagnosticsSnapshot());

    Tp1RxEvent event;
    event.type = Tp1RxEventType::AckDiagnosticsSnapshot;
    event.ackDiagnostics.windowOpenedNoDecisionCount = 3;
    event.ackDiagnostics.decisionLatchedLateCount = 4;
    event.ackDiagnostics.responseEmittedCount = 5;
    event.ackDiagnostics.deadlineMissCount = 6;
    event.ackDiagnostics.overflowErrorCount = 7;
    event.ackDiagnostics.rxAckObservedCount = 8;
    event.ackDiagnostics.unsupportedRawIngressCount = 9;
    backend.emit(event);

    TEST_ASSERT_TRUE(controller.hasAckDiagnosticsSnapshot());
    const auto snapshot = controller.getLatestAckDiagnosticsSnapshot();
    TEST_ASSERT_EQUAL(3, static_cast<int>(snapshot.windowOpenedNoDecisionCount));
    TEST_ASSERT_EQUAL(4, static_cast<int>(snapshot.decisionLatchedLateCount));
    TEST_ASSERT_EQUAL(5, static_cast<int>(snapshot.responseEmittedCount));
    TEST_ASSERT_EQUAL(6, static_cast<int>(snapshot.deadlineMissCount));
    TEST_ASSERT_EQUAL(7, static_cast<int>(snapshot.overflowErrorCount));
    TEST_ASSERT_EQUAL(8, static_cast<int>(snapshot.rxAckObservedCount));
    TEST_ASSERT_EQUAL(9, static_cast<int>(snapshot.unsupportedRawIngressCount));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_local_busy_is_forwarded_to_backend);
    RUN_TEST(test_ack_group_addresses_are_forwarded_to_backend);
    RUN_TEST(test_tx_ack_response_latching_updates_from_backend_event);
    RUN_TEST(test_tx_ack_deadline_miss_flag_updates_from_backend_event);
    RUN_TEST(test_rx_ack_observed_does_not_mutate_tx_ack_state);
    RUN_TEST(test_request_diagnostics_snapshot_is_forwarded_to_backend);
    RUN_TEST(test_supports_diagnostics_snapshot_reflects_backend_capability);
    RUN_TEST(test_on_tick_triggers_periodic_diagnostics_snapshot);
    RUN_TEST(test_on_tick_noop_when_period_disabled);
    RUN_TEST(test_on_tick_propagates_backend_service_error);
    RUN_TEST(test_on_tick_skips_snapshot_when_capability_not_supported);
    RUN_TEST(test_on_tick_applies_retry_backoff_after_snapshot_failure);
    RUN_TEST(test_on_tick_retry_cooldown_progresses_while_capability_disabled);
    RUN_TEST(test_ack_diagnostics_snapshot_is_cached_from_backend_event);

    return UNITY_END();
}
