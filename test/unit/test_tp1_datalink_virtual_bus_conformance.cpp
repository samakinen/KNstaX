// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/physical/tp1_mac_physical.hpp"
#include "../common/virtual_bitbang_tp1_driver.hpp"
#include "../common/virtual_tp1_test_scheduler.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/network/network_layer.hpp"
#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/bitbang_driver_timer_isr.hpp"
#include "knx/physical/bitbang_driver_tp1_interface.hpp"
#include "knx/physical/bitbang_medium_backend_adapter.hpp"
#include "knx/physical/tp1_ack_utils.hpp"
#include "knx/physical/timer_gpio_hal_virtual.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/transport/transport_layer.hpp"
#include "knx/physical/virtual_tp1_bus_peer.hpp"
#include "knx/physical/virtual_tp1_test_runtime.hpp"

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <algorithm>
#include <string>
#include <vector>

using namespace knx;
using namespace knx::datalink;
using namespace knx::physical;
using namespace knx::testsupport;

namespace {

/// Tasking platform that never runs the task it "creates" and records when it
/// is deleted, so the order of Tp1DataLinkLayer::close() can be observed.
class TeardownOrderPlatform : public platform::TaskingPlatform {
public:
    TeardownOrderPlatform(platform::TaskingPlatform& inner, std::vector<std::string>& log)
        : _inner(inner), _log(log) {}

    platform::MutexHandle createMutex() override { return _inner.createMutex(); }
    void deleteMutex(platform::MutexHandle mutex) override { _inner.deleteMutex(mutex); }
    util::Result<void> lockMutex(platform::MutexHandle mutex, uint32_t timeoutMs) override {
        return _inner.lockMutex(mutex, timeoutMs);
    }
    void unlockMutex(platform::MutexHandle mutex) override { _inner.unlockMutex(mutex); }

    platform::TaskHandle createTask(const platform::TaskConfig&) override { return &_dummyTask; }
    void deleteTask(platform::TaskHandle) override { _log.push_back("deleteTask"); }
    platform::TaskHandle currentTask() override { return nullptr; }
    void taskDelay(uint32_t) override {}
    void taskYield() override {}
    util::Result<void> taskNotifyGive(platform::TaskHandle) override { return util::Result<void>::ok(); }
    util::Result<void> taskNotifyGiveFromISR(platform::TaskHandle) override { return util::Result<void>::ok(); }
    uint32_t taskNotifyTake(platform::TaskNotifyClearMode, uint32_t) override { return 0u; }

private:
    platform::TaskingPlatform& _inner;
    std::vector<std::string>& _log;
    int _dummyTask{0};
};

VirtualTp1TestRuntime runtime;
VirtualTp1BusPeer busPeer(runtime.bus());
std::unique_ptr<VirtualBitBangTp1Driver> driver;
std::unique_ptr<Tp1MacPhysical> macPhysical;
std::unique_ptr<Tp1DataLinkLayer> dlLayer;
std::unique_ptr<network::NetworkLayer> nwLayer;
std::unique_ptr<transport::TransportLayer> tpLayer;
std::unique_ptr<VirtualTp1TestScheduler> scheduler;
std::vector<LDataFrame> receivedFrames;
std::optional<network::NDataFrame> capturedNetworkFrame;

constexpr IndividualAddress kOwnAddress(1, 1, 10);
constexpr IndividualAddress kRemoteAddress(1, 1, 20);

std::vector<uint8_t> encodeTp1(const LDataFrame& frame) {
    uint8_t buffer[23];
    auto res = FrameCodec::encodeFrame(frame, std::span<uint8_t>(buffer));
    TEST_ASSERT_TRUE(res.isOk());
    return std::vector<uint8_t>(buffer, buffer + res.value());
}

LDataFrame decodeTp1OrFail(std::span<const uint8_t> raw) {
    LDataFrame frame;
    auto res = FrameCodec::decodeFrame(raw, frame);
    TEST_ASSERT_TRUE(res.isOk());
    return frame;
}

LDataFrame makeInboundIndividualWrite(const IndividualAddress& destination, uint8_t payload, bool ackRequested = true) {
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = ackRequested;
    frame.confirmation = false;
    frame.source = kRemoteAddress;
    frame.setDestination(destination);
    frame.hopCount = 6;
    frame.setTpdu(protocol::TPCI::UnnumberedData,
                  application::APCIService::GroupValueWrite,
                  {payload});
    return frame;
}

void injectRawFrame(std::span<const uint8_t> raw, uint64_t startUs = 20u) {
    TEST_ASSERT_TRUE(scheduler->injectRawFrame(raw, startUs));
}

void injectInboundLDataFrame(const LDataFrame& frame, uint64_t startUs = 20u) {
    TEST_ASSERT_TRUE(scheduler->injectLDataFrame(frame, startUs));
}

void expectNoDeliveredFrames() {
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(receivedFrames.size()));
}

util::Result<size_t> sendRawFrameWithVirtualTxOutcome(std::span<const uint8_t> raw,
                                                      VirtualTp1PeerResponse response)
{
    driver->clearSentFrames();
    busPeer.clearScript();

    auto beginResult = macPhysical->beginTransmit(raw);
    TEST_ASSERT_TRUE(beginResult.isOk());

    TEST_ASSERT_TRUE(scheduler->advanceUntilSentFrameCount(1u));
    TEST_ASSERT_TRUE(scheduler->driveTxOutcomeForSentFrame(0u, response));

    for (uint32_t i = 0u; i < 50u; ++i) {
        auto outcome = macPhysical->pollTransmit(beginResult.value());
        TEST_ASSERT_TRUE(outcome.isOk());
        switch (outcome.value()) {
            case Tp1TxOutcomeState::Pending:
                runtime.advanceTimeMs(1u);
                continue;
            case Tp1TxOutcomeState::Success:
                return raw.size();
            case Tp1TxOutcomeState::Busy:
                return util::ErrorCode::Busy;
            case Tp1TxOutcomeState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
            case Tp1TxOutcomeState::Timeout:
                return util::ErrorCode::Timeout;
        }
    }

    return util::ErrorCode::Timeout;
}

LDataFrame makeOutboundGroupWrite(const GroupAddress& destination, uint8_t payload) {
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = true;
    frame.confirmation = false;
    frame.source = kOwnAddress;
    frame.destination = destination;
    frame.destinationType = AddressType::Group;
    frame.hopCount = 6;
    frame.setTpdu(protocol::TPCI::UnnumberedData,
                  application::APCIService::GroupValueWrite,
                  {payload});
    return frame;
}

size_t processRxAvailableWithTxOutcome(VirtualTp1PeerResponse response,
                                      size_t expectedSentFrameCount)
{
    auto beginResult = dlLayer->beginReceive(0u);
    TEST_ASSERT_TRUE(beginResult.isOk());

    bool droveTxOutcome = (expectedSentFrameCount == 0u);

    for (uint32_t i = 0u; i < 150u; ++i) {
        auto progress = dlLayer->pollReceive();
        TEST_ASSERT_TRUE(progress.isOk());
        switch (progress.value()) {
            case Tp1DataLinkLayer::RxProgressState::Pending:
                if (!droveTxOutcome && driver->sentFrameCount() >= expectedSentFrameCount) {
                    TEST_ASSERT_TRUE(scheduler->driveTxOutcomeForSentFrame(expectedSentFrameCount - 1u,
                                                                           response));
                    droveTxOutcome = true;
                }
                runtime.advanceTimeMs(1u);
                continue;
            case Tp1DataLinkLayer::RxProgressState::Complete:
                return dlLayer->rxProgressDeliveredCount();
        }
    }

    TEST_FAIL_MESSAGE("Timed out completing data-link RX progression");
    return 0u;
}

util::Result<void> sendLDataFrameWithAckSequence(const LDataFrame& frame,
                                                 const std::vector<VirtualTp1PeerResponse>& ackSequence)
{
    driver->clearSentFrames();
    busPeer.clearScript();

    auto beginResult = dlLayer->beginTransmit(frame);
    TEST_ASSERT_TRUE(beginResult.isOk());

    for (size_t attempt = 0; attempt < ackSequence.size(); ++attempt) {
        const size_t expectedCount = attempt + 1u;
        bool startedAttempt = false;
        for (uint32_t i = 0u; i < 150u; ++i) {
            auto progress = dlLayer->pollTransmit();
            TEST_ASSERT_TRUE(progress.isOk());
            switch (progress.value()) {
                case Tp1DataLinkLayer::TxProgressState::Pending:
                    if (driver->sentFrameCount() >= expectedCount) {
                        startedAttempt = true;
                        i = 150u;
                        break;
                    }
                    runtime.advanceTimeMs(1u);
                    continue;
                case Tp1DataLinkLayer::TxProgressState::Success:
                    return util::Result<void>::ok();
                case Tp1DataLinkLayer::TxProgressState::Busy:
                    return util::ErrorCode::Busy;
                case Tp1DataLinkLayer::TxProgressState::TransmissionFailed:
                    return util::ErrorCode::TransmissionFailed;
                case Tp1DataLinkLayer::TxProgressState::Timeout:
                    return util::ErrorCode::Timeout;
            }
        }

        if (!startedAttempt) {
            TEST_FAIL_MESSAGE("Timed out waiting for data-link TX attempt");
        }

        TEST_ASSERT_TRUE(scheduler->driveTxOutcomeForSentFrame(attempt, ackSequence[attempt]));

        const bool expectAnotherAttempt = (attempt + 1u) < ackSequence.size();
        bool advancedToNextAttempt = false;

        for (uint32_t i = 0u; i < 150u; ++i) {
            auto progress = dlLayer->pollTransmit();
            TEST_ASSERT_TRUE(progress.isOk());
            switch (progress.value()) {
                case Tp1DataLinkLayer::TxProgressState::Pending:
                    if (expectAnotherAttempt && driver->sentFrameCount() >= (expectedCount + 1u)) {
                        advancedToNextAttempt = true;
                        i = 150u;
                        break;
                    }
                    runtime.advanceTimeMs(1u);
                    continue;
                case Tp1DataLinkLayer::TxProgressState::Success:
                    return util::Result<void>::ok();
                case Tp1DataLinkLayer::TxProgressState::Busy:
                    return util::ErrorCode::Busy;
                case Tp1DataLinkLayer::TxProgressState::TransmissionFailed:
                    return util::ErrorCode::TransmissionFailed;
                case Tp1DataLinkLayer::TxProgressState::Timeout:
                    return util::ErrorCode::Timeout;
            }
        }

        if (expectAnotherAttempt && !advancedToNextAttempt) {
            TEST_FAIL_MESSAGE("Timed out waiting for retried data-link TX attempt");
        }
    }

    return util::ErrorCode::Timeout;
}

void initTransportStack(const IndividualAddress& ownAddress = kOwnAddress) {
    nwLayer = std::make_unique<network::NetworkLayer>(*dlLayer);
    tpLayer = std::make_unique<transport::TransportLayer>(*nwLayer);

    TEST_ASSERT_TRUE(nwLayer->init(ownAddress).isOk());
    TEST_ASSERT_TRUE(tpLayer->init(ownAddress).isOk());
    capturedNetworkFrame.reset();
}

LDataFrame makeInboundTransportControlFrame(const IndividualAddress& source,
                                            const protocol::TPCIField& tpci)
{
    LDataFrame frame;
    frame.standardFrame = true;
    frame.repeated = false;
    frame.priority = Priority::Low;
    frame.ackRequested = false;
    frame.confirmation = false;
    frame.source = source;
    frame.setDestination(kOwnAddress);
    frame.hopCount = 6;
    frame.tpdu = {};
    frame.setTpdu(tpci, application::APCIField(0), std::span<const uint8_t>{});
    return frame;
}

util::Result<ConnectionIndex> connectTransportWithLowLevelAck(const IndividualAddress& remote) {
    const size_t initialSentCount = driver->sentFrameCount();
    auto beginResult = tpLayer->beginConnect(remote);
    TEST_ASSERT_TRUE(beginResult.isOk());

    bool startedAttempt = false;
    for (uint32_t i = 0u; i < 150u; ++i) {
        auto progress = tpLayer->pollConnect();
        TEST_ASSERT_TRUE(progress.isOk());
        switch (progress.value()) {
            case transport::TransportLayer::ControlSendProgressState::Pending:
                if (driver->sentFrameCount() >= (initialSentCount + 1u)) {
                    startedAttempt = true;
                    i = 150u;
                    break;
                }
                runtime.advanceTimeMs(1u);
                continue;
            case transport::TransportLayer::ControlSendProgressState::Success:
                return beginResult.value();
            case transport::TransportLayer::ControlSendProgressState::Busy:
                return util::ErrorCode::Busy;
            case transport::TransportLayer::ControlSendProgressState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
            case transport::TransportLayer::ControlSendProgressState::Timeout:
                return util::ErrorCode::Timeout;
        }
    }

    if (!startedAttempt) {
        TEST_FAIL_MESSAGE("Timed out waiting for transport connect TX attempt");
    }

    TEST_ASSERT_TRUE(scheduler->driveTxOutcomeForSentFrame(initialSentCount,
                                                           VirtualTp1PeerResponse::Ack));

    for (uint32_t i = 0u; i < 150u; ++i) {
        auto progress = tpLayer->pollConnect();
        TEST_ASSERT_TRUE(progress.isOk());
        switch (progress.value()) {
            case transport::TransportLayer::ControlSendProgressState::Pending:
                runtime.advanceTimeMs(1u);
                continue;
            case transport::TransportLayer::ControlSendProgressState::Success:
                return beginResult.value();
            case transport::TransportLayer::ControlSendProgressState::Busy:
                return util::ErrorCode::Busy;
            case transport::TransportLayer::ControlSendProgressState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
            case transport::TransportLayer::ControlSendProgressState::Timeout:
                return util::ErrorCode::Timeout;
        }
    }

    return util::ErrorCode::Timeout;
}

util::Result<void> sendConnectedDataWithLowLevelAck(ConnectionIndex index, std::span<const uint8_t> payload) {
    const size_t initialSentCount = driver->sentFrameCount();
    auto beginResult = tpLayer->beginSendConnectedData(index, payload);
    TEST_ASSERT_TRUE(beginResult.isOk());

    bool startedAttempt = false;
    for (uint32_t i = 0u; i < 150u; ++i) {
        auto progress = tpLayer->pollConnectedDataSend();
        TEST_ASSERT_TRUE(progress.isOk());
        switch (progress.value()) {
            case transport::TransportLayer::ConnectedSendProgressState::Pending:
                if (driver->sentFrameCount() >= (initialSentCount + 1u)) {
                    startedAttempt = true;
                    i = 150u;
                    break;
                }
                runtime.advanceTimeMs(1u);
                continue;
            case transport::TransportLayer::ConnectedSendProgressState::Success:
                return util::Result<void>::ok();
            case transport::TransportLayer::ConnectedSendProgressState::Busy:
                return util::ErrorCode::Busy;
            case transport::TransportLayer::ConnectedSendProgressState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
            case transport::TransportLayer::ConnectedSendProgressState::Timeout:
                return util::ErrorCode::Timeout;
        }
    }

    if (!startedAttempt) {
        TEST_FAIL_MESSAGE("Timed out waiting for transport TX attempt");
    }

    TEST_ASSERT_TRUE(scheduler->driveTxOutcomeForSentFrame(initialSentCount,
                                                           VirtualTp1PeerResponse::Ack));

    for (uint32_t i = 0u; i < 150u; ++i) {
        auto progress = tpLayer->pollConnectedDataSend();
        TEST_ASSERT_TRUE(progress.isOk());
        switch (progress.value()) {
            case transport::TransportLayer::ConnectedSendProgressState::Pending:
                runtime.advanceTimeMs(1u);
                continue;
            case transport::TransportLayer::ConnectedSendProgressState::Success:
                return util::Result<void>::ok();
            case transport::TransportLayer::ConnectedSendProgressState::Busy:
                return util::ErrorCode::Busy;
            case transport::TransportLayer::ConnectedSendProgressState::TransmissionFailed:
                return util::ErrorCode::TransmissionFailed;
            case transport::TransportLayer::ConnectedSendProgressState::Timeout:
                return util::ErrorCode::Timeout;
        }
    }

    return util::ErrorCode::Timeout;
}

util::Result<void> sendConnectedDataWithLowLevelAck(ConnectionIndex index, std::initializer_list<uint8_t> payload) {
    return sendConnectedDataWithLowLevelAck(index, std::span<const uint8_t>(payload.begin(), payload.size()));
}

void processRetransmissionsWithLowLevelAck(uint32_t currentTimeMs) {
    const size_t initialSentCount = driver->sentFrameCount();
    auto beginResult = tpLayer->beginProcessRetransmissions(currentTimeMs);
    TEST_ASSERT_TRUE(beginResult.isOk());

    bool startedAttempt = false;
    for (uint32_t i = 0u; i < 150u; ++i) {
        auto progress = tpLayer->pollProcessRetransmissions();
        TEST_ASSERT_TRUE(progress.isOk());
        switch (progress.value()) {
            case transport::TransportLayer::ControlSendProgressState::Pending:
                if (driver->sentFrameCount() >= (initialSentCount + 1u)) {
                    startedAttempt = true;
                    i = 150u;
                    break;
                }
                runtime.advanceTimeMs(1u);
                continue;
            case transport::TransportLayer::ControlSendProgressState::Success:
                return;
            case transport::TransportLayer::ControlSendProgressState::Busy:
                TEST_FAIL_MESSAGE("Retransmission send reported busy");
            case transport::TransportLayer::ControlSendProgressState::TransmissionFailed:
                TEST_FAIL_MESSAGE("Retransmission send failed");
            case transport::TransportLayer::ControlSendProgressState::Timeout:
                TEST_FAIL_MESSAGE("Retransmission send timed out");
        }
    }

    if (!startedAttempt) {
        TEST_FAIL_MESSAGE("Timed out waiting for retransmission TX attempt");
    }

    TEST_ASSERT_TRUE(scheduler->driveTxOutcomeForSentFrame(initialSentCount,
                                                           VirtualTp1PeerResponse::Ack));

    for (uint32_t i = 0u; i < 150u; ++i) {
        auto progress = tpLayer->pollProcessRetransmissions();
        TEST_ASSERT_TRUE(progress.isOk());
        switch (progress.value()) {
            case transport::TransportLayer::ControlSendProgressState::Pending:
                runtime.advanceTimeMs(1u);
                continue;
            case transport::TransportLayer::ControlSendProgressState::Success:
                return;
            case transport::TransportLayer::ControlSendProgressState::Busy:
                TEST_FAIL_MESSAGE("Retransmission send reported busy");
            case transport::TransportLayer::ControlSendProgressState::TransmissionFailed:
                TEST_FAIL_MESSAGE("Retransmission send failed");
            case transport::TransportLayer::ControlSendProgressState::Timeout:
                TEST_FAIL_MESSAGE("Retransmission send timed out");
        }
    }

    TEST_FAIL_MESSAGE("Timed out completing retransmission processing");
}

void captureNextTransportNetworkFrame() {
    capturedNetworkFrame.reset();
    nwLayer->setReceiveCallback([](const network::NDataFrame& frame) {
        capturedNetworkFrame = frame;
    });
}

} // namespace

void setUp() {
    runtime.reset();
    busPeer.clearScript();
    receivedFrames.clear();

    driver = std::make_unique<VirtualBitBangTp1Driver>(runtime.bus());
    macPhysical = std::make_unique<Tp1MacPhysical>(
        std::make_unique<BitBangMediumBackendAdapter>(*driver, *driver));
    macPhysical->setTimingPlatform(&runtime.platform());

    Tp1DataLinkConfig config = Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;

    dlLayer = std::make_unique<Tp1DataLinkLayer>(runtime.platform(), *macPhysical, config);
    dlLayer->setReceiveCallback([](const LDataFrame& frame) {
        receivedFrames.push_back(frame);
    });

    TEST_ASSERT_TRUE(dlLayer->init(kOwnAddress).isOk());
    macPhysical->setReceiveCallback(nullptr, nullptr);
    scheduler = std::make_unique<VirtualTp1TestScheduler>(runtime, busPeer, *driver);
    driver->clearSentFrames();
}

void tearDown() {
    if (dlLayer) {
        dlLayer->close();
        dlLayer.reset();
    }
    if (tpLayer) {
        tpLayer->close();
        tpLayer.reset();
    }
    if (nwLayer) {
        nwLayer->close();
        nwLayer.reset();
    }
    if (macPhysical) {
        macPhysical->close();
        macPhysical.reset();
    }
    if (driver) {
        driver->close();
        driver.reset();
    }
    scheduler.reset();
    receivedFrames.clear();
}

void test_VDLBUS_001_targeted_individual_rx_is_delivered_without_dl_frames() {
    // KNX 03_02_02 §2.2.7: the acknowledgment for a received L_Data frame is
    // the single-character short-acknowledge, generated at the MAC/ISR level.
    // The data link layer must deliver the frame and emit NO L_Data traffic
    // of its own.
    driver->clearSentFrames();

    injectRawFrame(encodeTp1(makeInboundIndividualWrite(kOwnAddress, 0x41u, true)));

    const size_t delivered = processRxAvailableWithTxOutcome(VirtualTp1PeerResponse::Silent, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(delivered));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(receivedFrames.size()));
    TEST_ASSERT_EQUAL_UINT16(kOwnAddress.raw, receivedFrames[0].destination.raw);
    TEST_ASSERT_TRUE(receivedFrames[0].destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL_UINT8(0x41u, receivedFrames[0].payload()[0]);

    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(driver->sentFrameCount()));

    const auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(1u, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.filterDropped);
}

void test_VDLBUS_002_non_targeted_individual_rx_is_filtered_without_ack() {
    driver->clearSentFrames();

    injectRawFrame(encodeTp1(makeInboundIndividualWrite(IndividualAddress(1, 1, 11), 0x52u, true)));

    const size_t delivered = processRxAvailableWithTxOutcome(VirtualTp1PeerResponse::Silent, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(delivered));
    expectNoDeliveredFrames();
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(driver->sentFrameCount()));

    const auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(0u, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.filterDropped);
}

void test_VDLBUS_003_targeted_bad_checksum_rx_is_dropped_without_dl_frames() {
    // A corrupted frame is answered with the single-character NAK at the
    // MAC/ISR level (error downgrade of the armed short-acknowledge). The
    // data link layer only counts the decode failure — it must not transmit
    // anything itself.
    driver->clearSentFrames();

    auto raw = encodeTp1(makeInboundIndividualWrite(kOwnAddress, 0x63u, true));
    TEST_ASSERT_FALSE(raw.empty());
    raw.back() ^= 0xFFu;

    injectRawFrame(raw);

    const size_t delivered = processRxAvailableWithTxOutcome(VirtualTp1PeerResponse::Silent, 0u);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(delivered));
    expectNoDeliveredFrames();

    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(driver->sentFrameCount()));

    const auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(0u, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.decodeFailed);
}

void test_VDLBUS_004_default_address_rx_reaches_network_layer_when_uncommissioned() {
    // An uncommissioned device carries the default individual address
    // 15.15.255; the ETS default-address management procedures (03_05_02)
    // address it directly, so such frames must be delivered. Commissioned
    // devices (own != 15.15.255) must NOT accept them — 03_02_02 §2.3 only
    // makes a device "addressed" when DA equals its own individual address.
    driver->clearSentFrames();
    initTransportStack(initialIndividualAddress());
    captureNextTransportNetworkFrame();

    dlLayer->setOwnAddress(initialIndividualAddress());

    injectRawFrame(encodeTp1(makeInboundIndividualWrite(initialIndividualAddress(), 0x74u, true)));

    const size_t delivered = processRxAvailableWithTxOutcome(VirtualTp1PeerResponse::Silent, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(delivered));
    TEST_ASSERT_TRUE(capturedNetworkFrame.has_value());
    TEST_ASSERT_EQUAL_UINT16(initialIndividualAddress().raw, capturedNetworkFrame->dlFrame.destination.raw);
    TEST_ASSERT_TRUE(capturedNetworkFrame->dlFrame.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL_UINT8(0x74u, capturedNetworkFrame->dlFrame.payload()[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(driver->sentFrameCount()));

    const auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(1u, stats.rxFrames);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.filterDropped);
}

void test_VDLBUS_101_tp1_mac_physical_send_succeeds_on_ack_response() {
    const auto raw = encodeTp1(makeOutboundGroupWrite(GroupAddress(2, 2, 2), 0x71u));

    const auto result = sendRawFrameWithVirtualTxOutcome(raw, VirtualTp1PeerResponse::Ack);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(raw.size()), static_cast<uint32_t>(result.value()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(driver->sentFrameCount()));
}

void test_VDLBUS_102_tp1_mac_physical_send_reports_busy_on_busy_response() {
    const auto raw = encodeTp1(makeOutboundGroupWrite(GroupAddress(2, 2, 3), 0x72u));

    const auto result = sendRawFrameWithVirtualTxOutcome(raw, VirtualTp1PeerResponse::Busy);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Busy), static_cast<int>(result.error()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(driver->sentFrameCount()));
}

void test_VDLBUS_103_tp1_mac_physical_send_times_out_when_peer_is_silent() {
    const auto raw = encodeTp1(makeOutboundGroupWrite(GroupAddress(2, 2, 4), 0x73u));

    const auto result = sendRawFrameWithVirtualTxOutcome(raw, VirtualTp1PeerResponse::Silent);
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Timeout), static_cast<int>(result.error()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(driver->sentFrameCount()));
}

void test_VDLBUS_201_datalink_send_retries_busy_then_succeeds_on_ack() {
    const auto frame = makeOutboundGroupWrite(GroupAddress(3, 3, 1), 0x81u);

    const auto result = sendLDataFrameWithAckSequence(
        frame,
        {VirtualTp1PeerResponse::Busy, VirtualTp1PeerResponse::Ack});

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(driver->sentFrameCount()));

    const auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(1u, stats.txFrames);
}

void test_VDLBUS_202_datalink_send_fails_after_busy_retry_budget_is_exhausted() {
    const auto frame = makeOutboundGroupWrite(GroupAddress(3, 3, 2), 0x82u);

    const auto result = sendLDataFrameWithAckSequence(
        frame,
        {
            VirtualTp1PeerResponse::Busy,
            VirtualTp1PeerResponse::Busy,
            VirtualTp1PeerResponse::Busy,
            VirtualTp1PeerResponse::Busy,
        });

    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Busy), static_cast<int>(result.error()));
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(driver->sentFrameCount()));

    const auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(0u, stats.txFrames);
}

// Busy and "no L_ACK" are different failures and must not share a budget.
// "Busy" is local backpressure — our own driver was still transmitting — while
// a missing L_ACK is a protocol event that 03/02/02 answers with a repetition.
// While the two shared one counter, a frame that had hit busy three times gave
// up on its very first missing L_ACK instead of repeating, which a management
// client sees only as "the device does not respond".
void test_VDLBUS_203_busy_retries_do_not_consume_the_l2_repetition_budget() {
    const auto frame = makeOutboundGroupWrite(GroupAddress(3, 3, 3), 0x83u);

    const auto result = sendLDataFrameWithAckSequence(
        frame,
        {
            // Exhaust the busy budget entirely...
            VirtualTp1PeerResponse::Busy,
            VirtualTp1PeerResponse::Busy,
            VirtualTp1PeerResponse::Busy,
            // ...then the full repetition budget must still be available.
            VirtualTp1PeerResponse::Silent,
            VirtualTp1PeerResponse::Silent,
            VirtualTp1PeerResponse::Silent,
            VirtualTp1PeerResponse::Ack,
        });

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(7u, static_cast<uint32_t>(driver->sentFrameCount()));

    const auto stats = dlLayer->getStatistics();
    TEST_ASSERT_EQUAL_UINT32(1u, stats.txFrames);
    // Only the L_ACK failures count as repetitions; busy retries do not.
    TEST_ASSERT_EQUAL_UINT32(3u, stats.txRepetitions);
}

// 03_02_02 §2.4.2: group frames are acknowledged too, so an unanswered group
// frame is repeated three times. Nobody listening to a group address is a
// legitimate configuration, so the send then counts as done; before, the
// first copy was reported sent without waiting for anything.
void test_VDLBUS_204_unacknowledged_group_frame_is_repeated_then_reported_sent() {
    const auto frame = makeOutboundGroupWrite(GroupAddress(3, 3, 4), 0x84u);

    const auto result = sendLDataFrameWithAckSequence(
        frame,
        {
            VirtualTp1PeerResponse::Silent,
            VirtualTp1PeerResponse::Silent,
            VirtualTp1PeerResponse::Silent,
            VirtualTp1PeerResponse::Silent,
        });

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(driver->sentFrameCount()));
    // Every repetition carries the repeat flag (wire bit 5 cleared).
    for (size_t i = 1; i < 4u; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0u, driver->sentFrame(i)[0] & CTRL_REPEAT);
    }
    TEST_ASSERT_EQUAL_UINT32(3u, dlLayer->getStatistics().txRepetitions);
}

// A NAK means somebody received the group frame and could not read it: that
// still fails once the repetitions are spent.
void test_VDLBUS_205_nakked_group_frame_still_fails_after_repetitions() {
    const auto frame = makeOutboundGroupWrite(GroupAddress(3, 3, 5), 0x85u);

    const auto result = sendLDataFrameWithAckSequence(
        frame,
        {
            VirtualTp1PeerResponse::Nack,
            VirtualTp1PeerResponse::Nack,
            VirtualTp1PeerResponse::Nack,
            VirtualTp1PeerResponse::Nack,
        });

    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::TransmissionFailed), static_cast<int>(result.error()));
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(driver->sentFrameCount()));
}

// Only group frames get that leniency: an individual frame nobody answers is
// a failed delivery.
void test_VDLBUS_206_unacknowledged_individual_frame_still_times_out() {
    LDataFrame frame = makeOutboundGroupWrite(GroupAddress(3, 3, 6), 0x86u);
    frame.setDestination(kRemoteAddress);

    const auto result = sendLDataFrameWithAckSequence(
        frame,
        {
            VirtualTp1PeerResponse::Silent,
            VirtualTp1PeerResponse::Silent,
            VirtualTp1PeerResponse::Silent,
            VirtualTp1PeerResponse::Silent,
        });

    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::Timeout), static_cast<int>(result.error()));
}

// close() must disconnect the interrupt's wake-up before it deletes the task
// that wake-up notifies; the reverse order let an edge in between hand a freed
// task to vTaskNotifyGiveFromISR.
void test_VDLBUS_207_close_clears_isr_notify_before_deleting_rx_task() {
    dlLayer->close();
    dlLayer.reset();

    std::vector<std::string> log;
    TeardownOrderPlatform platform(runtime.platform(), log);
    driver->setCallLog(&log);

    Tp1DataLinkConfig config = Tp1DataLinkConfig::defaults();
    config.enableRxTask = true;
    auto layer = std::make_unique<Tp1DataLinkLayer>(platform, *macPhysical, config);
    TEST_ASSERT_TRUE(layer->init(kOwnAddress).isOk());
    log.clear();

    layer->close();
    driver->setCallLog(nullptr);

    const auto cleared = std::find(log.begin(), log.end(), std::string("notify-cleared"));
    const auto deleted = std::find(log.begin(), log.end(), std::string("deleteTask"));
    TEST_ASSERT_TRUE(cleared != log.end());
    TEST_ASSERT_TRUE(deleted != log.end());
    TEST_ASSERT_TRUE(cleared < deleted);
}

void test_VDLBUS_301_transport_matching_nak_retransmits_same_sequence_over_virtual_bus() {
    initTransportStack();

    const IndividualAddress remote(1, 1, 20);
    auto connRes = connectTransportWithLowLevelAck(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex connIdx = connRes.value();

    tpLayer->processRetransmissions(1000u);
    injectInboundLDataFrame(makeInboundTransportControlFrame(
        remote,
        protocol::TPCIField::create(protocol::TPCIControl::Connect)));
    // The inbound T_Connect is answered with an echoed T_Connect response
    // (03.03.04 §5.1 Table 5 E00/A1) — drive its DL-ACK so the driver
    // returns to idle before the next send.
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(processRxAvailableWithTxOutcome(VirtualTp1PeerResponse::Ack, 1u)));
    TEST_ASSERT_TRUE(tpLayer->isConnected(connIdx));

    driver->clearSentFrames();

    TEST_ASSERT_TRUE(sendConnectedDataWithLowLevelAck(connIdx, {0x91u}).isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(driver->sentFrameCount()));

    const auto firstTx = decodeTp1OrFail(driver->sentFrame(0u));
    const auto firstHeader = protocol::unpackTpduHeader(firstTx.tpdu[0], firstTx.tpdu[1]);
    const uint8_t seq = firstHeader.tpci.seqNum();

    injectInboundLDataFrame(makeInboundTransportControlFrame(remote, protocol::TPCIField::nak(seq)));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(processRxAvailableWithTxOutcome(VirtualTp1PeerResponse::Silent, 0u)));

    processRetransmissionsWithLowLevelAck(5000u);

    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(driver->sentFrameCount()));
    const auto secondTx = decodeTp1OrFail(driver->sentFrame(1u));
    const auto secondHeader = protocol::unpackTpduHeader(secondTx.tpdu[0], secondTx.tpdu[1]);
    TEST_ASSERT_EQUAL(protocol::TPCI::NumberedData, secondHeader.tpci.type());
    TEST_ASSERT_EQUAL_UINT8(seq, secondHeader.tpci.seqNum());
}

void test_VDLBUS_302_transport_matching_ack_prevents_timeout_retransmit_over_virtual_bus() {
    initTransportStack();

    const IndividualAddress remote(1, 1, 21);
    auto connRes = connectTransportWithLowLevelAck(remote);
    TEST_ASSERT_TRUE(connRes.isOk());
    const ConnectionIndex connIdx = connRes.value();

    tpLayer->processRetransmissions(1000u);
    injectInboundLDataFrame(makeInboundTransportControlFrame(
        remote,
        protocol::TPCIField::create(protocol::TPCIControl::Connect)));
    // The inbound T_Connect is answered with an echoed T_Connect response
    // (03.03.04 §5.1 Table 5 E00/A1) — drive its DL-ACK so the driver
    // returns to idle before the next send.
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(processRxAvailableWithTxOutcome(VirtualTp1PeerResponse::Ack, 1u)));
    TEST_ASSERT_TRUE(tpLayer->isConnected(connIdx));

    driver->clearSentFrames();

    TEST_ASSERT_TRUE(sendConnectedDataWithLowLevelAck(connIdx, {0x92u}).isOk());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(driver->sentFrameCount()));

    const auto firstTx = decodeTp1OrFail(driver->sentFrame(0u));
    const auto firstHeader = protocol::unpackTpduHeader(firstTx.tpdu[0], firstTx.tpdu[1]);
    const uint8_t seq = firstHeader.tpci.seqNum();

    injectInboundLDataFrame(makeInboundTransportControlFrame(remote, protocol::TPCIField::ack(seq)));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(processRxAvailableWithTxOutcome(VirtualTp1PeerResponse::Silent, 0u)));
    // Past the 3 s acknowledge timeout but inside the 6 s connection timeout,
    // which since E16 closes an idle connection with T_Disconnect.
    tpLayer->processRetransmissions(1000u + constants::timing::ACK_TIMEOUT_MS + 1000u);

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(driver->sentFrameCount()));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_VDLBUS_001_targeted_individual_rx_is_delivered_without_dl_frames);
    RUN_TEST(test_VDLBUS_002_non_targeted_individual_rx_is_filtered_without_ack);
    RUN_TEST(test_VDLBUS_003_targeted_bad_checksum_rx_is_dropped_without_dl_frames);
    RUN_TEST(test_VDLBUS_004_default_address_rx_reaches_network_layer_when_uncommissioned);
    RUN_TEST(test_VDLBUS_101_tp1_mac_physical_send_succeeds_on_ack_response);
    RUN_TEST(test_VDLBUS_102_tp1_mac_physical_send_reports_busy_on_busy_response);
    RUN_TEST(test_VDLBUS_103_tp1_mac_physical_send_times_out_when_peer_is_silent);
    RUN_TEST(test_VDLBUS_201_datalink_send_retries_busy_then_succeeds_on_ack);
    RUN_TEST(test_VDLBUS_202_datalink_send_fails_after_busy_retry_budget_is_exhausted);
    RUN_TEST(test_VDLBUS_203_busy_retries_do_not_consume_the_l2_repetition_budget);
    RUN_TEST(test_VDLBUS_204_unacknowledged_group_frame_is_repeated_then_reported_sent);
    RUN_TEST(test_VDLBUS_205_nakked_group_frame_still_fails_after_repetitions);
    RUN_TEST(test_VDLBUS_206_unacknowledged_individual_frame_still_times_out);
    RUN_TEST(test_VDLBUS_207_close_clears_isr_notify_before_deleting_rx_task);
    RUN_TEST(test_VDLBUS_301_transport_matching_nak_retransmits_same_sequence_over_virtual_bus);
    RUN_TEST(test_VDLBUS_302_transport_matching_ack_prevents_timeout_retransmit_over_virtual_bus);
    return UNITY_END();
}