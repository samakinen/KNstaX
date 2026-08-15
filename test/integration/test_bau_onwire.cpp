// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_bau_onwire.cpp
 * @brief BAU end-to-end on-wire golden tests (TP1 bytes ↔ APCI service)
 */

#include "unity.h"

#include "knx/bau/bau.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/types.hpp"
#include "knx/platform/linux_platform.hpp"

#include "../mocks/mock_physical_layer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "knx/objects/object_persistence.hpp"

using namespace knx;
using namespace knx::test;

static MockPhysicalLayer* physicalLayer;
static std::unique_ptr<knx::platform::LinuxPlatform> platformInstance;
static std::unique_ptr<knx::bau::BusAccessUnit> bauInstance;

static std::mutex gCbMutex;
static bool gotWrite{false};
static bool gotRead{false};
static knx::GroupObjectIndex lastIdx;
static std::vector<uint8_t> lastData;

static void clearPersistenceStore() {
    std::error_code ec;
    (void)std::filesystem::remove_all(knx::objects::persistenceNamespaceDir("knx_objects"), ec);
}

void setUp(void) {
    clearPersistenceStore();

    platformInstance = std::make_unique<knx::platform::LinuxPlatform>();
    auto ownedPhysical = std::make_unique<MockPhysicalLayer>();
    physicalLayer = ownedPhysical.get();
    auto stackPort = knx::test::createTp1TestStackPort(*platformInstance, std::move(ownedPhysical));
    bauInstance = std::make_unique<knx::bau::BusAccessUnit>(*platformInstance, std::move(stackPort));

    {
        std::lock_guard<std::mutex> lk(gCbMutex);
        gotWrite = false;
        gotRead = false;
        lastIdx = knx::GroupObjectIndex::invalid();
        lastData.clear();
    }

    bauInstance->setGroupObjectWriteCallback([](knx::GroupObjectIndex idx, std::span<const uint8_t> data) {
        {
            std::lock_guard<std::mutex> lk(gCbMutex);
            lastIdx = idx;
            lastData.assign(data.begin(), data.end());
            gotWrite = true;
        }
    });

    bauInstance->setGroupObjectReadCallback([](knx::GroupObjectIndex idx) {
        {
            std::lock_guard<std::mutex> lk(gCbMutex);
            lastIdx = idx;
            lastData.clear();
            gotRead = true;
        }
    });

    TEST_ASSERT_TRUE(bauInstance->init(IndividualAddress(1, 1, 10)).isOk());
    TEST_ASSERT_TRUE(bauInstance->link().hasStackPort());

    // Keep RX deterministic in tests; accept injected frames regardless of filter table.
    bauInstance->link().setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);
}

void tearDown(void) {
    if (bauInstance) {
        bauInstance->close();
        bauInstance.reset();
    }
    physicalLayer = nullptr;
    platformInstance.reset();
}

static std::vector<uint8_t> withChecksum(std::vector<uint8_t> frameNoChecksum) {
    const uint8_t checksum = knx::datalink::FrameCodec::calculateChecksum(
        frameNoChecksum);
    frameNoChecksum.push_back(checksum);
    return frameNoChecksum;
}

namespace {

class RecordingTp1Backend final : public knx::physical::Tp1MediumBackend {
public:
    util::Result<void> init(const knx::physical::Tp1MediumConfig& config) override {
        _config = config;
        _state = knx::physical::Tp1MediumState::Idle;
        return util::Result<void>::ok();
    }

    void close() override {
        _state = knx::physical::Tp1MediumState::Uninitialized;
    }

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) override {
        _lastSentFrame.assign(frame.begin(), frame.end());
        return frame.size();
    }

    void setEventCallback(knx::physical::Tp1EventCallback callback, void* context) override {
        _callback = std::move(callback);
        _callbackContext = context;
    }

    knx::physical::Tp1MediumState getState() const override {
        return _state;
    }

    knx::physical::Tp1CapabilityProfile getCapabilities() const override {
        return {};
    }

    util::Result<void> setBusMonitorMode(bool enabled) override {
        _config.busMonitorMode = enabled;
        return util::Result<void>::ok();
    }

    util::Result<void> service() override {
        return util::Result<void>::ok();
    }

    const std::vector<uint8_t>& lastSentFrame() const {
        return _lastSentFrame;
    }

private:
    knx::physical::Tp1MediumConfig _config{};
    knx::physical::Tp1MediumState _state{knx::physical::Tp1MediumState::Uninitialized};
    knx::physical::Tp1EventCallback _callback{};
    void* _callbackContext{nullptr};
    std::vector<uint8_t> _lastSentFrame{};
};

} // namespace


static void waitFlag(bool& flag, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(gCbMutex);
            if (flag) {
                return;
            }
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return;
        }

        bauInstance->loop();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Read callbacks and the follow-on auto response are delivered from
// bauInstance->loop(), so poll the owner loop until the outcome is visible.
static void waitForOutcomeCount(size_t expected, int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (bauInstance->transmission().queuedOutcomeCount() < expected) {
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        bauInstance->loop();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

static void waitForOwnerWorkHint(size_t expectedLoopWork,
                                 size_t expectedDeferredWork,
                                 bool pumpLoop = false,
                                 int timeoutMs = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto hint = bauInstance->ownerWorkHint();
        if (hint.pendingLoopWorkItems == expectedLoopWork
            && hint.pendingDeferredWorkItems == expectedDeferredWork) {
            return;
        }

        if (std::chrono::steady_clock::now() >= deadline) {
            return;
        }

        if (pumpLoop) {
            bauInstance->loop();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void test_bau_send_group_value_read_encodes_exact_tp1_bytes(void) {
    const GroupAddress dest(0, 1, 0); // raw 0x0100, valid
    TEST_ASSERT_TRUE(dest.isValid());

    TEST_ASSERT_TRUE(bauInstance->requestGroupValue(dest).isOk());

    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));

    // Expected TP1 standard frame for A_GroupValue_Read to group 0/1/0.
    // ctrl 0xBC: L_Data_Standard, not repeated, low priority (03_02_02 §2.3.2)
    const std::vector<uint8_t> expected = withChecksum({
        0xBC,       // ctrl: standard, not repeated, low priority
        0x11, 0x0A, // src 1.1.10
        0x01, 0x00, // dest group 0/1/0
        0xE1,       // group + hop=6 + (tpduLen-1)=1
        0x00, 0x00  // TPDU: GroupValueRead
    });


    TEST_ASSERT_EQUAL_UINT32(expected.size(), sent.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sent[i]);
    }
}

void test_bau_send_group_value_write_encodes_exact_tp1_bytes_short_apdu(void) {
    const GroupAddress dest(0, 1, 0);
    TEST_ASSERT_TRUE(dest.isValid());

    TEST_ASSERT_TRUE(bauInstance->sendGroupValue(dest, std::vector<uint8_t>{0x01}).isOk());

    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));

    // Expected TP1 standard frame for A_GroupValue_Write with short APDU (data6=1).
    const std::vector<uint8_t> expected = withChecksum({
        0xBC,       // ctrl: standard, not repeated, low priority
        0x11, 0x0A, // src 1.1.10
        0x01, 0x00, // dest group 0/1/0
        0xE1,       // group + hop=6 + (tpduLen-1)=1
        0x00, 0x81  // TPDU: GroupValueWrite (data6=1)
    });


    TEST_ASSERT_EQUAL_UINT32(expected.size(), sent.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sent[i]);
    }
}

void test_bau_backend_construction_path_encodes_exact_tp1_bytes(void) {
    auto typedPlatform = std::make_unique<knx::platform::LinuxPlatform>();
    auto backend = std::make_unique<RecordingTp1Backend>();
    auto* backendPtr = backend.get();
    knx::bau::BusAccessUnit typedBau(*typedPlatform, std::move(backend));

    const GroupAddress dest(0, 1, 0);
    TEST_ASSERT_TRUE(dest.isValid());
    TEST_ASSERT_TRUE(typedBau.init(IndividualAddress(1, 1, 10)).isOk());

    TEST_ASSERT_TRUE(typedBau.requestGroupValue(dest).isOk());

    const std::vector<uint8_t> expected = withChecksum({
        0xBC,
        0x11, 0x0A,
        0x01, 0x00,
        0xE1,
        0x00, 0x00
    });

    const auto& sent = backendPtr->lastSentFrame();
    TEST_ASSERT_EQUAL_UINT32(expected.size(), sent.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sent[i]);
    }

    typedBau.close();
}

void test_bau_stack_port_construction_path_encodes_exact_tp1_bytes(void) {
    auto typedPlatform = std::make_unique<knx::platform::LinuxPlatform>();
    auto backend = std::make_unique<RecordingTp1Backend>();
    auto* backendPtr = backend.get();
    auto stackPort = knx::bau::createTp1StackPort(*typedPlatform, std::move(backend));

    TEST_ASSERT_NOT_NULL(stackPort.get());

    knx::bau::BusAccessUnit typedBau(*typedPlatform, std::move(stackPort));

    const GroupAddress dest(0, 1, 0);
    TEST_ASSERT_TRUE(dest.isValid());
    TEST_ASSERT_TRUE(typedBau.init(IndividualAddress(1, 1, 10)).isOk());

    TEST_ASSERT_TRUE(typedBau.requestGroupValue(dest).isOk());

    const std::vector<uint8_t> expected = withChecksum({
        0xBC,
        0x11, 0x0A,
        0x01, 0x00,
        0xE1,
        0x00, 0x00
    });

    const auto& sent = backendPtr->lastSentFrame();
    TEST_ASSERT_EQUAL_UINT32(expected.size(), sent.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], sent[i]);
    }

    typedBau.close();
}

void test_bau_begin_request_group_value_progresses_to_success(void) {
    const GroupAddress dest(0, 1, 0);
    TEST_ASSERT_TRUE(dest.isValid());

    TEST_ASSERT_TRUE(bauInstance->beginRequestGroupValue(dest).isOk());

    auto progress = bauInstance->transmission().poll();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(knx::bau::BusAccessUnit::TransmissionProgressState::Success),
                      static_cast<int>(progress.value()));

    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));
    TEST_ASSERT_FALSE(sent.empty());
}

void test_bau_restore_does_not_clobber_persisted_individual_address(void) {
    clearPersistenceStore();

    {
        auto firstPlatform = std::make_unique<knx::platform::LinuxPlatform>();
        auto firstPhysicalOwned = std::make_unique<MockPhysicalLayer>();
        auto firstStackPort = knx::test::createTp1TestStackPort(*firstPlatform, std::move(firstPhysicalOwned));
        auto firstBau = std::make_unique<knx::bau::BusAccessUnit>(*firstPlatform, std::move(firstStackPort));

        TEST_ASSERT_TRUE(firstBau->init(IndividualAddress(15, 15, 255)).isOk());
        firstBau->deviceObject().setProgModeSilent(Toggle::Enable);

        const std::vector<uint8_t> subnet{0x11};
        const std::vector<uint8_t> device{0x03};
        TEST_ASSERT_TRUE(firstBau->writeProperty(InterfaceObjectType::device(),
                                                 InterfaceObjectInstance(1),
                                                 static_cast<application::PropertyID>(objects::DeviceProperty::SubnetAddress),
                                                 1,
                                                 subnet)
                             .isOk());
        TEST_ASSERT_TRUE(firstBau->writeProperty(InterfaceObjectType::device(),
                                                 InterfaceObjectInstance(1),
                                                 static_cast<application::PropertyID>(objects::DeviceProperty::DeviceAddress),
                                                 1,
                                                 device)
                             .isOk());
        TEST_ASSERT_EQUAL_HEX16(0x1103, firstBau->deviceObject().readIndividualAddress().raw);
    }

    auto secondPlatform = std::make_unique<knx::platform::LinuxPlatform>();
    auto secondPhysicalOwned = std::make_unique<MockPhysicalLayer>();
    auto* secondPhysical = secondPhysicalOwned.get();
    auto secondStackPort = knx::test::createTp1TestStackPort(*secondPlatform, std::move(secondPhysicalOwned));
    auto secondBau = std::make_unique<knx::bau::BusAccessUnit>(*secondPlatform, std::move(secondStackPort));

    TEST_ASSERT_TRUE(secondBau->init(IndividualAddress(15, 15, 255)).isOk());
    TEST_ASSERT_EQUAL_HEX16(0x1103, secondBau->deviceObject().readIndividualAddress().raw);

    const GroupAddress dest(0, 1, 0);
    TEST_ASSERT_TRUE(dest.isValid());
    TEST_ASSERT_TRUE(secondBau->sendGroupValue(dest, std::vector<uint8_t>{0x01}).isOk());

    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(secondPhysical->getSentFrame(sent));
    TEST_ASSERT_TRUE(sent.size() >= 3u);
    TEST_ASSERT_EQUAL_HEX8(0x11, sent[1]);
    TEST_ASSERT_EQUAL_HEX8(0x03, sent[2]);
}

void test_bau_incoming_group_value_write_decodes_to_service_and_payload(void) {
    {
        std::lock_guard<std::mutex> lk(gCbMutex);
        gotWrite = false;
        lastData.clear();
    }

    // Register the group object so the object-index write callback fires.
    const knx::GroupObjectIndex writeIdx = bauInstance->addGroupObject(GroupAddress(0, 1, 0),
                                                                        application::dptids::Bool,
                                                                        false,
                                                                        true,
                                                                        false,
                                                                        true);
    TEST_ASSERT_TRUE(writeIdx.isValid());

    // Inject an on-wire TP1 group write (no ACK requested to keep output deterministic).
    const std::vector<uint8_t> incoming = withChecksum({
        0x88,       // ctrl: standard + Normal priority, no ACK
        0x11, 0x0B, // src 1.1.11
        0x01, 0x00, // dest group 0/1/0
        0xE1,       // group + hop=6 + (tpduLen-1)=1
        0x00, 0x82  // TPDU: GroupValueWrite (data6=2)
    });

    physicalLayer->injectFrame(incoming);
    waitFlag(gotWrite);

    TEST_ASSERT_TRUE(gotWrite);
    TEST_ASSERT_EQUAL_UINT8(writeIdx.value(), lastIdx.value());
    TEST_ASSERT_EQUAL_UINT32(1, lastData.size());
    TEST_ASSERT_EQUAL_HEX8(0x02, lastData[0]);
}

// A communication object the project left unlinked (no association entry) has
// nowhere to send. That is a configuration choice, not a fault: the send must
// succeed as a no-op and put nothing on the wire. Regression for the publish
// path reporting NotInitialized on every refresh of an unlinked object.
void test_bau_send_for_unlinked_object_is_silent_no_op(void) {
    // Register straight into the object table, as the ETS-commissioned runtime
    // does: the object exists but no address/association entry refers to it
    // until a download (or bindGroupObjectToAddress) links one.
    application::GroupObjectConfig config{};
    config.dpt = application::dptids::Bool;
    config.flags.communication = true;
    config.flags.read = true;
    config.flags.transmit = true;
    const knx::GroupObjectIndex idx = bauInstance->groupObjectTable().addGroupObject(
        std::make_unique<application::GroupObject>(config));
    TEST_ASSERT_TRUE(idx.isValid());
    TEST_ASSERT_FALSE(bauInstance->isGroupObjectLinked(idx));
    TEST_ASSERT_EQUAL_UINT32(0u, bauInstance->groupObjectAssociationCount(idx));

    TEST_ASSERT_TRUE(bauInstance->setGroupObjectValue(idx, application::DptValue(true)).isOk());

    const std::vector<uint8_t> payload{0x01};
    TEST_ASSERT_TRUE(bauInstance->sendGroupValueForObject(idx, payload).isOk());
    TEST_ASSERT_TRUE(bauInstance->respondGroupValueForObject(idx, payload).isOk());

    std::vector<uint8_t> sent;
    TEST_ASSERT_FALSE(physicalLayer->getSentFrame(sent));

    // Binding an address makes the same object transmit for real.
    const GroupAddress dest(0, 2, 7);
    TEST_ASSERT_TRUE(bauInstance->bindGroupObjectToAddress(idx, dest).isOk());
    TEST_ASSERT_TRUE(bauInstance->isGroupObjectLinked(idx));
    TEST_ASSERT_TRUE(bauInstance->sendGroupValueForObject(idx, payload).isOk());
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));
}

void test_bau_incoming_group_value_read_triggers_group_value_response_onwire(void) {
    {
        std::lock_guard<std::mutex> lk(gCbMutex);
        gotRead = false;
    }

    const GroupAddress dest(0, 1, 0);
    TEST_ASSERT_TRUE(dest.isValid());

    // Add a group object with a known value so BAU can respond.
    const knx::GroupObjectIndex idx = bauInstance->addGroupObject(dest,
                                                                  application::dptids::Bool,
                                                                  true,
                                                                  true,
                                                                  true,
                                                                  true);
    TEST_ASSERT_TRUE(idx.isValid());

    TEST_ASSERT_TRUE(bauInstance->setGroupObjectValue(idx, application::DptValue(true)).isOk());

    TEST_ASSERT_TRUE(bauInstance->isGroupObjectValid(idx));

    // Inject an on-wire TP1 group read (no ACK requested).
    const std::vector<uint8_t> incomingRead = withChecksum({
        0x88,       // ctrl: standard + Normal priority, no ACK
        0x11, 0x0B, // src 1.1.11
        0x01, 0x00, // dest group 0/1/0
        0xE1,       // group + hop=6 + (tpduLen-1)=1
        0x00, 0x00  // TPDU: GroupValueRead
    });

    physicalLayer->injectFrame(incomingRead);
    waitFlag(gotRead);

    TEST_ASSERT_TRUE(gotRead);

    // BAU should respond with a GroupValueResponse short APDU (data6=1).
    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));

    const std::vector<uint8_t> expectedResponse = withChecksum({
        0xBC,       // ctrl: standard, not repeated, low priority: standard + Low priority + ACK requested
        0x11, 0x0A, // src 1.1.10
        0x01, 0x00, // dest group 0/1/0
        0xE1,       // group + hop=6 + (tpduLen-1)=1
        0x00, 0x41  // TPDU: GroupValueResponse (data6=1)
    });


    TEST_ASSERT_EQUAL_UINT32(expectedResponse.size(), sent.size());
    for (size_t i = 0; i < expectedResponse.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expectedResponse[i], sent[i]);
    }
}

void test_bau_auto_group_value_response_queues_retry_aware_outcome(void) {
    {
        std::lock_guard<std::mutex> lk(gCbMutex);
        gotRead = false;
    }

    const GroupAddress dest(0, 1, 1);
    TEST_ASSERT_TRUE(dest.isValid());

    const knx::GroupObjectIndex idx = bauInstance->addGroupObject(dest,
                                                                  application::dptids::Bool,
                                                                  true,
                                                                  true,
                                                                  true,
                                                                  true);
    TEST_ASSERT_TRUE(idx.isValid());

    TEST_ASSERT_TRUE(bauInstance->setGroupObjectValue(idx, application::DptValue(true)).isOk());

    bauInstance->transmission().setDefaultOptions(
        knx::bau::BusAccessUnit::TransmissionOptions{2u, true, true, false});

    physicalLayer->queueSendResult(knx::util::ErrorCode::Timeout);
    physicalLayer->queueSendResult(knx::util::ErrorCode::Success);

    const std::vector<uint8_t> incomingRead = withChecksum({
        0x88,
        0x11, 0x0B,
        0x01, 0x01,
        0xE1,
        0x00, 0x00
    });

    physicalLayer->injectFrame(incomingRead);
    waitFlag(gotRead);

    TEST_ASSERT_TRUE(gotRead);
    waitForOutcomeCount(1);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(bauInstance->transmission().queuedOutcomeCount()));

    knx::bau::BusAccessUnit::TransmissionOutcome outcome;
    TEST_ASSERT_TRUE(bauInstance->transmission().popOutcome(outcome));
    TEST_ASSERT_EQUAL_UINT16(dest.raw, outcome.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<int>(knx::bau::BusAccessUnit::MessageKind::GroupValueResponse), static_cast<int>(outcome.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_bau_deferred_auto_group_value_response_requires_explicit_progression(void) {
    {
        std::lock_guard<std::mutex> lk(gCbMutex);
        gotRead = false;
    }

    const GroupAddress dest(0, 1, 2);
    TEST_ASSERT_TRUE(dest.isValid());

    const knx::GroupObjectIndex idx = bauInstance->addGroupObject(dest,
                                                                  application::dptids::Bool,
                                                                  true,
                                                                  true,
                                                                  true,
                                                                  true);
    TEST_ASSERT_TRUE(idx.isValid());

    TEST_ASSERT_TRUE(bauInstance->setGroupObjectValue(idx, application::DptValue(true)).isOk());

    bauInstance->transmission().setAutoResponseMode(knx::bau::BusAccessUnit::AutoResponseMode::Deferred);

    const std::vector<uint8_t> incomingRead = withChecksum({
        0x88,
        0x11, 0x0B,
        0x01, 0x02,
        0xE1,
        0x00, 0x00
    });

    physicalLayer->injectFrame(incomingRead);
    waitFlag(gotRead);

    TEST_ASSERT_TRUE(gotRead);
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(bauInstance->transmission().queuedAutomaticResponseCount()));

    std::vector<uint8_t> sent;
    TEST_ASSERT_FALSE(physicalLayer->getSentFrame(sent));

    TEST_ASSERT_TRUE(bauInstance->transmission().beginAutomaticResponse().isOk());
    auto progress = bauInstance->transmission().pollAutomaticResponse();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(knx::bau::BusAccessUnit::TransmissionProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));
    TEST_ASSERT_FALSE(sent.empty());
}

void test_bau_deferred_auto_group_value_response_drops_newest_when_full(void) {
    bauInstance->transmission().setAutoResponseMode(knx::bau::BusAccessUnit::AutoResponseMode::Deferred);

    struct Entry {
        GroupAddress address;
        bool value;
    };

    const std::array<Entry, 5> entries{{
        {GroupAddress(0, 1, 10), false},
        {GroupAddress(0, 1, 11), true},
        {GroupAddress(0, 1, 12), false},
        {GroupAddress(0, 1, 13), true},
        {GroupAddress(0, 1, 14), false},
    }};

    for (const auto& entry : entries) {
        const auto idx = bauInstance->addGroupObject(entry.address,
                                                     application::dptids::Bool,
                                                     true,
                                                     true,
                                                     true,
                                                     true);
        TEST_ASSERT_TRUE(idx.isValid());
        TEST_ASSERT_TRUE(bauInstance->setGroupObjectValue(idx, application::DptValue(entry.value)).isOk());

        {
            std::lock_guard<std::mutex> lk(gCbMutex);
            gotRead = false;
        }

        const std::vector<uint8_t> incomingRead = withChecksum({
            0x88,
            0x11, 0x0B,
            static_cast<uint8_t>((entry.address.raw >> 8) & 0xFFu),
            static_cast<uint8_t>(entry.address.raw & 0xFFu),
            0xE1,
            0x00, 0x00
        });

        physicalLayer->injectFrame(incomingRead);
        waitFlag(gotRead);
        TEST_ASSERT_TRUE(gotRead);
    }

    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(bauInstance->transmission().queuedAutomaticResponseCount()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(bauInstance->transmission().droppedAutomaticResponseCount()));

    std::vector<uint8_t> sent;
    TEST_ASSERT_FALSE(physicalLayer->getSentFrame(sent));

    TEST_ASSERT_TRUE(bauInstance->transmission().beginAutomaticResponse().isOk());
    auto progress = bauInstance->transmission().pollAutomaticResponse();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(knx::bau::BusAccessUnit::TransmissionProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_TRUE(physicalLayer->getSentFrame(sent));
    TEST_ASSERT_TRUE(sent.size() >= 5u);
    TEST_ASSERT_EQUAL_HEX8(0x01u, sent[3]);
    TEST_ASSERT_EQUAL_HEX8(0x0Au, sent[4]);
}

void test_bau_owner_work_hint_tracks_inbound_loop_work_and_deferred_responses(void) {
    std::atomic<unsigned int> workAvailableCount{0u};
    bauInstance->setWorkAvailableCallback([&workAvailableCount]() {
        workAvailableCount.fetch_add(1u, std::memory_order_relaxed);
    });

    const GroupAddress writeDest(0, 1, 20);
    const auto writeIdx = bauInstance->addGroupObject(writeDest,
                                                      application::dptids::Bool,
                                                      false,
                                                      true,
                                                      false,
                                                      true);
    TEST_ASSERT_TRUE(writeIdx.isValid());

    const std::vector<uint8_t> incomingWriteA = withChecksum({
        0x88,
        0x11, 0x0B,
        0x01, 0x14,
        0xE1,
        0x00, 0x81
    });
    const std::vector<uint8_t> incomingWriteB = withChecksum({
        0x88,
        0x11, 0x0B,
        0x01, 0x14,
        0xE1,
        0x00, 0x82
    });

    physicalLayer->injectFrame(incomingWriteA);

    waitForOwnerWorkHint(1u, 0u);

    auto hint = bauInstance->ownerWorkHint();
    TEST_ASSERT_TRUE(hint.hasImmediateWork());
    TEST_ASSERT_TRUE(hint.shouldCallLoop());
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(hint.pendingLoopWorkItems));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(hint.pendingDeferredWorkItems));
    TEST_ASSERT_FALSE(hint.maxSleepMs.has_value());
    TEST_ASSERT_EQUAL_UINT32(1u, workAvailableCount.load(std::memory_order_relaxed));

    physicalLayer->injectFrame(incomingWriteB);

    waitForOwnerWorkHint(2u, 0u);

    hint = bauInstance->ownerWorkHint();
    TEST_ASSERT_EQUAL_UINT32(2u, static_cast<uint32_t>(hint.pendingLoopWorkItems));
    TEST_ASSERT_EQUAL_UINT32(1u, workAvailableCount.load(std::memory_order_relaxed));

    waitForOwnerWorkHint(0u, 0u, true);

    hint = bauInstance->ownerWorkHint();
    TEST_ASSERT_FALSE(hint.hasImmediateWork());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(hint.pendingLoopWorkItems));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(hint.pendingDeferredWorkItems));

    physicalLayer->injectFrame(incomingWriteA);

    waitForOwnerWorkHint(1u, 0u);

    hint = bauInstance->ownerWorkHint();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(hint.pendingLoopWorkItems));
    TEST_ASSERT_EQUAL_UINT32(2u, workAvailableCount.load(std::memory_order_relaxed));

    waitForOwnerWorkHint(0u, 0u, true);

    bauInstance->transmission().setAutoResponseMode(knx::bau::BusAccessUnit::AutoResponseMode::Deferred);

    {
        std::lock_guard<std::mutex> lk(gCbMutex);
        gotRead = false;
    }

    const GroupAddress readDest(0, 1, 21);
    const auto readIdx = bauInstance->addGroupObject(readDest,
                                                     application::dptids::Bool,
                                                     true,
                                                     true,
                                                     true,
                                                     true);
    TEST_ASSERT_TRUE(readIdx.isValid());
    TEST_ASSERT_TRUE(bauInstance->setGroupObjectValue(readIdx, application::DptValue(true)).isOk());

    const std::vector<uint8_t> incomingRead = withChecksum({
        0x88,
        0x11, 0x0B,
        0x01, 0x15,
        0xE1,
        0x00, 0x00
    });

    physicalLayer->injectFrame(incomingRead);
    waitForOwnerWorkHint(1u, 0u);
    TEST_ASSERT_EQUAL_UINT32(3u, workAvailableCount.load(std::memory_order_relaxed));

    waitFlag(gotRead);
    waitForOwnerWorkHint(0u, 1u, true);

    hint = bauInstance->ownerWorkHint();
    TEST_ASSERT_TRUE(hint.hasImmediateWork());
    TEST_ASSERT_FALSE(hint.shouldCallLoop());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(hint.pendingLoopWorkItems));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(hint.pendingDeferredWorkItems));

    TEST_ASSERT_TRUE(bauInstance->transmission().beginAutomaticResponse().isOk());
    auto progress = bauInstance->transmission().pollAutomaticResponse();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(knx::bau::BusAccessUnit::TransmissionProgressState::Success),
                      static_cast<int>(progress.value()));

    waitForOwnerWorkHint(0u, 0u);

    hint = bauInstance->ownerWorkHint();
    TEST_ASSERT_FALSE(hint.hasImmediateWork());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(hint.pendingDeferredWorkItems));
}

// ETS reads PID_SECURITY_MODE of the Security Interface Object with
// A_FunctionPropertyExtState_Read before a secure download.  The request bytes
// below are the ones ETS 6 put on the wire; answering them with
// E_DATA_TYPE_CONFLICT (0xFE) instead of the mode made ETS abort the download
// with "ETS tried to read a protected or a non-existing memory block".
//
// This is a wiring test: PropertyExtServices has always been able to answer,
// but nothing installed the BAU's function provider into it, so the extended
// services rejected every request while the classic ones worked.
void test_bau_function_property_ext_state_read_reports_security_mode_onwire(void) {
    const std::vector<uint8_t> incoming = withChecksum({
        0x88,       // ctrl: standard + Normal priority, no ACK
        0x11, 0x0B, // src 1.1.11
        0x11, 0x0A, // dest 1.1.10 (our own individual address)
        0x68,       // individual + hop=6 + (tpduLen-1)=8
        0x01, 0xD5, // TPDU: T_Data_Individual + APCI 0x1D5 FunctionPropertyExtState_Read
        0x00, 0x11, // interface_object_type = 17 (Security Interface Object)
        0x00, 0x10, // object_instance = 1 (12 bit) + high nibble of property_id
        0x33,       // property_id = 51 (PID_SECURITY_MODE)
        0x00, 0x00  // function input data
    });

    physicalLayer->injectFrame(incoming);

    std::vector<uint8_t> sent;
    for (int attempt = 0; attempt < 200 && !physicalLayer->getSentFrame(sent); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT_FALSE(sent.empty());

    // A_FunctionPropertyExtState_Response: echoed header, then the return code,
    // the echoed ReadServiceID and the state octet (03/05/01 Figure 71).
    // Security is disabled until a tool key is applied.
    TEST_ASSERT_EQUAL_HEX8(0x01, sent[6]);
    TEST_ASSERT_EQUAL_HEX8(0xD6, sent[7]);
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[8]);   // object_type high
    TEST_ASSERT_EQUAL_HEX8(0x11, sent[9]);   // object_type low = 17
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[10]);  // object_instance high
    TEST_ASSERT_EQUAL_HEX8(0x10, sent[11]);  // instance low + property_id high
    TEST_ASSERT_EQUAL_HEX8(0x33, sent[12]);  // property_id low = 51
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[13]);  // return_code = E_SUCCESS, not 0xFE
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[14]);  // ReadServiceID = read security mode
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[15]);  // security mode = disabled
}

// PID_SECURITY_MODE is a Function Property whose data is
// Reserved | ServiceID | ServiceInfo (03/05/01 §6.3.5.1), and §6.3.5 allows the
// command only "using Secure Communication […] Even if Security Mode is
// disabled". An unsecured A_FunctionPropertyExtCommand is therefore refused
// with E_ACCESS_DENIED — otherwise one plain telegram from anywhere on the bus
// would switch this device's Data Secure on or off.
void test_bau_function_property_command_security_mode_refused_when_unsecured(void) {
    TEST_ASSERT_FALSE(bauInstance->securityObject().isSecurityEnabled());

    const std::vector<uint8_t> incoming = withChecksum({
        0x88,       // ctrl: standard + Normal priority, no ACK
        0x11, 0x0B, // src 1.1.11
        0x11, 0x0A, // dest 1.1.10 (our own individual address)
        0x69,       // individual + hop=6 + (tpduLen-1)=9
        0x01, 0xD4, // TPDU: T_Data_Individual + APCI 0x1D4 FunctionPropertyExtCommand
        0x00, 0x11, // interface_object_type = 17 (Security Interface Object)
        0x00, 0x10, // object_instance = 1 (12 bit) + high nibble of property_id
        0x33,       // property_id = 51 (PID_SECURITY_MODE)
        0x00,       // Reserved
        0x00,       // ServiceID = write security mode
        0x01        // ServiceInfo = enable
    });

    physicalLayer->injectFrame(incoming);

    std::vector<uint8_t> sent;
    for (int attempt = 0; attempt < 200 && !physicalLayer->getSentFrame(sent); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT_FALSE(sent.empty());

    // The whole point: the mode did not move.
    TEST_ASSERT_FALSE(bauInstance->securityObject().isSecurityEnabled());

    // §3.4.8.3: a negative return code is answered without a data field.
    TEST_ASSERT_EQUAL_HEX8(0x01, sent[6]);
    TEST_ASSERT_EQUAL_HEX8(0xD6, sent[7]);
    TEST_ASSERT_EQUAL_HEX8(0x33, sent[12]);  // property_id low = 51
    TEST_ASSERT_EQUAL_HEX8(0xFC, sent[13]);  // return_code = E_ACCESS_DENIED
    TEST_ASSERT_EQUAL_UINT32(15u, static_cast<uint32_t>(sent.size()));
}

// A secured S-A_Sync_Request from ETS must be answered on the wire. The device
// answers on the communication mode it was asked on — broadcast here — and a
// broadcast is never acknowledged on TP1: if CTRL bit 1 (ACK request) is set,
// Tp1MacPhysical opens an L_ACK window, waits for an acknowledgement nobody
// sends, repeats the frame three times and reports the send as failed.
void test_bau_secure_sync_request_is_answered_onwire_without_ack_request(void) {
    const std::array<uint8_t, 6> serial{0x00, 0xFA, 0x00, 0x00, 0x00, 0x01};
    bauInstance->deviceObject().setSerialNumber(serial);

    std::array<uint8_t, 16> toolKey{};
    for (size_t i = 0; i < toolKey.size(); ++i) {
        toolKey[i] = static_cast<uint8_t>(i);
    }
    bauInstance->securityObject().setToolKey(toolKey);
    bauInstance->securityObject().setSecurityMode(knx::objects::SecurityMode::Enabled);

    // S-A_Sync_Request from 1.1.254 to the broadcast address, secured with the
    // tool key above and addressed to this device's serial number.
    const std::vector<uint8_t> request = withChecksum({
        0x30,             // ctrl: extended, not repeated, system priority, no ACK request
        0xE0,             // ctrle: group addressed, hop count 6, format 0000
        0x11, 0xFE,       // src 1.1.254
        0x00, 0x00,       // dest: broadcast
        0x18,             // length: 25 TPDU octets - 1
        0x03, 0xF1, 0x92, // Secure APCI, SCF: tool access, CCM A+C, SyncRequest
        0x00, 0x00, 0x00, 0x00, 0x00, 0x07,  // SeqNrlocal
        0x00, 0xFA, 0x00, 0x00, 0x00, 0x01,  // KNX Serial Number
        0xEC, 0x1B, 0xDC, 0xD7, 0x48, 0x4E,  // encrypted challenge
        0xFD, 0xE8, 0x19, 0x52               // MAC
    });

    physicalLayer->injectFrame(request);

    std::vector<uint8_t> sent;
    for (int attempt = 0; attempt < 200 && !physicalLayer->getSentFrame(sent); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT_FALSE(sent.empty());  // an S-A_Sync_Response must reach the wire

    // The response cannot be compared byte for byte: it carries a random value
    // that the device draws itself. Everything around it is fixed.
    TEST_ASSERT_EQUAL_UINT32(33u, static_cast<uint32_t>(sent.size()));
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[0] & 0x80);  // extended frame
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[0] & 0x02);  // ACK request clear
    TEST_ASSERT_EQUAL_HEX8(0x80, sent[1] & 0x80);  // group addressed
    TEST_ASSERT_EQUAL_HEX8(0x11, sent[2]);         // src 1.1.10
    TEST_ASSERT_EQUAL_HEX8(0x0A, sent[3]);
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[4]);         // dest: broadcast
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[5]);
    TEST_ASSERT_EQUAL_HEX8(0x18, sent[6]);         // 25 TPDU octets - 1
    TEST_ASSERT_EQUAL_HEX8(0x03, sent[7]);         // Secure APCI
    TEST_ASSERT_EQUAL_HEX8(0xF1, sent[8]);
    TEST_ASSERT_EQUAL_HEX8(0x93, sent[9]);         // SCF: tool access, SyncResponse

    const uint8_t checksum = knx::datalink::FrameCodec::calculateChecksum(
        std::vector<uint8_t>(sent.begin(), sent.end() - 1));
    TEST_ASSERT_EQUAL_HEX8(checksum, sent.back());
}

// ETS installs its own Tool Key with A_PropertyExtValue_WriteCon, but it does so
// over secured communication: 03/05/01 §6.3.10 gives PID_TOOL_KEY the 00C/00C
// Access Policy, and §1.3 requires a device to protect keys "from any access".
// An unsecured write of the Tool Key is therefore refused — accepting it would
// let anyone on the bus hand themselves the device's tool credentials.
void test_bau_property_ext_write_tool_key_refused_when_unsecured(void) {
    const std::array<uint8_t, 16> newKey{
        0x4E, 0x51, 0xA5, 0xC0, 0xA1, 0xA0, 0x4A, 0xB8,
        0x74, 0xED, 0xDB, 0xC0, 0xF6, 0xF1, 0xE9, 0x7C};
    TEST_ASSERT_TRUE(bauInstance->securityObject().getToolKey() != newKey);

    const std::vector<uint8_t> incoming = withChecksum({
        0x30,       // ctrl: extended, not repeated, system priority, no ACK request
        0x60,       // ctrle: individually addressed, hop count 6, format 0000
        0x11, 0x0B, // src 1.1.11
        0x11, 0x0A, // dest 1.1.10 (our own individual address)
        0x19,       // length: 26 TPDU octets - 1
        0x01, 0xCE, // TPDU: T_Data_Individual + APCI 0x1CE PropertyExtValue_WriteCon
        0x00, 0x11, // interface_object_type = 17 (Security Interface Object)
        0x00, 0x10, // object_instance = 1 (12 bit) + high nibble of property_id
        0x38,       // property_id = 56 (PID_TOOL_KEY)
        0x01,       // element count
        0x00, 0x01, // start index
        0x4E, 0x51, 0xA5, 0xC0, 0xA1, 0xA0, 0x4A, 0xB8,
        0x74, 0xED, 0xDB, 0xC0, 0xF6, 0xF1, 0xE9, 0x7C
    });

    physicalLayer->injectFrame(incoming);

    std::vector<uint8_t> sent;
    for (int attempt = 0; attempt < 200 && !physicalLayer->getSentFrame(sent); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT_FALSE(sent.empty());

    // A_PropertyExtValue_WriteConRes with nr_of_elem = 0 and E_ACCESS_DENIED.
    TEST_ASSERT_EQUAL_HEX8(0x01, sent[6]);
    TEST_ASSERT_EQUAL_HEX8(0xCF, sent[7]);   // A_PropertyExtValue_WriteConRes
    TEST_ASSERT_EQUAL_HEX8(0x38, sent[12]);  // property_id = 56 (PID_TOOL_KEY)
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[13]);  // nr_of_elem = 0
    TEST_ASSERT_EQUAL_HEX8(0xFC, sent[16]);  // return code = E_ACCESS_DENIED

    // And the key the device secures traffic with is untouched.
    TEST_ASSERT_TRUE(bauInstance->securityObject().getToolKey() != newKey);
}

// ETS installs its Tool Key and Sequence Number Sending and then immediately
// master-resets the device. Those writes have to reach NVS before the restart:
// coming back up on the factory key makes the first S-A_Sync_Request after the
// reboot unverifiable, which ETS reports as "no SyncResponse was received;
// probably the key did not match".
//
// The write goes through the local property API rather than an injected
// telegram because a plain telegram no longer reaches this object at all — see
// test_bau_property_ext_write_tool_key_refused_when_unsecured. What is under
// test here is the persistence, not the transport.
void test_bau_security_object_writes_survive_restart(void) {
    clearPersistenceStore();

    const std::array<uint8_t, 16> etsKey{
        0x4E, 0x51, 0xA5, 0xC0, 0xA1, 0xA0, 0x4A, 0xB8,
        0x74, 0xED, 0xDB, 0xC0, 0xF6, 0xF1, 0xE9, 0x7C};

    {
        auto firstPlatform = std::make_unique<knx::platform::LinuxPlatform>();
        auto firstPhysicalOwned = std::make_unique<MockPhysicalLayer>();
        auto firstStackPort = knx::test::createTp1TestStackPort(*firstPlatform, std::move(firstPhysicalOwned));
        auto firstBau = std::make_unique<knx::bau::BusAccessUnit>(*firstPlatform, std::move(firstStackPort));

        TEST_ASSERT_TRUE(firstBau->init(IndividualAddress(1, 1, 3)).isOk());
        firstBau->link().setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);

        TEST_ASSERT_TRUE(firstBau->writeProperty(
            knx::InterfaceObjectType::security(),
            knx::InterfaceObjectInstance(1),
            static_cast<knx::application::PropertyID>(knx::objects::SecurityProperty::ToolKey),
            1,
            std::span<const uint8_t>(etsKey.data(), etsKey.size())).isOk());
        TEST_ASSERT_TRUE(firstBau->securityObject().getToolKey() == etsKey);

        // What the A_Restart handler does before taking the device down.
        firstBau->flushPendingPersistence();
        firstBau->close();
    }

    auto secondPlatform = std::make_unique<knx::platform::LinuxPlatform>();
    auto secondPhysicalOwned = std::make_unique<MockPhysicalLayer>();
    auto secondStackPort = knx::test::createTp1TestStackPort(*secondPlatform, std::move(secondPhysicalOwned));
    auto secondBau = std::make_unique<knx::bau::BusAccessUnit>(*secondPlatform, std::move(secondStackPort));

    TEST_ASSERT_TRUE(secondBau->init(IndividualAddress(1, 1, 3)).isOk());
    TEST_ASSERT_TRUE(secondBau->securityObject().getToolKey() == etsKey);

    secondBau->close();
    clearPersistenceStore();
}

// ETS ends every secure download with a ConfirmedRestart, so a group key table
// that does not survive the reboot is a key table that is never used: the
// device comes back up in Security Mode with no key, sends every group
// telegram in plain, and answers PID_GO_DIAGNOSTICS with E_DATA_VOID.
// 03/05/01 §6.3.7 "Master Reset" is explicit that erase code 01h leaves the
// Group Key Table "not influenced: no change".
//
// The multi-entry table is the point: the generic save path stores element 1
// only, which would restore 1 of 3 keys and look like it worked.
void test_bau_group_key_table_survives_restart(void) {
    clearPersistenceStore();

    const GroupAddress addresses[] = {GroupAddress(1, 1, 2), GroupAddress(1, 1, 3),
                                      GroupAddress(1, 1, 4)};

    std::vector<uint8_t> table;
    for (size_t entry = 0; entry < 3; ++entry) {
        table.push_back(0x00);
        table.push_back(static_cast<uint8_t>(entry + 1));  // GA_Index, 1-based
        for (size_t i = 0; i < 16; ++i) {
            table.push_back(static_cast<uint8_t>(0x10 * (entry + 1) + i));
        }
    }
    TEST_ASSERT_EQUAL_UINT32(54u, table.size());

    {
        auto firstPlatform = std::make_unique<knx::platform::LinuxPlatform>();
        auto firstStackPort = knx::test::createTp1TestStackPort(
            *firstPlatform, std::make_unique<MockPhysicalLayer>());
        auto firstBau = std::make_unique<knx::bau::BusAccessUnit>(*firstPlatform,
                                                                  std::move(firstStackPort));
        TEST_ASSERT_TRUE(firstBau->init(IndividualAddress(1, 1, 3)).isOk());

        for (const auto& address : addresses) {
            TEST_ASSERT_TRUE(firstBau->addGroupObject(address, application::dptids::Bool,
                                                      true, true, true, true).isValid());
        }

        TEST_ASSERT_TRUE(firstBau->writeProperty(
            InterfaceObjectType::security(),
            InterfaceObjectInstance(1),
            static_cast<application::PropertyID>(knx::objects::SecurityProperty::GroupKeyTable),
            1,
            table).isOk());
        TEST_ASSERT_EQUAL_UINT16(3u, firstBau->securityObject().groupKeyCount());

        firstBau->flushPendingPersistence();
        firstBau->close();
    }

    auto secondPlatform = std::make_unique<knx::platform::LinuxPlatform>();
    auto secondStackPort = knx::test::createTp1TestStackPort(
        *secondPlatform, std::make_unique<MockPhysicalLayer>());
    auto secondBau = std::make_unique<knx::bau::BusAccessUnit>(*secondPlatform,
                                                               std::move(secondStackPort));
    TEST_ASSERT_TRUE(secondBau->init(IndividualAddress(1, 1, 3)).isOk());

    // All three keys are back, each against the address its GA_Index named.
    TEST_ASSERT_EQUAL_UINT16(3u, secondBau->securityObject().groupKeyCount());
    for (size_t entry = 0; entry < 3; ++entry) {
        std::array<uint8_t, 16> resolved{};
        TEST_ASSERT_TRUE(
            secondBau->securityObject().getGroupKey(addresses[entry], resolved).isOk());
        for (size_t i = 0; i < resolved.size(); ++i) {
            TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>(0x10 * (entry + 1) + i), resolved[i]);
        }
    }

    secondBau->close();
    clearPersistenceStore();
}

// ETS ends a Data Secure download by driving the Security Interface Object's
// load state machine with A_FunctionPropertyExtCommand on PID_LOAD_STATE_CONTROL
// (it addresses that object by type, not by index) — over secured
// communication. 03/05/01 §6.3.4: "The Load Control shall only be accessible
// using the Role 'Tool' […] with the Tool Access flag set in the S-A_Data-
// service". Unsecured, the load state machine must not move: it gates the key
// tables, so driving it is a way to invalidate them.
void test_bau_function_property_load_state_refused_when_unsecured(void) {
    const uint8_t stateBefore = bauInstance->securityObject().loadState();
    // 17 TPDU octets, so an extended frame — the same shape ETS sends.
    const std::vector<uint8_t> incoming = withChecksum({
        0x30,       // ctrl: extended, not repeated, system priority, no ACK request
        0x60,       // ctrle: individually addressed, hop count 6
        0x11, 0x0B, // src 1.1.11
        0x11, 0x0A, // dest 1.1.10 (our own individual address)
        0x10,       // length: 17 TPDU octets - 1
        0x01, 0xD4, // TPDU: T_Data_Individual + APCI 0x1D4 FunctionPropertyExtCommand
        0x00, 0x11, // interface_object_type = 17 (Security Interface Object)
        0x00, 0x10, // object_instance = 1 + high nibble of property_id
        0x05,       // property_id = 5 (PID_LOAD_STATE_CONTROL)
        0x01,       // load event 1 = StartLoading
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // additional load control data
    });

    physicalLayer->injectFrame(incoming);

    std::vector<uint8_t> sent;
    for (int attempt = 0; attempt < 200 && !physicalLayer->getSentFrame(sent); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    TEST_ASSERT_FALSE(sent.empty());

    TEST_ASSERT_EQUAL_HEX8(0x01, sent[6]);
    TEST_ASSERT_EQUAL_HEX8(0xD6, sent[7]);   // A_FunctionPropertyExtState_Response
    TEST_ASSERT_EQUAL_HEX8(0x05, sent[12]);  // property_id = 5
    TEST_ASSERT_EQUAL_HEX8(0xFC, sent[13]);  // return code E_ACCESS_DENIED

    TEST_ASSERT_EQUAL_UINT8(stateBefore, bauInstance->securityObject().loadState());
}

// ETS cannot put a telegram on a Data Secure group address itself — it has no
// Sequence Number Sending for the group key — so "read this group address" in
// the ETS group monitor becomes PID_GO_DIAGNOSTICS WriteServiceID 03h on the
// Group Object Table Object and the device transmits on ETS' behalf
// (03/05/01 §4.8.1.3.5). The frame below is the one ETS 6 sends, with the
// group address of this fixture substituted.
static std::vector<uint8_t> goDiagnosticsSendReadFrame(uint8_t flags) {
    return withChecksum({
        0x30,       // ctrl: extended, not repeated, system priority, no ACK request
        0x60,       // ctrle: individually addressed, hop count 6
        0x11, 0x0B, // src 1.1.11
        0x11, 0x0A, // dest 1.1.10 (our own individual address)
        0x0B,       // length: 12 TPDU octets - 1
        0x01, 0xD4, // TPDU: T_Data_Individual + APCI 0x1D4 FunctionPropertyExtCommand
        0x00, 0x09, // interface_object_type = 9 (Group Object Table Object)
        0x00, 0x10, // object_instance = 1 + high nibble of property_id
        0x42,       // property_id = 66 (PID_GO_DIAGNOSTICS)
        0x00,       // Reserved
        0x03,       // WriteServiceID 03h - Send A_GroupValue_Read
        flags,      // Flags: b0 authentication, b1 confidentiality
        0x01, 0x00  // Group Address 0/1/0
    });
}

/// Collect up to `limit` frames the device puts on the bus, giving it time to
/// answer. Returns fewer than `limit` only if the device stopped sending.
static std::vector<std::vector<uint8_t>> collectSentFrames(size_t limit) {
    std::vector<std::vector<uint8_t>> frames;
    for (int attempt = 0; attempt < 200 && frames.size() < limit; ++attempt) {
        std::vector<uint8_t> sent;
        if (physicalLayer->getSentFrame(sent)) {
            frames.push_back(std::move(sent));
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return frames;
}

void test_bau_go_diagnostics_send_group_value_read_puts_read_on_bus(void) {
    const GroupAddress dest(0, 1, 0);
    const knx::GroupObjectIndex idx = bauInstance->addGroupObject(dest,
                                                                  application::dptids::Bool,
                                                                  true, true, true, true);
    TEST_ASSERT_TRUE(idx.isValid());

    physicalLayer->injectFrame(goDiagnosticsSendReadFrame(0x00));

    // Two frames: the A_GroupValue_Read the command asked for, then the
    // A_FunctionPropertyExtState_Response that reports the outcome.
    const auto frames = collectSentFrames(2);
    TEST_ASSERT_EQUAL_UINT32(2u, frames.size());

    const auto& read = frames[0];
    TEST_ASSERT_EQUAL_HEX8(0x01, read[3]);   // dest group 0/1/0
    TEST_ASSERT_EQUAL_HEX8(0x00, read[4]);
    TEST_ASSERT_EQUAL_HEX8(0x00, read[6]);   // TPDU: A_GroupValue_Read
    TEST_ASSERT_EQUAL_HEX8(0x00, read[7]);

    const auto& response = frames[1];
    TEST_ASSERT_EQUAL_HEX8(0x01, response[6]);
    TEST_ASSERT_EQUAL_HEX8(0xD6, response[7]);  // A_FunctionPropertyExtState_Response
    TEST_ASSERT_EQUAL_HEX8(0x42, response[12]); // property_id = 66
    TEST_ASSERT_EQUAL_HEX8(0x00, response[13]); // return code E_SUCCESS
    TEST_ASSERT_EQUAL_HEX8(0x03, response[14]); // WriteServiceID echoed back
}

// §4.8.1.3.5 Table 39: E_DATA_VOID covers "Security is requested for GA for
// which there is no GA Key". Sending in plain what the client asked to have
// secured would be worse than refusing.
void test_bau_go_diagnostics_refuses_secure_read_without_group_key(void) {
    const GroupAddress dest(0, 1, 0);
    TEST_ASSERT_TRUE(bauInstance->addGroupObject(dest, application::dptids::Bool,
                                                 true, true, true, true).isValid());

    physicalLayer->injectFrame(goDiagnosticsSendReadFrame(0x03));  // auth + conf

    const auto frames = collectSentFrames(1);
    TEST_ASSERT_EQUAL_UINT32(1u, frames.size());  // no group read went out

    const auto& response = frames[0];
    TEST_ASSERT_EQUAL_HEX8(0xD6, response[7]);
    TEST_ASSERT_EQUAL_HEX8(0x42, response[12]);
    TEST_ASSERT_EQUAL_HEX8(0xF8, response[13]);  // E_DATA_VOID
    TEST_ASSERT_EQUAL_HEX8(0x03, response[14]);
}

// PID_GRP_KEY_TABLE (03/05/01 §6.3.7) arrives as PDT_GENERIC_18[]: GA_Index +
// 16-octet key, where GA_Index is the TSAP — the 1-based index into the Group
// Address Table — and NOT the group address. The property store keeps the array
// verbatim, so unless the BAU resolves it the Secure Application Layer finds no
// key, every group telegram goes out in plain, and PID_GO_DIAGNOSTICS refuses a
// secured send with E_DATA_VOID.
void test_bau_downloaded_group_key_table_becomes_usable_key(void) {
    const GroupAddress dest(0, 1, 0);
    TEST_ASSERT_TRUE(bauInstance->addGroupObject(dest, application::dptids::Bool,
                                                 true, true, true, true).isValid());
    // addGroupObject put the address at TSAP 1.
    TEST_ASSERT_EQUAL_UINT16(1u, bauInstance->addressTable().findIndex(dest).value());

    std::array<uint8_t, 16> expectedKey{};
    for (size_t i = 0; i < expectedKey.size(); ++i) {
        expectedKey[i] = static_cast<uint8_t>(0xA0 + i);
    }

    std::vector<uint8_t> entry{0x00, 0x01};  // GA_Index 1
    entry.insert(entry.end(), expectedKey.begin(), expectedKey.end());

    TEST_ASSERT_TRUE(bauInstance->writeProperty(InterfaceObjectType::security(),
                                                InterfaceObjectInstance(1),
                                                static_cast<application::PropertyID>(53),
                                                1,
                                                entry).isOk());

    std::array<uint8_t, 16> resolved{};
    TEST_ASSERT_TRUE(bauInstance->securityObject().getGroupKey(dest, resolved).isOk());
    for (size_t i = 0; i < expectedKey.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expectedKey[i], resolved[i]);
    }
    TEST_ASSERT_EQUAL_UINT16(1u, bauInstance->securityObject().groupKeyCount());

    // The key must be keyed by group address, not by the raw index octets: a
    // lookup on the address the index stands for is the only one the Secure
    // Application Layer ever performs.
    std::array<uint8_t, 16> unrelated{};
    TEST_ASSERT_FALSE(bauInstance->securityObject()
                          .getGroupKey(GroupAddress(0, 1, 1), unrelated).isOk());
}

// A Data Secure replay window is only worth as much as its memory. 03/05/01
// §6.3.8.4 requires all Last Valid SeqNr values to "be saved in full at
// power-down and be restored in full at power-up", and §6.2 keeps the Sequence
// Number for Tool Access unchanged across a Confirmed Restart — which is the
// restart ETS performs at the end of every secure download. Without this, the
// device comes back accepting every sequence number a partner already used,
// so any telegram captured before the reboot can be replayed once.
void test_bau_security_sequence_numbers_survive_restart(void) {
    clearPersistenceStore();

    const IndividualAddress tool(1, 1, 1);
    const IndividualAddress peer(1, 1, 7);
    constexpr uint64_t kToolSequence = 0x0000AABBCCDD00ull;
    constexpr uint64_t kPeerSequence = 0x0000000000BEEFull;

    // PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE as ETS downloads it: two entries of
    // IA(2) + Last Valid SeqNr(6), with the sequence fields still zero.
    std::vector<uint8_t> table;
    for (const auto& address : {tool, peer}) {
        table.push_back(static_cast<uint8_t>(address.raw >> 8));
        table.push_back(static_cast<uint8_t>(address.raw & 0xFFu));
        table.insert(table.end(), 6, 0x00);
    }

    {
        auto firstPlatform = std::make_unique<knx::platform::LinuxPlatform>();
        auto firstStackPort = knx::test::createTp1TestStackPort(
            *firstPlatform, std::make_unique<MockPhysicalLayer>());
        auto firstBau = std::make_unique<knx::bau::BusAccessUnit>(*firstPlatform,
                                                                  std::move(firstStackPort));
        TEST_ASSERT_TRUE(firstBau->init(IndividualAddress(1, 1, 3)).isOk());

        TEST_ASSERT_TRUE(firstBau->writeProperty(
            InterfaceObjectType::security(),
            InterfaceObjectInstance(1),
            static_cast<application::PropertyID>(
                knx::objects::SecurityProperty::SecurityIndividualAddressTable),
            1,
            table).isOk());

        // Settle the download before the sequence numbers move, so what the
        // second flush stores is the sequence checkpoint alone and not a
        // property write that happened to carry it along.
        firstBau->flushPendingPersistence();

        // What the Secure Application Layer does after verifying a telegram.
        firstBau->securityObject().setPeerSequence(peer, kPeerSequence);
        firstBau->securityObject().setToolAccessSequence(kToolSequence);

        firstBau->flushPendingPersistence();
        firstBau->close();
    }

    auto secondPlatform = std::make_unique<knx::platform::LinuxPlatform>();
    auto secondStackPort = knx::test::createTp1TestStackPort(
        *secondPlatform, std::make_unique<MockPhysicalLayer>());
    auto secondBau = std::make_unique<knx::bau::BusAccessUnit>(*secondPlatform,
                                                               std::move(secondStackPort));
    TEST_ASSERT_TRUE(secondBau->init(IndividualAddress(1, 1, 3)).isOk());

    TEST_ASSERT_TRUE(secondBau->securityObject().getPeerSequence(peer) == kPeerSequence);
    TEST_ASSERT_TRUE(secondBau->securityObject().getToolAccessSequence() == kToolSequence);
    // A partner that never sent anything stays at zero rather than inheriting
    // another partner's window.
    TEST_ASSERT_TRUE(secondBau->securityObject().getPeerSequence(tool) == 0u);

    secondBau->close();
    clearPersistenceStore();
}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_bau_function_property_ext_state_read_reports_security_mode_onwire);
    RUN_TEST(test_bau_function_property_command_security_mode_refused_when_unsecured);
    RUN_TEST(test_bau_function_property_load_state_refused_when_unsecured);
    RUN_TEST(test_bau_downloaded_group_key_table_becomes_usable_key);
    RUN_TEST(test_bau_go_diagnostics_send_group_value_read_puts_read_on_bus);
    RUN_TEST(test_bau_go_diagnostics_refuses_secure_read_without_group_key);
    RUN_TEST(test_bau_property_ext_write_tool_key_refused_when_unsecured);
    RUN_TEST(test_bau_secure_sync_request_is_answered_onwire_without_ack_request);
    RUN_TEST(test_bau_send_group_value_read_encodes_exact_tp1_bytes);
    RUN_TEST(test_bau_send_group_value_write_encodes_exact_tp1_bytes_short_apdu);
    RUN_TEST(test_bau_backend_construction_path_encodes_exact_tp1_bytes);
    RUN_TEST(test_bau_stack_port_construction_path_encodes_exact_tp1_bytes);
    RUN_TEST(test_bau_begin_request_group_value_progresses_to_success);
    RUN_TEST(test_bau_restore_does_not_clobber_persisted_individual_address);
    RUN_TEST(test_bau_security_object_writes_survive_restart);
    RUN_TEST(test_bau_group_key_table_survives_restart);
    RUN_TEST(test_bau_security_sequence_numbers_survive_restart);
    RUN_TEST(test_bau_incoming_group_value_write_decodes_to_service_and_payload);
    RUN_TEST(test_bau_send_for_unlinked_object_is_silent_no_op);
    RUN_TEST(test_bau_incoming_group_value_read_triggers_group_value_response_onwire);
    RUN_TEST(test_bau_auto_group_value_response_queues_retry_aware_outcome);
    RUN_TEST(test_bau_deferred_auto_group_value_response_requires_explicit_progression);
    RUN_TEST(test_bau_deferred_auto_group_value_response_drops_newest_when_full);
    RUN_TEST(test_bau_owner_work_hint_tracks_inbound_loop_work_and_deferred_responses);

    return UNITY_END();
}
