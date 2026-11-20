// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_knx_stack_e2e.cpp
 * @brief End-to-end KNX stack tests with BAU and object-layer integration
 *
 * The device runs on a VirtualPlatform: its data link RX task is a fibre, and
 * the test body is the only thing that makes it run (an injected frame, a
 * clock advance). A request the test injects has been handled, and its answer
 * transmitted to completion, by the time injectFrame() returns.
 *
 * On LinuxPlatform the RX task was a real thread that answered management
 * requests on its own. The test took "the answer is in the mock's sent queue"
 * for "the device has finished sending it", and the RX thread could still be
 * between handing the frame to the physical layer and closing its data link
 * TX operation; the test's next link().sendFrame() then met that operation and
 * failed with Busy ("TX operation already active"). With the CPUs loaded
 * about 1 run in 70 failed that way, in the ASan and the plain build alike:
 * mostly in test_e2e_property_value_write_individual_address_via_properties,
 * where the address change also has the RX thread save to persistence first,
 * and in test_e2e_classic_and_extended_descriptions_agree.
 */

#include "unity.h"
#include "knx/types.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/bau/bau.hpp"
#include "knx/platform/virtual_platform.hpp"
#include "knx/platform/virtual_test_clock.hpp"
#include "knx/objects/device_object.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "../mocks/mock_physical_layer.hpp"
#include <filesystem>
#include <memory>
#include <span>
#include <vector>
#include "knx/objects/object_persistence.hpp"

using namespace knx;
using namespace knx::test;

static MockPhysicalLayer* physicalLayer;
static knx::platform::VirtualTestClock testClock;
static std::unique_ptr<knx::platform::VirtualPlatform> platformInstance;
static std::unique_ptr<bau::BusAccessUnit> bauUnit;
static std::vector<std::pair<knx::GroupObjectIndex, std::vector<uint8_t>>> receivedWrites;
static std::vector<knx::GroupObjectIndex> receivedReads;

static void clearReceivedCallbacks() {
    receivedWrites.clear();
    receivedReads.clear();
}

static void pumpBau(int iterations = 1) {
    for (int index = 0; index < iterations; ++index) {
        bauUnit->loop();
    }
}

/// Run the device for up to @p timeoutMs of virtual time, one millisecond at
/// a time, until @p done. The time is the device's, not the host's: a loaded
/// or instrumented host makes the test slower, never different.
template <typename Done>
static bool runUntil(Done done, uint32_t timeoutMs) {
    for (uint32_t elapsedMs = 0;; ++elapsedMs) {
        pumpBau();
        if (done()) {
            return true;
        }
        if (elapsedMs >= timeoutMs) {
            return false;
        }
        platformInstance->advanceTimeMs(1u);
    }
}

static bool waitForReceivedWrites(size_t expected, uint32_t timeoutMs = 250) {
    return runUntil([&]() { return receivedWrites.size() >= expected; }, timeoutMs);
}

static bool waitForReceivedReads(size_t expected, uint32_t timeoutMs = 250) {
    return runUntil([&]() { return receivedReads.size() >= expected; }, timeoutMs);
}

static bool waitForSentFrame(std::vector<uint8_t>& frame, uint32_t timeoutMs = 250) {
    return runUntil([&]() { return physicalLayer->tryGetSentFrame(frame); }, timeoutMs);
}

static void drainSentFrames() {
    std::vector<uint8_t> tmp;
    while (physicalLayer->tryGetSentFrame(tmp)) {
        // drain
    }
}

static uint8_t ctrlByte(std::span<const uint8_t> frame) {
    return frame.empty() ? 0 : frame[0];
}

static void setTpdu(datalink::LDataFrame& frame, knx::protocol::TPCI tpci, application::APCIField apci, std::span<const uint8_t> payload) {
    frame.setTpdu(protocol::TPCIField::create(tpci), apci, payload);
}

static void setTpdu(datalink::LDataFrame& frame, knx::protocol::TPCI tpci, application::APCIService apci, std::span<const uint8_t> payload) {
    frame.setTpdu(protocol::TPCIField::create(tpci), application::APCIField::create(apci), payload);
}

static void setTpdu(datalink::LDataFrame& frame, knx::protocol::TPCI tpci, application::APCIField apci, std::initializer_list<uint8_t> payload) {
    frame.setTpdu(protocol::TPCIField::create(tpci), apci, payload);
}

static void setTpdu(datalink::LDataFrame& frame, knx::protocol::TPCI tpci, application::APCIService apci, std::initializer_list<uint8_t> payload) {
    frame.setTpdu(protocol::TPCIField::create(tpci), application::APCIField::create(apci), payload);
}

static void clearPersistenceStore() {
    std::error_code ec;
    (void)std::filesystem::remove_all(knx::objects::persistenceNamespaceDir("knx_objects"), ec);
}

void setUp(void) {
    clearPersistenceStore();

    testClock.reset();
    platformInstance = std::make_unique<knx::platform::VirtualPlatform>(testClock);
    auto ownedPhysical = std::make_unique<MockPhysicalLayer>();
    physicalLayer = ownedPhysical.get();
    auto stackPort = knx::test::createTp1TestStackPort(*platformInstance, std::move(ownedPhysical));
    bauUnit = std::make_unique<bau::BusAccessUnit>(*platformInstance, std::move(stackPort));
    clearReceivedCallbacks();

    // Configure device
    bauUnit->deviceObject().setManufacturerId(knx::ManufacturerId(0x1234));

    // Register callbacks by group-object index
    bauUnit->setGroupObjectWriteCallback([](knx::GroupObjectIndex idx, std::span<const uint8_t> data) {
        receivedWrites.push_back({idx, std::vector<uint8_t>(data.begin(), data.end())});
    });

    bauUnit->setGroupObjectReadCallback([](knx::GroupObjectIndex idx) {
        receivedReads.push_back(idx);
    });

    // Start stack
    TEST_ASSERT_TRUE(bauUnit->init(IndividualAddress(1, 1, 10)).isOk());

    // Create group objects via BAU
    TEST_ASSERT_EQUAL_UINT16(0,
                             bauUnit->addGroupObject(GroupAddress(5, 3, 7),
                                                     application::dptids::Bool,
                                                     true,
                                                     true,
                                                     false,
                                                     true)
                                 .value());

    TEST_ASSERT_EQUAL_UINT16(1,
                             bauUnit->addGroupObject(GroupAddress(1, 2, 3),
                                                     application::dptids::Bool,
                                                     true,
                                                     true,
                                                     false,
                                                     true)
                                 .value());

    TEST_ASSERT_EQUAL_UINT16(2,
                             bauUnit->addGroupObject(GroupAddress(2, 1, 1),
                                                     application::dptids::SceneNumber,
                                                     true,
                                                     true,
                                                     false,
                                                     true)
                                 .value());
}

void tearDown(void) {
    bauUnit->close();
    bauUnit.reset();
    physicalLayer = nullptr;
    platformInstance.reset();
    clearReceivedCallbacks();
}

// 1) Individual-address telegram addressed to device => delivered without any
//    DL-generated frames. The TP1 acknowledgment is the single-character
//    short-ack produced at MAC/ISR level (03_02_02 §2.2.7), never an L_Data
//    frame from the data link layer.
void test_e2e_individual_address_no_dl_ack_frame(void) {
    drainSentFrames();

    // Send individual write using raw data link frame (simulate external device)
    datalink::LDataFrame tx;
    tx.source = IndividualAddress(1, 1, 20);
    tx.destination = GroupAddress();
    tx.destination.raw = IndividualAddress(1, 1, 10).raw; // our IA
    tx.destinationType = AddressType::Individual;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIField(0x00), {0x01});
    tx.ackRequested = true;

    // Encode and inject
    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }

    physicalLayer->injectFrame(raw);

    // No DL-generated frame may appear on the bus.
    std::vector<uint8_t> unexpected;
    TEST_ASSERT_FALSE(waitForSentFrame(unexpected));
}

// 2) Group write to subscribed GA => expect local delivery without ACK
void test_e2e_group_write_ack(void) {
    drainSentFrames();
    GroupAddress ga(5, 3, 7);

    // Build group write telegram
    datalink::LDataFrame tx;
    tx.source = IndividualAddress(1, 1, 42);
    tx.destination = ga;
    tx.destinationType = AddressType::Group;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01}); // DPT1 true
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }

    physicalLayer->injectFrame(raw);

    // Group-addressed telegrams must not emit TP1 ACKs.
    std::vector<uint8_t> ack;
    TEST_ASSERT_FALSE(physicalLayer->tryGetSentFrame(ack));

    // Application callback fired (object index 0 = GA(5,3,7))
    TEST_ASSERT_TRUE(waitForReceivedWrites(1));
    {
        TEST_ASSERT_EQUAL(1, static_cast<int>(receivedWrites.size()));
        TEST_ASSERT_EQUAL(0, static_cast<int>(receivedWrites[0].first.value()));
        TEST_ASSERT_EQUAL(0x01, receivedWrites[0].second[0]);
    }

    // Group object updated
    TEST_ASSERT_TRUE(bauUnit->isGroupObjectValid(knx::GroupObjectIndex(0)));
    auto objValue = bauUnit->groupObjectValue(knx::GroupObjectIndex(0));
    TEST_ASSERT_TRUE(objValue.isOk());
    TEST_ASSERT_TRUE(objValue.value().asBool());
}

// 3) Group read to subscribed GA => expect read callback and response, but no ACK
void test_e2e_group_read_ack(void) {
    drainSentFrames();
    GroupAddress ga(1, 2, 3);

    // Pre-set a value in the group object
    TEST_ASSERT_TRUE(bauUnit->setGroupObjectValue(knx::GroupObjectIndex(1), application::DptValue(true)).isOk());

    datalink::LDataFrame tx;
    tx.source = IndividualAddress(1, 1, 77);
    tx.destination = ga;
    tx.destinationType = AddressType::Group;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueRead, {});
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }

    physicalLayer->injectFrame(raw);

    // Read callback fired (object index 1 = GA(1,2,3))
    TEST_ASSERT_TRUE(waitForReceivedReads(1));
    {
        TEST_ASSERT_EQUAL(1, static_cast<int>(receivedReads.size()));
        TEST_ASSERT_EQUAL(1, static_cast<int>(receivedReads[0].value()));
    }

    // Group-addressed telegrams do not emit TP1 ACKs, but a subscribed read
    // still produces the application-layer group value response telegram.
    std::vector<uint8_t> response;
    TEST_ASSERT_TRUE(waitForSentFrame(response));
    TEST_ASSERT_TRUE((ctrlByte(response) & 0x01) == 0);
}

// 4) Telegram addressed to different group => expect no ACK
void test_e2e_other_group_no_ack(void) {
    drainSentFrames();

    datalink::LDataFrame tx;
    tx.source = IndividualAddress(1, 1, 33);
    tx.destination = GroupAddress(9, 9, 9); // not subscribed
    tx.destinationType = AddressType::Group;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x22});
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }

    physicalLayer->injectFrame(raw);

    // No ACK expected
    std::vector<uint8_t> maybeAck;
    TEST_ASSERT_FALSE(physicalLayer->tryGetSentFrame(maybeAck));

    // No callbacks fired
    {
        TEST_ASSERT_TRUE(receivedWrites.empty());
    }
}

// 5) Telegram addressed to us with invalid CRC => dropped silently at the DL.
//    The NAK for a corrupted frame is the MAC/ISR-level short-ack downgrade;
//    the data link layer never transmits anything of its own.
void test_e2e_invalid_crc_no_dl_nack_frame(void) {
    drainSentFrames();

    datalink::LDataFrame tx;
    tx.source = IndividualAddress(1, 1, 55);
    tx.destination = GroupAddress();
    tx.destination.raw = IndividualAddress(1, 1, 10).raw; // our IA
    tx.destinationType = AddressType::Individual;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIField(0x00), {0x44});
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }

    // Corrupt checksum
    TEST_ASSERT_FALSE(raw.empty());
    raw.back() ^= 0xFF;

    physicalLayer->injectFrame(raw);

    // Nothing transmitted, nothing delivered.
    std::vector<uint8_t> unexpected;
    TEST_ASSERT_FALSE(waitForSentFrame(unexpected));

    // No callbacks on bad checksum
    {
        TEST_ASSERT_TRUE(receivedWrites.empty());
    }
}

// 6) Property services: PropertyValueRead => expect PropertyValueResponse on the wire
void test_e2e_property_value_read_manufacturer_id(void) {
    drainSentFrames();

    const IndividualAddress requester(1, 1, 20);
    const IndividualAddress ourAddr(1, 1, 10);

    // Build property value read telegram addressed to us
    datalink::LDataFrame tx;
    tx.source = requester;
    tx.destination = GroupAddress();
    tx.destination.raw = ourAddr.raw;
    tx.destinationType = AddressType::Individual;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;

    // Payload: objectIndex, propertyId, (elementCount<<4)|(startIndex>>8), startIndex
    const uint8_t objectIndex = 0;
    const uint8_t propertyId = 12; // ManufacturerId (per KNX PropertyID)
    const uint8_t elementCount = 1;
    const uint16_t startIndex = 1;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyValueRead, {
        objectIndex,
        propertyId,
        static_cast<uint8_t>(((elementCount & 0x0F) << 4) | ((startIndex >> 8) & 0x0F)),
        static_cast<uint8_t>(startIndex & 0xFF),
    });
    tx.ackRequested = true;

    // Encode and inject
    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);

    // Expect PropertyValueResponse telegram (no DL ack frames precede it)
    std::vector<uint8_t> respRaw;
    if (!waitForSentFrame(respRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyValueResponse telegram, but none was sent");
        return;
    }

    datalink::LDataFrame resp;
    auto dec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(respRaw), resp);
    if (!dec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode PropertyValueResponse TP1 frame");
        return;
    }

    TEST_ASSERT_TRUE(resp.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL_UINT16(ourAddr.raw, resp.source.raw);
    TEST_ASSERT_EQUAL_UINT16(requester.raw, resp.destination.raw);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyValueResponse), resp.apci().raw);

    const auto respData = resp.payload();
    if (respData.size() < 6) {
        TEST_FAIL_MESSAGE("PropertyValueResponse payload too short");
        return;
    }
    TEST_ASSERT_EQUAL_UINT8(objectIndex, respData[0]);
    TEST_ASSERT_EQUAL_UINT8(propertyId, respData[1]);
    TEST_ASSERT_EQUAL_UINT8(elementCount, (respData[2] >> 4) & 0x0F);
    uint16_t gotStartIndex = (static_cast<uint16_t>(respData[2] & 0x0F) << 8) | respData[3];
    TEST_ASSERT_EQUAL_UINT16(startIndex, gotStartIndex);

    // ManufacturerId was configured via facade in setUp(): 0x1234
    TEST_ASSERT_EQUAL_UINT8(0x12, respData[4]);
    TEST_ASSERT_EQUAL_UINT8(0x34, respData[5]);
}

// 7) Property services: PropertyValueWrite ProgMode + readback
void test_e2e_property_value_write_prog_mode_and_readback(void) {
    drainSentFrames();

    const IndividualAddress requester(1, 1, 20);
    const IndividualAddress ourAddr(1, 1, 10);

    // Write ProgMode=1
    datalink::LDataFrame tx;
    tx.source = requester;
    tx.destination = GroupAddress();
    tx.destination.raw = ourAddr.raw;
    tx.destinationType = AddressType::Individual;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;

    const uint8_t objectIndex = 0;
    const uint8_t propertyId = 54; // ProgMode
    const uint8_t elementCount = 1;
    const uint16_t startIndex = 1;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyValueWrite, {
        objectIndex,
        propertyId,
        static_cast<uint8_t>(((elementCount & 0x0F) << 4) | ((startIndex >> 8) & 0x0F)),
        static_cast<uint8_t>(startIndex & 0xFF),
        0x01,
    });
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    // Expect PropertyValueResponse confirming write
    std::vector<uint8_t> respRaw;
    if (!waitForSentFrame(respRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyValueResponse for ProgMode write, but none was sent");
        return;
    }
    datalink::LDataFrame resp;
    auto dec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(respRaw), resp);
    if (!dec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode ProgMode write PropertyValueResponse");
        return;
    }
    TEST_ASSERT_TRUE(resp.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyValueResponse), resp.apci().raw);
    const auto respWriteData = resp.payload();
    TEST_ASSERT_TRUE(respWriteData.size() >= 5);
    TEST_ASSERT_EQUAL_UINT8(objectIndex, respWriteData[0]);
    TEST_ASSERT_EQUAL_UINT8(propertyId, respWriteData[1]);
    TEST_ASSERT_EQUAL_UINT8(0x01, respWriteData[4]);

    // Read back ProgMode and expect 0x01
    datalink::LDataFrame rd;
    rd.source = requester;
    rd.destination = GroupAddress();
    rd.destination.raw = ourAddr.raw;
    rd.destinationType = AddressType::Individual;
    rd.priority = Priority::Normal;
    rd.hopCount = 6;
    setTpdu(rd, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyValueRead, {
        objectIndex,
        propertyId,
        static_cast<uint8_t>(((elementCount & 0x0F) << 4) | ((startIndex >> 8) & 0x0F)),
        static_cast<uint8_t>(startIndex & 0xFF),
    });
    rd.ackRequested = true;

    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(rd).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    // Response
    if (!waitForSentFrame(respRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyValueResponse for ProgMode read, but none was sent");
        return;
    }
    dec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(respRaw), resp);
    if (!dec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode ProgMode read PropertyValueResponse");
        return;
    }
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyValueResponse), resp.apci().raw);
    const auto respReadData = resp.payload();
    TEST_ASSERT_TRUE(respReadData.size() >= 5);
    TEST_ASSERT_EQUAL_UINT8(0x01, respReadData[4]);
}

// 8) Property services: Subnet/DeviceAddress writes apply immediately.
//    These are load-procedure operations (KNX 03_05_02) with their own
//    authorization; programming mode gates only the broadcast
//    A_IndividualAddress_Write management service. Each applied write moves
//    the device, so subsequent telegrams must target the updated address.
void test_e2e_property_value_write_individual_address_via_properties(void) {
    drainSentFrames();

    const IndividualAddress requester(1, 1, 20);
    IndividualAddress targetAddr(1, 1, 10);

    const uint8_t objectIndex = 0;
    const uint8_t pidSubnet = 57; // SubnetAddress (high octet)
    const uint8_t pidDevice = 58; // DeviceAddress (low octet)
    const uint8_t elementCount = 1;
    const uint16_t startIndex = 1;

    auto sendWriteByte = [&](uint8_t pid, uint8_t value) {
        datalink::LDataFrame tx;
        tx.source = requester;
        tx.destination = GroupAddress();
        tx.destination.raw = targetAddr.raw;
        tx.destinationType = AddressType::Individual;
        tx.priority = Priority::Normal;
        tx.hopCount = 6;
        setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyValueWrite, {
            objectIndex,
            pid,
            static_cast<uint8_t>(((elementCount & 0x0F) << 4) | ((startIndex >> 8) & 0x0F)),
            static_cast<uint8_t>(startIndex & 0xFF),
            value,
        });
        tx.ackRequested = true;

        std::vector<uint8_t> raw;
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
        physicalLayer->injectFrame(raw);
    };

    // The PropertyValueResponse reads back the property value after the write
    // (KNX 03_03_07), so it must match the newly written value.
    auto expectReadBackResponse = [&](uint8_t pid, uint8_t writtenValue) {
        std::vector<uint8_t> rbRaw;
        TEST_ASSERT_TRUE(waitForSentFrame(rbRaw));
        datalink::LDataFrame rb;
        TEST_ASSERT_TRUE(datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(rbRaw), rb).isOk());
        TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyValueResponse), rb.apci().raw);
        auto rbPayload = rb.payload();
        TEST_ASSERT_EQUAL_UINT32(5u, static_cast<uint32_t>(rbPayload.size()));
        TEST_ASSERT_EQUAL_UINT8(objectIndex, rbPayload[0]);
        TEST_ASSERT_EQUAL_UINT8(pid, rbPayload[1]);
        TEST_ASSERT_EQUAL_UINT8(writtenValue, rbPayload[4]);
    };

    // Device IA starts as 1.1.10. Write device octet 0x21 -> IA 1.1.33.
    sendWriteByte(pidDevice, 0x21);
    expectReadBackResponse(pidDevice, 0x21);
    targetAddr = IndividualAddress(static_cast<uint16_t>(0x1121));

    // Write subnet octet 0x22 -> IA 2.2.33 (frame targets the moved device).
    sendWriteByte(pidSubnet, 0x22);
    expectReadBackResponse(pidSubnet, 0x22);
    targetAddr = IndividualAddress(static_cast<uint16_t>(0x2221));

    // The device must answer at its new address.
    sendWriteByte(pidDevice, 0x0A);
    expectReadBackResponse(pidDevice, 0x0A);
}

// 9) Property services: PropertyDescriptionRead => RW descriptor for ProgMode
void test_e2e_property_description_read_prog_mode_descriptor(void) {
    drainSentFrames();

    const IndividualAddress requester(1, 1, 20);
    const IndividualAddress ourAddr(1, 1, 10);

    datalink::LDataFrame tx;
    tx.source = requester;
    tx.destination = GroupAddress();
    tx.destination.raw = ourAddr.raw;
    tx.destinationType = AddressType::Individual;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyDescriptionRead, {
        0,   // objectIndex: Device Object
        54,  // propertyId: ProgMode
        0,   // propertyIndex: ID query
    });
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    std::vector<uint8_t> respRaw;
    if (!waitForSentFrame(respRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyDescriptionResponse, but none was sent");
        return;
    }

    datalink::LDataFrame resp;
    auto dec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(respRaw), resp);
    if (!dec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode PropertyDescriptionResponse TP1 frame");
        return;
    }

    TEST_ASSERT_TRUE(resp.destinationType == AddressType::Individual);
    TEST_ASSERT_EQUAL_UINT16(ourAddr.raw, resp.source.raw);
    TEST_ASSERT_EQUAL_UINT16(requester.raw, resp.destination.raw);
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyDescriptionResponse), resp.apci().raw);
    auto payload = resp.payload();
    TEST_ASSERT_EQUAL(7, static_cast<int>(payload.size()));

    TEST_ASSERT_EQUAL_UINT8(0, payload[0]);
    TEST_ASSERT_EQUAL_UINT8(54, payload[1]);
    // 03/03/07 §3.4.3.1: "For new implementations the property_index shall
    // contain the correct value of the addressed Property". Device Object
    // properties are numbered in ascending PID order, PID_PROG_MODE is the 14th.
    TEST_ASSERT_EQUAL_UINT8(13, payload[2]);

    // type=Bitset8 (0x33) and writeEnabled=true (bit7)
    TEST_ASSERT_EQUAL_UINT8(0xB3, payload[3]);
    // maxElements=1
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[4]);
    TEST_ASSERT_EQUAL_UINT8(0x01, payload[5]);
    // readLevel/writeLevel are 0 in BAU registration
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[6]);
}

// 10) Property services: PropertyDescriptionRead => RO descriptor for ManufacturerId
void test_e2e_property_description_read_manufacturer_id_descriptor(void) {
    drainSentFrames();

    const IndividualAddress requester(1, 1, 20);
    const IndividualAddress ourAddr(1, 1, 10);

    datalink::LDataFrame tx;
    tx.source = requester;
    tx.destination = GroupAddress();
    tx.destination.raw = ourAddr.raw;
    tx.destinationType = AddressType::Individual;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyDescriptionRead, {
        0,   // objectIndex: Device Object
        12,  // propertyId: ManufacturerId
        0,   // propertyIndex: ID query
    });
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    std::vector<uint8_t> respRaw;
    if (!waitForSentFrame(respRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyDescriptionResponse, but none was sent");
        return;
    }

    datalink::LDataFrame resp;
    auto dec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(respRaw), resp);
    if (!dec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode PropertyDescriptionResponse TP1 frame");
        return;
    }

    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyDescriptionResponse), resp.apci().raw);
    auto payload = resp.payload();
    TEST_ASSERT_EQUAL(7, static_cast<int>(payload.size()));

    TEST_ASSERT_EQUAL_UINT8(0, payload[0]);
    TEST_ASSERT_EQUAL_UINT8(12, payload[1]);
    TEST_ASSERT_EQUAL_UINT8(5, payload[2]);  // the property's own index, not the echo

    // type=UnsignedInt (4) and writeEnabled=false
    TEST_ASSERT_EQUAL_UINT8(0x04, payload[3]);
    // maxElements=1
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[4]);
    TEST_ASSERT_EQUAL_UINT8(0x01, payload[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[6]);
}

// 11) Property services: PropertyDescriptionRead => non-device object descriptor (Group Object Table PID_TABLE)
// Group Object Table (object type 9) sits at object index 3 in the canonical
// SystemB layout (0=Device, 1=AddrTable, 2=AssocTable, 3=GOTable, 4=AppProgram)
void test_e2e_property_description_read_group_object_table_table_data_descriptor(void) {
    drainSentFrames();

    const IndividualAddress requester(1, 1, 20);
    const IndividualAddress ourAddr(1, 1, 10);

    datalink::LDataFrame tx;
    tx.source = requester;
    tx.destination = GroupAddress();
    tx.destination.raw = ourAddr.raw;
    tx.destinationType = AddressType::Individual;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyDescriptionRead, {
        3,  // objectIndex: Group Object Table (canonical SystemB ordering)
        23, // propertyId: PID_TABLE
        0,  // propertyIndex: ID query
    });
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    std::vector<uint8_t> respRaw;
    if (!waitForSentFrame(respRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyDescriptionResponse, but none was sent");
        return;
    }

    datalink::LDataFrame resp;
    auto dec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(respRaw), resp);
    if (!dec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode PropertyDescriptionResponse TP1 frame");
        return;
    }

    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyDescriptionResponse), resp.apci().raw);
    auto payload = resp.payload();
    TEST_ASSERT_EQUAL(7, static_cast<int>(payload.size()));
    TEST_ASSERT_EQUAL_UINT8(3, payload[0]);  // objectIndex 3 (Group Object Table)
    TEST_ASSERT_EQUAL_UINT8(23, payload[1]);
    TEST_ASSERT_EQUAL_UINT8(3, payload[2]);  // PIDs 1, 5, 7, 23, 66: index 3
    // 03/05/01 §4.18.6.2.4 (Realisation Type 7): PID_TABLE is PDT_GENERIC_02[]
    // (0x12), one Group Object Descriptor per element, and writeable (0x80):
    // "Writing shall only have effect in the load state Loading".
    TEST_ASSERT_EQUAL_UINT8(0x92, payload[3]);
    // max_nr_of_elem = 256, the table's fixed capacity (12-bit field).
    TEST_ASSERT_EQUAL_UINT8(0x01, payload[4]);
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[6]);
}

// 12) Property services: PropertyDescriptionRead by property index (Device Object)
void test_e2e_property_description_read_by_index_device_object(void) {
    drainSentFrames();

    const IndividualAddress requester(1, 1, 20);
    const IndividualAddress ourAddr(1, 1, 10);

    datalink::LDataFrame tx;
    tx.source = requester;
    tx.destination = GroupAddress();
    tx.destination.raw = ourAddr.raw;
    tx.destinationType = AddressType::Individual;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyDescriptionRead, {
        0,  // objectIndex: Device Object
        0,  // propertyId: 0 selects by index
        4,  // propertyIndex: 0-based, ascending PIDs 1, 5, 6, 9, 11 → SerialNumber
    });
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    std::vector<uint8_t> respRaw;
    if (!waitForSentFrame(respRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyDescriptionResponse, but none was sent");
        return;
    }

    datalink::LDataFrame resp;
    auto dec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(respRaw), resp);
    if (!dec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode PropertyDescriptionResponse TP1 frame");
        return;
    }

    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyDescriptionResponse), resp.apci().raw);
    auto payload = resp.payload();
    TEST_ASSERT_EQUAL(7, static_cast<int>(payload.size()));
    TEST_ASSERT_EQUAL_UINT8(0, payload[0]);
    TEST_ASSERT_EQUAL_UINT8(11, payload[1]); // SerialNumber (PID 11) at index 4
    TEST_ASSERT_EQUAL_UINT8(4, payload[2]); // echoed propertyIndex
    // type=PDT_GENERIC_06 (0x16) and writeEnabled=false
    TEST_ASSERT_EQUAL_UINT8(0x16, payload[3]);
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[4]);
    TEST_ASSERT_EQUAL_UINT8(0x01, payload[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[6]);
}

// Send one unconnected management request to the device and return the
// payload of the response (empty when none came back).
static std::vector<uint8_t> requestResponse(application::APCIService service,
                                            std::initializer_list<uint8_t> payload,
                                            application::APCIService expectedResponse) {
    drainSentFrames();
    const IndividualAddress requester(1, 1, 20);
    const IndividualAddress ourAddr(1, 1, 10);

    datalink::LDataFrame tx;
    tx.source = requester;
    tx.destination = GroupAddress();
    tx.destination.raw = ourAddr.raw;
    tx.destinationType = AddressType::Individual;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, service, payload);
    tx.ackRequested = true;

    std::vector<uint8_t> raw;
    if (!bauUnit->link().sendFrame(tx).isOk() || !waitForSentFrame(raw)) {
        return {};
    }
    physicalLayer->injectFrame(raw);

    // Skip anything that is not the expected response (e.g. a DL ACK).
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<uint8_t> respRaw;
        if (!waitForSentFrame(respRaw)) {
            return {};
        }
        datalink::LDataFrame resp;
        if (!datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(respRaw), resp).isOk()) {
            continue;
        }
        if (resp.apci().raw == static_cast<uint16_t>(expectedResponse)) {
            auto data = resp.payload();
            return std::vector<uint8_t>(data.begin(), data.end());
        }
    }
    return {};
}

static std::vector<uint8_t> classicDescription(uint8_t objectIndex, uint8_t pid, uint8_t index) {
    return requestResponse(application::APCIService::PropertyDescriptionRead, {objectIndex, pid, index},
                           application::APCIService::PropertyDescriptionResponse);
}

static std::vector<uint8_t> extDescription(uint16_t objectType, uint16_t pid, uint16_t index) {
    // OT(2) | OI(12 bit) PID(12 bit) | type(4 bit) index(12 bit)
    return requestResponse(application::APCIService::PropertyExtDescriptionRead,
                           {static_cast<uint8_t>(objectType >> 8), static_cast<uint8_t>(objectType & 0xFF),
                            0x00, static_cast<uint8_t>(0x10 | ((pid >> 8) & 0x0F)),
                            static_cast<uint8_t>(pid & 0xFF),
                            static_cast<uint8_t>((index >> 8) & 0x0F), static_cast<uint8_t>(index & 0xFF)},
                           application::APCIService::PropertyExtDescriptionResponse);
}

// 12b) A_PropertyDescription_Read and A_PropertyExtDescription_Read describe
// the same objects: for every index of every core object they name the same
// PID with the same datatype, size and access, and a by-PID read in either
// family reports that index (03/03/07 §3.4.3.1, §3.4.3.2).
void test_e2e_classic_and_extended_descriptions_agree(void) {
    // Canonical SystemB layout: index → object type.
    // Index 5, the Security Interface Object, exists only with KNX Secure.
    const uint16_t kTypes[] = {0, 1, 2, 9, 3, 17};
    constexpr uint8_t kObjects = KNX_SECURE_ENABLED ? 6 : 5;
    for (uint8_t object = 0; object < kObjects; ++object) {
        uint8_t index = 0;
        for (; index < 64; ++index) {
            const auto classic = classicDescription(object, 0, index);
            const auto ext = extDescription(kTypes[object], 0, index);
            TEST_ASSERT_EQUAL(7, static_cast<int>(classic.size()));
            TEST_ASSERT_EQUAL(15, static_cast<int>(ext.size()));

            const uint16_t classicMax = static_cast<uint16_t>(((classic[4] & 0x0F) << 8) | classic[5]);
            const uint16_t extMax = static_cast<uint16_t>((ext[12] << 8) | ext[13]);
            const uint16_t extPid = static_cast<uint16_t>(((ext[3] & 0x0F) << 8) | ext[4]);
            const uint16_t extIndex = static_cast<uint16_t>(((ext[5] & 0x0F) << 8) | ext[6]);
            if (classicMax == 0) {
                TEST_ASSERT_EQUAL_UINT16(0, extMax);
                break;
            }
            TEST_ASSERT_EQUAL_UINT16(classic[1], extPid);
            TEST_ASSERT_EQUAL_UINT8(index, classic[2]);
            TEST_ASSERT_EQUAL_UINT16(index, extIndex);
            TEST_ASSERT_EQUAL_UINT8(classic[3], ext[11]);  // write bit + PDT
            // Same capacity; the classic field has 12 bits and saturates.
            TEST_ASSERT_EQUAL_UINT16(extMax > 0x0FFF ? 0x0FFF : extMax, classicMax);
            TEST_ASSERT_EQUAL_UINT8(0, classic[4] & 0xF0);  // reserved bits
            TEST_ASSERT_EQUAL_UINT8(classic[6], ext[14]);  // access levels
            if (index == 0) {
                TEST_ASSERT_EQUAL_UINT8(1, classic[1]);  // PID_OBJECT_TYPE first
            }

            const auto classicById = classicDescription(object, classic[1], 0);
            const auto extById = extDescription(kTypes[object], classic[1], 0);
            TEST_ASSERT_EQUAL(7, static_cast<int>(classicById.size()));
            TEST_ASSERT_EQUAL(15, static_cast<int>(extById.size()));
            // A by-PID description is gated by the Access Policy (03/04/01
            // §6.2.6.3.4), e.g. the Security Object's key tables over a plain
            // connection: then both families refuse it alike.
            const uint16_t classicByIdMax = static_cast<uint16_t>(((classicById[4] & 0x0F) << 8) | classicById[5]);
            const uint16_t extByIdMax = static_cast<uint16_t>((extById[12] << 8) | extById[13]);
            if (classicByIdMax == 0) {
                TEST_ASSERT_EQUAL_UINT16(0, extByIdMax);
                continue;
            }
            TEST_ASSERT_EQUAL_UINT8(index, classicById[2]);
            TEST_ASSERT_EQUAL_UINT16(index, static_cast<uint16_t>(((extById[5] & 0x0F) << 8) | extById[6]));
        }
        TEST_ASSERT_TRUE(index > 1 && index < 64);
    }
}

// 12c) The Group Object Table's PID_TABLE through both families on the wire:
// same description, and the extended write is no longer refused while the
// classic one is accepted.
void test_e2e_group_object_table_table_through_both_families(void) {
    const auto classic = classicDescription(3, 23, 0);
    const auto ext = extDescription(9, 23, 0);
    TEST_ASSERT_EQUAL(7, static_cast<int>(classic.size()));
    TEST_ASSERT_EQUAL(15, static_cast<int>(ext.size()));
    TEST_ASSERT_EQUAL_UINT8(0x92, classic[3]);
    TEST_ASSERT_EQUAL_UINT8(0x92, ext[11]);
    TEST_ASSERT_EQUAL_UINT8(0x01, ext[12]);
    TEST_ASSERT_EQUAL_UINT8(0x00, ext[13]);

    // Element 0 (the length) through A_PropertyExtValue_Read, then a
    // descriptor write to element 1.
    const auto length = requestResponse(application::APCIService::PropertyExtValueRead,
                                        {0x00, 0x09, 0x00, 0x10, 23, 1, 0x00, 0x00},
                                        application::APCIService::PropertyExtValueResponse);
    TEST_ASSERT_EQUAL(10, static_cast<int>(length.size()));
    const uint16_t count = static_cast<uint16_t>((length[8] << 8) | length[9]);
    TEST_ASSERT_TRUE(count > 0);  // setUp() adds group objects
    const auto element = requestResponse(application::APCIService::PropertyExtValueRead,
                                         {0x00, 0x09, 0x00, 0x10, 23, 1, 0x00, 0x01},
                                         application::APCIService::PropertyExtValueResponse);
    TEST_ASSERT_EQUAL(10, static_cast<int>(element.size()));
    const auto written = requestResponse(application::APCIService::PropertyExtValueWriteCon,
                                         {0x00, 0x09, 0x00, 0x10, 23, 1, 0x00, 0x01, element[8], element[9]},
                                         application::APCIService::PropertyExtValueWriteConRes);
    TEST_ASSERT_EQUAL(9, static_cast<int>(written.size()));
    TEST_ASSERT_EQUAL_UINT8(0x00, written[8]);  // E_SUCCESS, was E_ACCESS_READ_ONLY
}

// 13) Property services: indexed Address Table TableData write + indexed read
void test_e2e_property_value_indexed_address_table_table_data(void) {
    drainSentFrames();

    const IndividualAddress requester(1, 1, 20);
    const IndividualAddress ourAddr(1, 1, 10);

    // Write 2 entries starting at index 1
    datalink::LDataFrame wr;
    wr.source = requester;
    wr.destination = GroupAddress();
    wr.destination.raw = ourAddr.raw;
    wr.destinationType = AddressType::Individual;
    wr.priority = Priority::Normal;
    wr.hopCount = 6;
    // objectIndex=1 (Address Table), propertyId=23 (PID_TABLE/TableData), elementCount=2, startIndex=1
    setTpdu(wr, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyValueWrite, {
        1,
        23,
        static_cast<uint8_t>((2u << 4) | 0u),
        0x01,
        0x09, 0x01, // 1/1/1
        0x09, 0x02, // 1/1/2
    });
    wr.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(wr).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    std::vector<uint8_t> wrRespRaw;
    if (!waitForSentFrame(wrRespRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyValueResponse to write, but none was sent");
        return;
    }
    datalink::LDataFrame wrResp;
    auto wrDec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(wrRespRaw), wrResp);
    if (!wrDec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode PropertyValueResponse (write)");
        return;
    }
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyValueResponse), wrResp.apci().raw);

    drainSentFrames();

    // Read back the 2nd entry only (startIndex=2, elementCount=1)
    datalink::LDataFrame rd;
    rd.source = requester;
    rd.destination = GroupAddress();
    rd.destination.raw = ourAddr.raw;
    rd.destinationType = AddressType::Individual;
    rd.priority = Priority::Normal;
    rd.hopCount = 6;
    setTpdu(rd, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyValueRead, {
        1,
        23,
        static_cast<uint8_t>((1u << 4) | 0u),
        0x02,
    });
    rd.ackRequested = true;

    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(rd).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    std::vector<uint8_t> rdRespRaw;
    if (!waitForSentFrame(rdRespRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyValueResponse (read), but none was sent");
        return;
    }
    datalink::LDataFrame rdResp;
    auto rdDec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(rdRespRaw), rdResp);
    if (!rdDec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode PropertyValueResponse (read)");
        return;
    }

    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyValueResponse), rdResp.apci().raw);
    auto payload = rdResp.payload();
    if (payload.size() < 6) {
        TEST_FAIL_MESSAGE("PropertyValueResponse payload too short");
        return;
    }
    // header: obj, prop, (count<<4)|hi(start), lo(start)
    TEST_ASSERT_EQUAL_UINT8(1, payload[0]);
    TEST_ASSERT_EQUAL_UINT8(23, payload[1]);
    TEST_ASSERT_EQUAL_UINT8(1, (payload[2] >> 4) & 0x0F);
    uint16_t gotStartIndex = (static_cast<uint16_t>(payload[2] & 0x0F) << 8) | payload[3];
    TEST_ASSERT_EQUAL_UINT16(2, gotStartIndex);
    // expect GA 1/1/2 => raw 0x0902
    TEST_ASSERT_EQUAL_UINT8(0x09, payload[4]);
    TEST_ASSERT_EQUAL_UINT8(0x02, payload[5]);
}

// 14) Property services: indexed Association Table TableData write + indexed read
void test_e2e_property_value_indexed_association_table_table_data(void) {
    drainSentFrames();

    const IndividualAddress requester(1, 1, 20);
    const IndividualAddress ourAddr(1, 1, 10);

    // Write 2 association entries starting at index 1
    datalink::LDataFrame wr;
    wr.source = requester;
    wr.destination = GroupAddress();
    wr.destination.raw = ourAddr.raw;
    wr.destinationType = AddressType::Individual;
    wr.priority = Priority::Normal;
    wr.hopCount = 6;
    // objectIndex=2 (Association Table), propertyId=23 (PID_TABLE/TableData), elementCount=2, startIndex=1
    setTpdu(wr, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyValueWrite, {
        2,
        23,
        static_cast<uint8_t>((2u << 4) | 0u),
        0x01,
        0x00, 0x01, 0x00, 0x10, // addrIndex=1, go=16
        0x00, 0x02, 0x00, 0x11, // addrIndex=2, go=17
    });
    wr.ackRequested = true;

    std::vector<uint8_t> raw;
    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(wr).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    std::vector<uint8_t> wrRespRaw;
    if (!waitForSentFrame(wrRespRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyValueResponse to write, but none was sent");
        return;
    }
    datalink::LDataFrame wrResp;
    auto wrDec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(wrRespRaw), wrResp);
    if (!wrDec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode PropertyValueResponse (write)");
        return;
    }
    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyValueResponse), wrResp.apci().raw);

    drainSentFrames();

    // Read back the 2nd entry only (startIndex=2, elementCount=1)
    datalink::LDataFrame rd;
    rd.source = requester;
    rd.destination = GroupAddress();
    rd.destination.raw = ourAddr.raw;
    rd.destinationType = AddressType::Individual;
    rd.priority = Priority::Normal;
    rd.hopCount = 6;
    setTpdu(rd, knx::protocol::TPCI::UnnumberedData, application::APCIService::PropertyValueRead, {
        2,
        23,
        static_cast<uint8_t>((1u << 4) | 0u),
        0x02,
    });
    rd.ackRequested = true;

    {
        TEST_ASSERT_TRUE(bauUnit->link().sendFrame(rd).isOk());
        TEST_ASSERT_TRUE(waitForSentFrame(raw));
    }
    physicalLayer->injectFrame(raw);


    std::vector<uint8_t> rdRespRaw;
    if (!waitForSentFrame(rdRespRaw)) {
        TEST_FAIL_MESSAGE("Expected PropertyValueResponse (read), but none was sent");
        return;
    }
    datalink::LDataFrame rdResp;
    auto rdDec = datalink::FrameCodec::decodeFrame(std::span<const uint8_t>(rdRespRaw), rdResp);
    if (!rdDec.isOk()) {
        TEST_FAIL_MESSAGE("Failed to decode PropertyValueResponse (read)");
        return;
    }

    TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(application::APCIService::PropertyValueResponse), rdResp.apci().raw);
    auto payload = rdResp.payload();
    if (payload.size() < 8) {
        TEST_FAIL_MESSAGE("PropertyValueResponse payload too short");
        return;
    }
    TEST_ASSERT_EQUAL_UINT8(2, payload[0]);
    TEST_ASSERT_EQUAL_UINT8(23, payload[1]);
    TEST_ASSERT_EQUAL_UINT8(1, (payload[2] >> 4) & 0x0F);
    uint16_t gotStartIndex = (static_cast<uint16_t>(payload[2] & 0x0F) << 8) | payload[3];
    TEST_ASSERT_EQUAL_UINT16(2, gotStartIndex);
    // expect addrIndex=2, go=17
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[4]);
    TEST_ASSERT_EQUAL_UINT8(0x02, payload[5]);
    TEST_ASSERT_EQUAL_UINT8(0x00, payload[6]);
    TEST_ASSERT_EQUAL_UINT8(0x11, payload[7]);
}

// 15) BAU API: send group value
void test_e2e_bau_send_group_value(void) {
    drainSentFrames();
    GroupAddress ga(5, 3, 7);
    std::vector<uint8_t> data = {0x01};

    TEST_ASSERT_TRUE(bauUnit->sendGroupValue(ga, data).isOk());

    // Telegram should be on the wire
    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(waitForSentFrame(sent));
    TEST_ASSERT_FALSE(sent.empty());
}

// 16) BAU API: request group value
void test_e2e_bau_request_group_value(void) {
    drainSentFrames();
    GroupAddress ga(5, 3, 7);

    TEST_ASSERT_TRUE(bauUnit->requestGroupValue(ga).isOk());

    // Telegram should be on the wire
    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(waitForSentFrame(sent));
    TEST_ASSERT_FALSE(sent.empty());
}

// 17) BAU API: begin send group value via explicit progression seam
void test_e2e_bau_begin_send_group_value(void) {
    drainSentFrames();
    GroupAddress ga(5, 3, 7);
    std::vector<uint8_t> data = {0x01};

    TEST_ASSERT_TRUE(bauUnit->beginSendGroupValue(ga, data).isOk());

    auto progress = bauUnit->transmission().poll();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(bau::BusAccessUnit::TransmissionProgressState::Success),
                      static_cast<int>(progress.value()));

    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(waitForSentFrame(sent));
    TEST_ASSERT_FALSE(sent.empty());
}

// 18) BAU/object API: request value by group-object index
void test_e2e_bau_request_group_object_value(void) {
    drainSentFrames();
    GroupAddress ga(6, 1, 2);
    knx::GroupObjectIndex idx = bauUnit->addGroupObject(
        ga, /*dpt=*/application::dptids::Bool, /*readable=*/true, /*writable=*/false, /*transmit=*/false, /*receivable=*/true);
    TEST_ASSERT_TRUE(idx.isValid());

    TEST_ASSERT_TRUE(bauUnit->requestGroupObjectValue(idx).isOk());

    // Telegram should be on the wire
    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(waitForSentFrame(sent));
    TEST_ASSERT_FALSE(sent.empty());
}

// 19) BAU API: request value by group address
void test_e2e_bau_request_group_object_value_by_address(void) {
    drainSentFrames();
    GroupAddress ga(7, 2, 1);
    knx::GroupObjectIndex idx = bauUnit->addGroupObject(
        ga, /*dpt=*/application::dptids::Bool, /*readable=*/true, /*writable=*/false, /*transmit=*/false, /*receivable=*/true);
    TEST_ASSERT_TRUE(idx.isValid());

    TEST_ASSERT_TRUE(bauUnit->requestGroupValue(ga).isOk());

    // Telegram should be on the wire
    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(waitForSentFrame(sent));
    TEST_ASSERT_FALSE(sent.empty());
}

// 20) BAU API: begin request value by group address
void test_e2e_bau_begin_request_group_object_value_by_address(void) {
    drainSentFrames();
    GroupAddress ga(7, 4, 1);
    knx::GroupObjectIndex idx = bauUnit->addGroupObject(
        ga, /*dpt=*/application::dptids::Bool, /*readable=*/true, /*writable=*/false, /*transmit=*/false, /*receivable=*/true);
    TEST_ASSERT_TRUE(idx.isValid());

    TEST_ASSERT_TRUE(bauUnit->beginRequestGroupValue(ga).isOk());

    auto progress = bauUnit->transmission().poll();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(bau::BusAccessUnit::TransmissionProgressState::Success),
                      static_cast<int>(progress.value()));

    std::vector<uint8_t> sent;
    TEST_ASSERT_TRUE(waitForSentFrame(sent));
    TEST_ASSERT_FALSE(sent.empty());
}

// 21) BAU API: observe internally triggered group-read response outcome
void test_e2e_bau_observes_auto_group_value_response_outcome(void) {
    drainSentFrames();
    GroupAddress ga(1, 2, 3);

    datalink::LDataFrame tx;
    tx.source = IndividualAddress(1, 1, 77);
    tx.destination = ga;
    tx.destinationType = AddressType::Group;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueRead, {});
    tx.ackRequested = false;

    std::vector<uint8_t> raw;
    TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
    TEST_ASSERT_TRUE(waitForSentFrame(raw));
    drainSentFrames();

    bauUnit->transmission().setDefaultOptions(
        bau::BusAccessUnit::TransmissionOptions{2u, true, true, false});

    physicalLayer->queueSendResult(knx::util::ErrorCode::Timeout);
    physicalLayer->queueSendResult(knx::util::ErrorCode::Success);

    TEST_ASSERT_TRUE(bauUnit->setGroupObjectValue(knx::GroupObjectIndex(1), application::DptValue(true)).isOk());

    physicalLayer->injectFrame(raw);

    std::vector<uint8_t> response;
    TEST_ASSERT_TRUE(waitForSentFrame(response));

    TEST_ASSERT_TRUE(waitForReceivedReads(1));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(bauUnit->transmission().queuedOutcomeCount()));

    bau::BusAccessUnit::TransmissionOutcome outcome;
    TEST_ASSERT_TRUE(bauUnit->transmission().popOutcome(outcome));
    TEST_ASSERT_EQUAL_UINT16(ga.raw, outcome.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<int>(bau::BusAccessUnit::MessageKind::GroupValueResponse), static_cast<int>(outcome.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_e2e_bau_deferred_auto_group_value_response_progression(void) {
    drainSentFrames();
    GroupAddress ga(1, 2, 3);

    datalink::LDataFrame tx;
    tx.source = IndividualAddress(1, 1, 88);
    tx.destination = ga;
    tx.destinationType = AddressType::Group;
    tx.priority = Priority::Normal;
    tx.hopCount = 6;
    setTpdu(tx, knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueRead, {});
    tx.ackRequested = false;

    std::vector<uint8_t> raw;
    TEST_ASSERT_TRUE(bauUnit->link().sendFrame(tx).isOk());
    TEST_ASSERT_TRUE(waitForSentFrame(raw));
    drainSentFrames();

    bauUnit->transmission().setAutoResponseMode(bau::BusAccessUnit::AutoResponseMode::Deferred);
    bauUnit->transmission().setDefaultOptions(bau::BusAccessUnit::TransmissionOptions{2u, true, true, false});

    physicalLayer->queueSendResult(knx::util::ErrorCode::Timeout);
    physicalLayer->queueSendResult(knx::util::ErrorCode::Success);

    TEST_ASSERT_TRUE(bauUnit->setGroupObjectValue(knx::GroupObjectIndex(1), application::DptValue(true)).isOk());

    physicalLayer->injectFrame(raw);

    TEST_ASSERT_TRUE(waitForReceivedReads(1));
    bool startedAutomaticResponse = false;
    for (uint32_t attempt = 0u; attempt < 1000u; ++attempt) {
        auto beginResult = bauUnit->transmission().beginAutomaticResponse();
        if (beginResult.isOk()) {
            startedAutomaticResponse = true;
            break;
        }
        TEST_ASSERT_EQUAL(static_cast<int>(util::ErrorCode::OperationNotReady), static_cast<int>(beginResult.error()));
    }
    TEST_ASSERT_TRUE(startedAutomaticResponse);

    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(bauUnit->transmission().queuedAutomaticResponseCount()));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(bauUnit->transmission().queuedOutcomeCount()));

    auto progress = bauUnit->transmission().pollAutomaticResponse();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(bau::BusAccessUnit::TransmissionProgressState::Pending),
                      static_cast<int>(progress.value()));

    progress = bauUnit->transmission().pollAutomaticResponse();
    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(static_cast<int>(bau::BusAccessUnit::TransmissionProgressState::Success),
                      static_cast<int>(progress.value()));

    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(bauUnit->transmission().queuedOutcomeCount()));
    bau::BusAccessUnit::TransmissionOutcome outcome;
    TEST_ASSERT_TRUE(bauUnit->transmission().popOutcome(outcome));
    TEST_ASSERT_EQUAL_UINT16(ga.raw, outcome.destination.raw);
    TEST_ASSERT_EQUAL(static_cast<int>(bau::BusAccessUnit::MessageKind::GroupValueResponse), static_cast<int>(outcome.kind));
    TEST_ASSERT_EQUAL(static_cast<int>(knx::util::ErrorCode::Success), static_cast<int>(outcome.result));
    TEST_ASSERT_EQUAL_UINT8(2u, outcome.attempts);
}

void test_e2e_bau_group_object_runtime_bridge_supports_scene_number(void) {
    const application::Dpt17Value original{42u};

    TEST_ASSERT_TRUE(bauUnit->setGroupObjectValue(knx::GroupObjectIndex(2), application::DptValue(original)).isOk());

    auto valueResult = bauUnit->groupObjectValue(knx::GroupObjectIndex(2));
    TEST_ASSERT_TRUE(valueResult.isOk());

    const auto& decoded = valueResult.value().asSceneNumber();
    TEST_ASSERT_EQUAL_UINT8(original.sceneNumber, decoded.sceneNumber);
}

int run_all_knx_stack_e2e_tests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_e2e_individual_address_no_dl_ack_frame);
    RUN_TEST(test_e2e_group_write_ack);
    RUN_TEST(test_e2e_group_read_ack);
    RUN_TEST(test_e2e_other_group_no_ack);
    RUN_TEST(test_e2e_invalid_crc_no_dl_nack_frame);
    RUN_TEST(test_e2e_property_value_read_manufacturer_id);
    RUN_TEST(test_e2e_property_value_write_prog_mode_and_readback);
    RUN_TEST(test_e2e_property_value_write_individual_address_via_properties);
    RUN_TEST(test_e2e_property_description_read_prog_mode_descriptor);
    RUN_TEST(test_e2e_property_description_read_manufacturer_id_descriptor);
    RUN_TEST(test_e2e_property_description_read_group_object_table_table_data_descriptor);
    RUN_TEST(test_e2e_property_description_read_by_index_device_object);
    RUN_TEST(test_e2e_classic_and_extended_descriptions_agree);
    RUN_TEST(test_e2e_group_object_table_table_through_both_families);
    RUN_TEST(test_e2e_property_value_indexed_address_table_table_data);
    RUN_TEST(test_e2e_property_value_indexed_association_table_table_data);
    RUN_TEST(test_e2e_bau_send_group_value);
    RUN_TEST(test_e2e_bau_request_group_value);
    RUN_TEST(test_e2e_bau_begin_send_group_value);
    RUN_TEST(test_e2e_bau_request_group_object_value);
    RUN_TEST(test_e2e_bau_request_group_object_value_by_address);
    RUN_TEST(test_e2e_bau_begin_request_group_object_value_by_address);
    RUN_TEST(test_e2e_bau_observes_auto_group_value_response_outcome);
    RUN_TEST(test_e2e_bau_deferred_auto_group_value_response_progression);
    RUN_TEST(test_e2e_bau_group_object_runtime_bridge_supports_scene_number);
    return UNITY_END();
}

int main() {
    return run_all_knx_stack_e2e_tests();
}
