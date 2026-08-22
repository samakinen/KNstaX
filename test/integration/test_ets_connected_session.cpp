// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_ets_connected_session.cpp
 * @brief Connection-oriented management session as ETS drives it
 *
 * Replays the opening of an ETS download — T_Connect, then the
 * A_DeviceDescriptor_Read / A_PropertyValue_Read / A_Authorize_Request /
 * A_PropertyDescription_Read exchange — through the whole BAU and checks the
 * transport sequence numbers on the wire.
 *
 * The interesting case is the lossy one. KNXnet/IP routing rides on 802.11
 * multicast when the device is on Wi-Fi: group-addressed frames are neither
 * acknowledged nor retried at the link layer, so an occasional T_ACK simply
 * does not arrive. That must cost at most one telegram, never the session.
 */

#include "unity.h"

#include "knx/bau/bau.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/protocol/tpdu_codec.hpp"
#include "knx/objects/object_persistence.hpp"

#include "../mocks/mock_physical_layer.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

using namespace knx;
using namespace knx::test;

namespace {

constexpr IndividualAddress kOwnAddress(0, 1, 1);
constexpr IndividualAddress kEtsAddress(0, 0, 1);

MockPhysicalLayer* gPhysical = nullptr;
std::unique_ptr<knx::platform::LinuxPlatform> gPlatform;
std::unique_ptr<knx::bau::BusAccessUnit> gBau;

/// One frame the device put on the wire, reduced to what this test asserts on.
struct SentFrame {
    protocol::TPCI tpci{};
    uint8_t sequenceNumber{0};
    uint16_t apci{0};
};

void clearPersistenceStore() {
    std::error_code ec;
    (void)std::filesystem::remove_all(knx::objects::persistenceNamespaceDir("knx_objects"), ec);
}

/// Put a frame from ETS on the bus, addressed point-to-point to the device.
void injectFromEts(std::span<const uint8_t> tpdu) {
    datalink::LDataFrame frame;
    frame.source = kEtsAddress;
    frame.destination = GroupAddress(kOwnAddress.raw);
    frame.destinationType = AddressType::Individual;
    frame.priority = Priority::System;
    frame.hopCount = 6;
    frame.tpdu.assign(tpdu);

    std::array<uint8_t, datalink::LDataFrame::MAX_FRAME_SIZE> buffer{};
    auto encoded = datalink::FrameCodec::encodeFrame(frame, buffer);
    TEST_ASSERT_TRUE(encoded.isOk());
    gPhysical->injectFrame(std::vector<uint8_t>(buffer.begin(), buffer.begin() + encoded.value()));
}

/// Run the owner loop for a while and collect everything the device sent.
std::vector<SentFrame> pumpAndCollect(int budgetMs = 200) {
    std::vector<SentFrame> sent;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    while (std::chrono::steady_clock::now() < deadline) {
        gBau->loop();

        std::vector<uint8_t> wire;
        while (gPhysical->getSentFrame(wire)) {
            datalink::LDataFrame decoded;
            if (datalink::FrameCodec::decodeFrame(wire, decoded).isError() || decoded.tpdu.empty()) {
                continue;
            }
            const auto header = protocol::unpackTpduHeader(
                decoded.tpdu[0], decoded.tpdu.size() > 1u ? decoded.tpdu[1] : 0u);
            sent.push_back(SentFrame{header.tpci.type(), header.tpci.seqNum(), header.apci.raw});
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return sent;
}

std::vector<uint8_t> numberedRequest(uint8_t seqNum,
                                     application::APCIService service,
                                     std::span<const uint8_t> data) {
    const auto tpdu = protocol::buildTpdu(
        protocol::TPCIField::create(protocol::TPCI::NumberedData, seqNum),
        application::APCIField::create(service),
        data);
    return std::vector<uint8_t>(tpdu.begin(), tpdu.end());
}

std::vector<uint8_t> ackFromEts(uint8_t seqNum) {
    const auto tpdu = protocol::buildTpdu(protocol::TPCIField::ack(seqNum),
                                          application::APCIField(0),
                                          {});
    return std::vector<uint8_t>(tpdu.begin(), tpdu.end());
}

/// The sequence number of the last T_Data_Connected the device sent, if any.
std::optional<uint8_t> lastConnectedDataSeq(const std::vector<SentFrame>& sent) {
    for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
        if (it->tpci == protocol::TPCI::NumberedData) {
            return it->sequenceNumber;
        }
    }
    return std::nullopt;
}

struct EtsRequest {
    application::APCIService service;
    std::vector<uint8_t> data;
};

/// The first four telegrams of an ETS download, in the order ETS sends them.
const std::vector<EtsRequest>& etsDownloadPrologue() {
    static const std::vector<EtsRequest> requests = {
        {application::APCIService::DeviceDescriptorRead, {}},
        // PID_MAX_APDU_LENGTH (56) of the device object.
        {application::APCIService::PropertyValueRead, {0x00, 0x38, 0x10, 0x01}},
        {application::APCIService::AuthorizeRequest, {0xFF, 0xFF, 0xFF, 0xFF}},
        // PID_TABLE (23) of the association table object.
        {application::APCIService::PropertyDescriptionRead, {0x02, 0x17, 0x00}},
        {application::APCIService::PropertyDescriptionRead, {0x02, 0x17, 0x00}},
    };
    return requests;
}

/**
 * @brief Drive the prologue and return the seq of each answer
 *
 * @param dropAckFromRequest index of the first request whose T_ACK is thrown
 *        away before it reaches the device; requests from there on are left
 *        unacknowledged, as a run of lost multicast frames would.
 */
std::vector<uint8_t> runDownloadPrologue(size_t dropAckFromRequest) {
    injectFromEts(std::vector<uint8_t>{0x80});  // T_Connect
    (void)pumpAndCollect();

    std::vector<uint8_t> responseSequenceNumbers;
    const auto& requests = etsDownloadPrologue();
    for (size_t i = 0; i < requests.size(); ++i) {
        injectFromEts(numberedRequest(static_cast<uint8_t>(i), requests[i].service, requests[i].data));
        const auto sent = pumpAndCollect();

        const auto responseSeq = lastConnectedDataSeq(sent);
        // No answer at all would make the sequence assertions below vacuous.
        TEST_ASSERT_TRUE(responseSeq.has_value());
        responseSequenceNumbers.push_back(*responseSeq);

        if (i >= dropAckFromRequest) {
            continue;  // T_ACK lost on the way to the device
        }
        injectFromEts(ackFromEts(*responseSeq));
        (void)pumpAndCollect(50);
    }
    return responseSequenceNumbers;
}

} // namespace

void setUp(void) {
    clearPersistenceStore();

    gPlatform = std::make_unique<knx::platform::LinuxPlatform>();
    auto ownedPhysical = std::make_unique<MockPhysicalLayer>();
    gPhysical = ownedPhysical.get();
    auto stackPort = knx::test::createTp1TestStackPort(*gPlatform, std::move(ownedPhysical));
    gBau = std::make_unique<knx::bau::BusAccessUnit>(*gPlatform, std::move(stackPort));

    TEST_ASSERT_TRUE(gBau->init(kOwnAddress).isOk());
    gBau->link().setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);
    (void)pumpAndCollect(20);
}

void tearDown(void) {
    if (gBau) {
        gBau->close();
        gBau.reset();
    }
    gPhysical = nullptr;
    gPlatform.reset();
}

// The happy path: every answer carries the next SeqNoSend.
void test_ets_prologue_numbers_every_response_in_order(void) {
    const auto sequenceNumbers = runDownloadPrologue(etsDownloadPrologue().size());

    TEST_ASSERT_EQUAL_UINT32(5, sequenceNumbers.size());
    for (uint8_t i = 0; i < sequenceNumbers.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(i, sequenceNumbers[i]);
    }
}

/**
 * A T_ACK that never arrives must not stall SeqNoSend.
 *
 * Regression: the counter used to advance only in handleAck(), so one lost
 * T_ACK froze it. Every later answer then repeated a sequence number the peer
 * had already consumed, the peer discarded them as duplicates, and ETS gave up
 * on the download after two application-level timeouts.
 */
void test_lost_ack_does_not_freeze_send_sequence(void) {
    const auto sequenceNumbers = runDownloadPrologue(2);

    TEST_ASSERT_EQUAL_UINT32(5, sequenceNumbers.size());
    for (uint8_t i = 0; i < sequenceNumbers.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(i, sequenceNumbers[i]);
    }
}

// The device's receive side is unaffected: each request is still T_ACKed with
// the sequence number it carried, whether or not our own answers get through.
void test_requests_are_acked_with_their_own_sequence_number(void) {
    injectFromEts(std::vector<uint8_t>{0x80});
    (void)pumpAndCollect();

    const auto& requests = etsDownloadPrologue();
    for (size_t i = 0; i < requests.size(); ++i) {
        injectFromEts(numberedRequest(static_cast<uint8_t>(i), requests[i].service, requests[i].data));
        const auto sent = pumpAndCollect();

        bool acked = false;
        for (const auto& frame : sent) {
            if (frame.tpci == protocol::TPCI::NumberedControl) {
                TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(i), frame.sequenceNumber);
                acked = true;
            }
        }
        TEST_ASSERT_TRUE(acked);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ets_prologue_numbers_every_response_in_order);
    RUN_TEST(test_lost_ack_does_not_freeze_send_sequence);
    RUN_TEST(test_requests_are_acked_with_their_own_sequence_number);
    return UNITY_END();
}
