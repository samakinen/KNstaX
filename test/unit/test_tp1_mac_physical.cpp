// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_tp1_mac_physical.cpp
 * @brief Unit tests for Tp1MacPhysical over a fake medium backend: TX-ACK
 *        outcome latching and ACK-input forwarding to the backend.
 *
 * The DL-ACK decision itself is made inside the medium backend's timing
 * domain (bitbang ISR / TPUART silicon); the physical only forwards the
 * decision inputs. See test_bitbang_driver_timer_isr.cpp for the decision
 * logic tests, and test_bitbang_mac_physical.cpp for the physical over the
 * real bitbang adapter.
 */

#include "unity.h"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/types.hpp"

#include <memory>
#include <vector>

using namespace knx::physical;
using namespace knx;

namespace {

class FakeTp1Backend : public Tp1MediumBackend {
public:
    util::Result<void> init(const Tp1MediumConfig& config) override {
        _config = config;
        _state = Tp1MediumState::Idle;
        return util::Result<void>::ok();
    }

    void close() override {
        _state = Tp1MediumState::Uninitialized;
    }

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) override {
        ++_sendCalls;
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
        caps.supportsDetailedTxConfirm = true;
        return caps;
    }

    util::Result<void> setBusMonitorMode(bool enabled) override {
        _config.busMonitorMode = enabled;
        return util::Result<void>::ok();
    }

    util::Result<void> setOwnAddress(uint16_t addressRaw) override {
        _config.ownIndividualAddressRaw = addressRaw;
        return util::Result<void>::ok();
    }

    void setAckGroupAddresses(std::span<const uint16_t> addresses) override {
        _ackGroupAddresses.assign(addresses.begin(), addresses.end());
    }

    void setLocalBusy(bool busy) override {
        _localBusy = busy;
    }

    util::Result<void> service() override {
        ++_serviceCallCount;
        if (_failService) {
            return util::ErrorCode::OperationFailed;
        }
        return util::Result<void>::ok();
    }

    void emit(const Tp1RxEvent& event) {
        if (_callback) {
            _callback(event, _context);
        }
    }

    void emitTxOutcome(Tp1RxEventType type, Tp1AckClass ackClass = Tp1AckClass::None) {
        Tp1RxEvent event;
        event.type = type;
        event.ackClass = ackClass;
        emit(event);
    }

    bool hasEventCallback() const { return static_cast<bool>(_callback); }
    bool localBusy() const { return _localBusy; }
    uint16_t ownAddressRaw() const { return _config.ownIndividualAddressRaw; }
    const std::vector<uint16_t>& ackGroupAddresses() const { return _ackGroupAddresses; }
    int serviceCallCount() const { return _serviceCallCount; }
    int sendCalls() const { return _sendCalls; }
    void setFailService(bool shouldFail) { _failService = shouldFail; }

private:
    Tp1MediumConfig _config{};
    Tp1MediumState _state{Tp1MediumState::Uninitialized};
    Tp1EventCallback _callback{};
    void* _context{nullptr};
    bool _localBusy{false};
    std::vector<uint16_t> _ackGroupAddresses{};
    int _serviceCallCount{0};
    int _sendCalls{0};
    bool _failService{false};
};

FakeTp1Backend* backend = nullptr;
std::unique_ptr<Tp1MacPhysical> physicalLayer;

std::vector<uint8_t> buildGroupFrame() {
    datalink::LDataFrame frame;
    frame.standardFrame = true;
    frame.priority = Priority::Low;
    frame.ackRequested = true;
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

uint32_t beginTransmit() {
    const auto frame = buildGroupFrame();
    auto begun = physicalLayer->beginTransmit(frame);
    TEST_ASSERT_TRUE(begun.isOk());
    return begun.value();
}

int pollState(uint32_t sequence) {
    auto polled = physicalLayer->pollTransmit(sequence);
    TEST_ASSERT_TRUE(polled.isOk());
    return static_cast<int>(polled.value());
}

constexpr int kPending = static_cast<int>(TxOutcomeState::Pending);
constexpr int kSuccess = static_cast<int>(TxOutcomeState::Success);
constexpr int kBusy = static_cast<int>(TxOutcomeState::Busy);
constexpr int kFailed = static_cast<int>(TxOutcomeState::TransmissionFailed);
constexpr int kTimeout = static_cast<int>(TxOutcomeState::Timeout);

} // namespace

void setUp(void) {
    auto owned = std::make_unique<FakeTp1Backend>();
    backend = owned.get();
    physicalLayer = std::make_unique<Tp1MacPhysical>(std::move(owned));
    TEST_ASSERT_TRUE(physicalLayer->init().isOk());
}

void tearDown(void) {
    physicalLayer.reset();
    backend = nullptr;
}

void test_own_address_and_ack_group_addresses_are_forwarded_to_backend() {
    TEST_ASSERT_TRUE(physicalLayer->setOwnAddress(0x1234).isOk());
    TEST_ASSERT_EQUAL_HEX16(0x1234, backend->ownAddressRaw());

    const uint16_t addresses[] = {0x0A0A, 0x1203};
    physicalLayer->setAckGroupAddresses(std::span<const uint16_t>(addresses, 2));

    TEST_ASSERT_EQUAL(2, static_cast<int>(backend->ackGroupAddresses().size()));
    TEST_ASSERT_EQUAL_UINT16(0x0A0A, backend->ackGroupAddresses()[0]);
    TEST_ASSERT_EQUAL_UINT16(0x1203, backend->ackGroupAddresses()[1]);

    physicalLayer->setAckGroupAddresses(std::span<const uint16_t>());
    TEST_ASSERT_EQUAL(0, static_cast<int>(backend->ackGroupAddresses().size()));
}

void test_local_busy_follows_the_receive_queue_and_is_cleared_on_close() {
    const auto frame = buildGroupFrame();
    Tp1RxEvent event;
    event.type = Tp1RxEventType::TelegramEnd;
    event.frame = frame;

    const size_t busyAt = Tp1MacPhysical::RX_QUEUE_CAPACITY - Tp1MacPhysical::RX_BUSY_RESERVE;
    for (size_t i = 0; i + 1 < busyAt; ++i) {
        backend->emit(event);
    }
    TEST_ASSERT_FALSE(backend->localBusy());
    backend->emit(event);
    TEST_ASSERT_TRUE(backend->localBusy());

    physicalLayer->close();
    TEST_ASSERT_FALSE(backend->localBusy());
    TEST_ASSERT_FALSE(backend->hasEventCallback());
}

void test_tx_outcome_is_pending_until_the_backend_reports_one() {
    const uint32_t sequence = beginTransmit();
    TEST_ASSERT_EQUAL(1, backend->sendCalls());
    TEST_ASSERT_EQUAL(kPending, pollState(sequence));
    TEST_ASSERT_EQUAL(kPending, pollState(sequence));

    backend->emitTxOutcome(Tp1RxEventType::TxAckResponse, Tp1AckClass::Ack);
    TEST_ASSERT_EQUAL(kSuccess, pollState(sequence));
}

void test_tx_ack_classes_map_to_outcomes() {
    struct Case {
        Tp1AckClass ackClass;
        int expected;
    };
    const Case cases[] = {
        {Tp1AckClass::Ack, kSuccess},
        {Tp1AckClass::Busy, kBusy},
        {Tp1AckClass::NackBusy, kBusy},
        {Tp1AckClass::Nack, kFailed},
    };

    for (const auto& c : cases) {
        const uint32_t sequence = beginTransmit();
        backend->emitTxOutcome(Tp1RxEventType::TxAckResponse, c.ackClass);
        TEST_ASSERT_EQUAL(c.expected, pollState(sequence));
    }
}

void test_tx_ack_deadline_miss_reports_timeout_and_a_later_ack_clears_it() {
    uint32_t sequence = beginTransmit();
    backend->emitTxOutcome(Tp1RxEventType::TxAckDeadlineMiss, Tp1AckClass::Busy);
    TEST_ASSERT_EQUAL(kTimeout, pollState(sequence));

    sequence = beginTransmit();
    backend->emitTxOutcome(Tp1RxEventType::TxAckResponse, Tp1AckClass::Ack);
    TEST_ASSERT_EQUAL(kSuccess, pollState(sequence));
}

void test_rx_ack_observed_does_not_count_as_a_tx_outcome() {
    const uint32_t sequence = beginTransmit();

    // An ACK seen for somebody else's frame is not this transmitter's answer.
    backend->emitTxOutcome(Tp1RxEventType::RxAckObserved, Tp1AckClass::Ack);
    TEST_ASSERT_EQUAL(kPending, pollState(sequence));

    backend->emitTxOutcome(Tp1RxEventType::TxAckResponse, Tp1AckClass::Nack);
    TEST_ASSERT_EQUAL(kFailed, pollState(sequence));
}

void test_send_frame_returns_the_latched_outcome() {
    // The outcome arrives while sendFrame waits, as it does from a real ISR.
    const auto frame = buildGroupFrame();
    physicalLayer->close();

    class AnsweringBackend : public FakeTp1Backend {
    public:
        util::Result<size_t> sendFrame(std::span<const uint8_t> bytes) override {
            auto sent = FakeTp1Backend::sendFrame(bytes);
            emitTxOutcome(Tp1RxEventType::TxAckResponse, answer);
            return sent;
        }
        Tp1AckClass answer{Tp1AckClass::Ack};
    };

    auto owned = std::make_unique<AnsweringBackend>();
    auto* answering = owned.get();
    Tp1MacPhysical physical(std::move(owned));
    TEST_ASSERT_TRUE(physical.init().isOk());

    auto ok = physical.sendFrame(frame);
    TEST_ASSERT_TRUE(ok.isOk());
    TEST_ASSERT_EQUAL(frame.size(), ok.value());

    answering->answer = Tp1AckClass::Busy;
    auto busy = physical.sendFrame(frame);
    TEST_ASSERT_TRUE(busy.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Busy), static_cast<int>(busy.error()));
}

void test_poll_transmit_services_the_backend_and_propagates_its_error() {
    const uint32_t sequence = beginTransmit();
    const int before = backend->serviceCallCount();
    TEST_ASSERT_EQUAL(kPending, pollState(sequence));
    TEST_ASSERT_EQUAL(before + 1, backend->serviceCallCount());

    backend->setFailService(true);
    TEST_ASSERT_TRUE(physicalLayer->pollTransmit(sequence).isError());
}

void test_close_resets_the_tx_outcome_state() {
    uint32_t sequence = beginTransmit();
    backend->emitTxOutcome(Tp1RxEventType::TxAckResponse, Tp1AckClass::Busy);
    TEST_ASSERT_EQUAL(kBusy, pollState(sequence));

    physicalLayer->close();
    TEST_ASSERT_TRUE(physicalLayer->init().isOk());

    // The sequence starts again from zero, so a fresh transmit is Pending
    // rather than inheriting the BUSY from before the close.
    sequence = beginTransmit();
    TEST_ASSERT_EQUAL_UINT32(0u, sequence);
    TEST_ASSERT_EQUAL(kPending, pollState(sequence));
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_own_address_and_ack_group_addresses_are_forwarded_to_backend);
    RUN_TEST(test_local_busy_follows_the_receive_queue_and_is_cleared_on_close);
    RUN_TEST(test_tx_outcome_is_pending_until_the_backend_reports_one);
    RUN_TEST(test_tx_ack_classes_map_to_outcomes);
    RUN_TEST(test_tx_ack_deadline_miss_reports_timeout_and_a_later_ack_clears_it);
    RUN_TEST(test_rx_ack_observed_does_not_count_as_a_tx_outcome);
    RUN_TEST(test_send_frame_returns_the_latched_outcome);
    RUN_TEST(test_poll_transmit_services_the_backend_and_propagates_its_error);
    RUN_TEST(test_close_resets_the_tx_outcome_state);

    return UNITY_END();
}
