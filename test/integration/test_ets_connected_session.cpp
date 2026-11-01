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
#include "knx/objects/security_interface_object.hpp"
#include "knx/objects/table_segments.hpp"

#include "../mocks/mock_physical_layer.hpp"

#include <algorithm>
#include <array>
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
/// A_Restart ends every commissioning sequence, and LinuxPlatform::restart()
/// exits the process. Record the request instead.
class RecordingPlatform final : public knx::platform::LinuxPlatform {
public:
    void restart() override { restartRequested = true; }
    bool restartRequested{false};
};

std::unique_ptr<RecordingPlatform> gPlatform;
std::unique_ptr<knx::bau::BusAccessUnit> gBau;

/// One frame the device put on the wire, reduced to what this test asserts on.
struct SentFrame {
    protocol::TPCI tpci{};
    uint8_t sequenceNumber{0};
    uint16_t apci{0};
    uint16_t destination{0};
    std::vector<uint8_t> payload;  // APDU octets after the two header octets
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
            sent.push_back(SentFrame{header.tpci.type(), header.tpci.seqNum(), header.apci.raw,
                                     decoded.destination.raw,
                                     std::vector<uint8_t>(decoded.tpdu.begin() + (decoded.tpdu.size() > 2u ? 2 : decoded.tpdu.size()),
                                                          decoded.tpdu.end())});
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

    gPlatform = std::make_unique<RecordingPlatform>();
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

// ---------------------------------------------------------------------------
// The whole ETS commissioning sequence, through the real stack.
//
// Guard rail for the communication-mode check (03/03/07 Table 1): every
// service below arrives on the mode ETS really uses for it — the
// individual-address services as broadcasts, a connectionless device
// descriptor read, then the download on a transport connection — and every
// one of them must still be answered.
// ---------------------------------------------------------------------------
namespace {

constexpr IndividualAddress kUnaddressed(15, 15, 255);
constexpr IndividualAddress kCommissionedAddress(1, 1, 7);

void injectTpdu(IndividualAddress source,
                uint16_t destination,
                AddressType destinationType,
                std::span<const uint8_t> tpdu) {
    datalink::LDataFrame frame;
    frame.source = source;
    frame.destination = GroupAddress(destination);
    frame.destinationType = destinationType;
    frame.priority = Priority::System;
    frame.hopCount = 6;
    frame.tpdu.assign(tpdu);

    std::array<uint8_t, datalink::LDataFrame::MAX_FRAME_SIZE> buffer{};
    auto encoded = datalink::FrameCodec::encodeFrame(frame, buffer);
    TEST_ASSERT_TRUE(encoded.isOk());
    gPhysical->injectFrame(std::vector<uint8_t>(buffer.begin(), buffer.begin() + encoded.value()));
}

std::vector<uint8_t> unnumbered(application::APCIService service,
                                uint8_t data6 = 0,
                                std::vector<uint8_t> payload = {}) {
    const auto tpdu = protocol::buildTpdu(
        protocol::TPCIField::create(protocol::TPCI::UnnumberedData),
        application::APCIField::create(service, data6),
        std::span<const uint8_t>(payload));
    return std::vector<uint8_t>(tpdu.begin(), tpdu.end());
}

std::vector<uint8_t> numbered(uint8_t seq,
                              application::APCIService service,
                              uint8_t data6 = 0,
                              std::vector<uint8_t> payload = {}) {
    const auto tpdu = protocol::buildTpdu(
        protocol::TPCIField::create(protocol::TPCI::NumberedData, seq),
        application::APCIField::create(service, data6),
        std::span<const uint8_t>(payload));
    return std::vector<uint8_t>(tpdu.begin(), tpdu.end());
}

/// The first application frame the device sent whose APCI, masked with
/// @p apciMask, equals @p apci.
const SentFrame* findResponse(const std::vector<SentFrame>& sent, uint16_t apciMask, uint16_t apci) {
    for (const auto& frame : sent) {
        if ((frame.tpci == protocol::TPCI::UnnumberedData || frame.tpci == protocol::TPCI::NumberedData)
            && (frame.apci & apciMask) == apci) {
            return &frame;
        }
    }
    return nullptr;
}

constexpr uint16_t kGroupMask = 0x3C0u;
constexpr uint16_t kFullMask = 0x3FFu;

/// One connected request from ETS; returns what the device sent back, after
/// acknowledging the device's numbered answer the way ETS does.
std::vector<SentFrame> connectedRequest(uint8_t seq,
                                        application::APCIService service,
                                        uint8_t data6 = 0,
                                        std::vector<uint8_t> payload = {}) {
    const auto request = numbered(seq, service, data6, std::move(payload));
    injectTpdu(kEtsAddress, kCommissionedAddress.raw, AddressType::Individual, request);
    auto sent = pumpAndCollect();
    for (const auto& frame : sent) {
        if (frame.tpci == protocol::TPCI::NumberedData) {
            const auto ack = protocol::buildTpdu(protocol::TPCIField::ack(frame.sequenceNumber),
                                                 application::APCIField(0),
                                                 std::span<const uint8_t>{});
            injectTpdu(kEtsAddress, kCommissionedAddress.raw, AddressType::Individual,
                       std::vector<uint8_t>(ack.begin(), ack.end()));
            (void)pumpAndCollect(30);
        }
    }
    return sent;
}

/// Broadcast address assignment and the connectionless identity check that
/// precede every ETS download.
void assignAddressLikeEts() {
    gBau->deviceObject().setProgMode(Toggle::Enable);
    (void)pumpAndCollect(20);

    // NM_IndividualAddress_Read: who is in programming mode?
    injectTpdu(kEtsAddress, 0x0000u, AddressType::Group,
               unnumbered(application::APCIService::IndividualAddressRead));
    auto sent = pumpAndCollect();
    TEST_ASSERT_NOT_NULL(findResponse(sent, kGroupMask,
        static_cast<uint16_t>(application::APCIService::IndividualAddressResponse)));

    // NM_IndividualAddress_Write, broadcast.
    injectTpdu(kEtsAddress, 0x0000u, AddressType::Group,
               unnumbered(application::APCIService::IndividualAddressWrite, 0,
                          {static_cast<uint8_t>(kCommissionedAddress.raw >> 8),
                           static_cast<uint8_t>(kCommissionedAddress.raw & 0xFFu)}));
    (void)pumpAndCollect();
    TEST_ASSERT_EQUAL_HEX16(kCommissionedAddress.raw, gBau->deviceObject().readIndividualAddress().raw);

    // Verify: read it back, broadcast again.
    injectTpdu(kEtsAddress, 0x0000u, AddressType::Group,
               unnumbered(application::APCIService::IndividualAddressRead));
    sent = pumpAndCollect();
    TEST_ASSERT_NOT_NULL(findResponse(sent, kGroupMask,
        static_cast<uint16_t>(application::APCIService::IndividualAddressResponse)));

    // Connectionless A_DeviceDescriptor_Read to the new address.
    injectTpdu(kEtsAddress, kCommissionedAddress.raw, AddressType::Individual,
               unnumbered(application::APCIService::DeviceDescriptorRead, 0));
    sent = pumpAndCollect();
    const auto* dd = findResponse(sent, kGroupMask,
        static_cast<uint16_t>(application::APCIService::DeviceDescriptorResponse));
    TEST_ASSERT_NOT_NULL(dd);
    TEST_ASSERT_EQUAL_HEX16(0x340u, dd->apci);  // descriptor type 0
    TEST_ASSERT_TRUE(dd->tpci == protocol::TPCI::UnnumberedData);

    gBau->deviceObject().setProgMode(Toggle::Disable);
}

/// The connected part of a download. With @p verifyMode, ETS first switches
/// Verify Mode on (DMP_MemWrite_RCoV, 03/05/02 §3.16.3) and expects each
/// A_Memory_Write to be answered with the read-back; without it
/// (DMP_MemWrite_RCo) the write is unanswered and ETS reads back itself.
void downloadLikeEts(bool verifyMode) {
    injectTpdu(kEtsAddress, kCommissionedAddress.raw, AddressType::Individual,
               std::vector<uint8_t>{0x80});  // T_Connect
    (void)pumpAndCollect();

    uint8_t seq = 0;
    auto sent = connectedRequest(seq++, application::APCIService::DeviceDescriptorRead, 0);
    TEST_ASSERT_NOT_NULL(findResponse(sent, kGroupMask, 0x340u));

    sent = connectedRequest(seq++, application::APCIService::AuthorizeRequest, 0,
                            {0x00, 0xFF, 0xFF, 0xFF, 0xFF});
    const auto* auth = findResponse(sent, kFullMask,
        static_cast<uint16_t>(application::APCIService::AuthorizeResponse));
    TEST_ASSERT_NOT_NULL(auth);
    TEST_ASSERT_EQUAL_UINT8(0u, auth->payload.at(0));  // unkeyed: level 0

    if (verifyMode) {
        // PID_DEVICE_CONTROL (14) of the Device Object, bit 2 = Verify Mode.
        sent = connectedRequest(seq++, application::APCIService::PropertyValueWrite, 0,
                                {0x00, 0x0E, 0x10, 0x01, 0x04});
        const auto* written = findResponse(sent, kFullMask,
            static_cast<uint16_t>(application::APCIService::PropertyValueResponse));
        TEST_ASSERT_NOT_NULL(written);
        TEST_ASSERT_EQUAL_HEX8(0x10, written->payload.at(2) & 0xF0u);  // one element back
    }

    // PID_LOAD_STATE_CONTROL (5) of the Group Address Table (index 1): StartLoading.
    sent = connectedRequest(seq++, application::APCIService::PropertyValueWrite, 0,
                            {0x01, 0x05, 0x10, 0x01, 0x01});
    TEST_ASSERT_NOT_NULL(findResponse(sent, kFullMask,
        static_cast<uint16_t>(application::APCIService::PropertyValueResponse)));

    // The table itself, by A_Memory_Write into its segment: one entry, 1/2/3.
    const uint16_t base = objects::tableseg::kGroupAddressTableBase;
    const std::vector<uint8_t> table = {0x00, 0x01, 0x0A, 0x03};
    std::vector<uint8_t> writePayload = {static_cast<uint8_t>(base >> 8), static_cast<uint8_t>(base & 0xFFu)};
    writePayload.insert(writePayload.end(), table.begin(), table.end());
    sent = connectedRequest(seq++, application::APCIService::MemoryWrite,
                            static_cast<uint8_t>(table.size()), writePayload);
    const auto* writeAnswer = findResponse(sent, kGroupMask,
        static_cast<uint16_t>(application::APCIService::MemoryResponse));
    if (verifyMode) {
        TEST_ASSERT_NOT_NULL(writeAnswer);
        TEST_ASSERT_EQUAL_HEX16(0x240u | table.size(), writeAnswer->apci);
        TEST_ASSERT_EQUAL_UINT32(2u + table.size(), writeAnswer->payload.size());
        TEST_ASSERT_EQUAL_HEX8(0x0A, writeAnswer->payload.at(4));
    } else {
        TEST_ASSERT_NULL(writeAnswer);
    }

    // ETS's own verification read.
    sent = connectedRequest(seq++, application::APCIService::MemoryRead,
                            static_cast<uint8_t>(table.size()),
                            {static_cast<uint8_t>(base >> 8), static_cast<uint8_t>(base & 0xFFu)});
    const auto* readBack = findResponse(sent, kGroupMask,
        static_cast<uint16_t>(application::APCIService::MemoryResponse));
    TEST_ASSERT_NOT_NULL(readBack);
    TEST_ASSERT_EQUAL_HEX16(0x240u | table.size(), readBack->apci);
    TEST_ASSERT_EQUAL_HEX8(0x03, readBack->payload.at(5));

    // LoadCompleted applies the segment.
    sent = connectedRequest(seq++, application::APCIService::PropertyValueWrite, 0,
                            {0x01, 0x05, 0x10, 0x01, 0x02});
    TEST_ASSERT_NOT_NULL(findResponse(sent, kFullMask,
        static_cast<uint16_t>(application::APCIService::PropertyValueResponse)));
    TEST_ASSERT_EQUAL_UINT16(1u, gBau->addressTable().entryCount());

    // A_Restart ends the download.
    TEST_ASSERT_FALSE(gPlatform->restartRequested);
    (void)connectedRequest(seq++, application::APCIService::Restart, 0);
    TEST_ASSERT_TRUE(gPlatform->restartRequested);
}

} // namespace

void test_full_ets_commissioning_sequence_without_verify_mode(void) {
    assignAddressLikeEts();
    downloadLikeEts(false);
}

void test_full_ets_commissioning_sequence_with_verify_mode(void) {
    assignAddressLikeEts();
    downloadLikeEts(true);
}

// The same services on the wrong communication mode are not executed: a
// broadcast A_Memory_Write and a group-addressed A_PropertyValue_Write reach
// the device (promiscuous link layer) but change nothing.
void test_management_writes_on_the_wrong_mode_are_ignored(void) {
    const uint16_t base = objects::tableseg::kGroupAddressTableBase;
    injectTpdu(kEtsAddress, 0x0000u, AddressType::Group,
               unnumbered(application::APCIService::MemoryWrite, 2,
                          {static_cast<uint8_t>(base >> 8), static_cast<uint8_t>(base & 0xFFu), 0xAB, 0xCD}));
    // PID_DEVICE_CONTROL write to group 1/2/3.
    injectTpdu(kEtsAddress, 0x0A03u, AddressType::Group,
               unnumbered(application::APCIService::PropertyValueWrite, 0,
                          {0x00, 0x0E, 0x10, 0x01, 0x04}));
    const auto sent = pumpAndCollect();
    TEST_ASSERT_TRUE(sent.empty());
    TEST_ASSERT_EQUAL_HEX8(0x00, gBau->deviceObject().getDeviceControl());

    // The broadcast write did not land: read it back the legitimate way.
    injectTpdu(kEtsAddress, kOwnAddress.raw, AddressType::Individual,
               unnumbered(application::APCIService::MemoryRead, 2,
                          {static_cast<uint8_t>(base >> 8), static_cast<uint8_t>(base & 0xFFu)}));
    const auto readSent = pumpAndCollect();
    const auto* readBack = findResponse(readSent, kGroupMask, 0x240u);
    TEST_ASSERT_NOT_NULL(readBack);
    TEST_ASSERT_EQUAL_HEX8(0x00, readBack->payload.at(2));
    TEST_ASSERT_EQUAL_HEX8(0x00, readBack->payload.at(3));
}

// ---------------------------------------------------------------------------
// Master Reset erase codes (03/05/02 §3.7.1.2.3), persisted across a reboot.
// ---------------------------------------------------------------------------
namespace {

const std::array<uint8_t, 16> kEtsToolKey{
    0x4E, 0x51, 0xA5, 0xC0, 0xA1, 0xA0, 0x4A, 0xB8,
    0x74, 0xED, 0xDB, 0xC0, 0xF6, 0xF1, 0xE9, 0x7C};
constexpr uint64_t kSequenceNumberSending = 5000u;

/// A device in the state ETS leaves it in: addressed, linked, parameterised
/// and secured, and persisted, as it would be before the reset arrives.
void commissionForReset() {
    TEST_ASSERT_TRUE(gBau->deviceObject().applyIndividualAddress(kCommissionedAddress).isOk());
    (void)gBau->addressTable().addEntry(GroupAddress(0x0A03u));
    const std::array<uint8_t, 4> parameters{0x11, 0x22, 0x33, 0x44};
    gBau->applicationProgram().setProgramData(parameters);
    TEST_ASSERT_TRUE(gBau->writeProperty(
        InterfaceObjectType::security(), InterfaceObjectInstance(1),
        static_cast<application::PropertyID>(objects::SecurityProperty::ToolKey), 1,
        std::span<const uint8_t>(kEtsToolKey)).isOk());
    // Security Mode stays off: with it on, the plain A_Restart below would be
    // refused by the Access Policies before it got here (as it should be).
    gBau->securityObject().setSendingSequence(kSequenceNumberSending);
    gBau->flushPendingPersistence();
    (void)pumpAndCollect(20);
}

/// A_Restart Master Reset, connectionless, the way DM_Restart_RCl sends it.
std::vector<SentFrame> masterReset(uint8_t eraseCode, uint8_t channel = 0, uint8_t extraApciBits = 0) {
    injectTpdu(kEtsAddress, kCommissionedAddress.raw, AddressType::Individual,
               unnumbered(application::APCIService::Restart,
                          static_cast<uint8_t>(0x01u | extraApciBits),
                          {eraseCode, channel}));
    return pumpAndCollect();
}

/// The A_Restart_Response error code, or -1 when there was no response.
int restartErrorCode(const std::vector<SentFrame>& sent) {
    const auto* response = findResponse(sent, kFullMask,
        static_cast<uint16_t>(application::APCIService::RestartResponse));
    if (response == nullptr || response->payload.empty()) {
        return -1;
    }
    return response->payload[0];
}

/// What the device boots into: a fresh BAU on the same persisted state,
/// started with the medium's default address as a product does.
void rebootFromPersistence() {
    if (gBau) {
        gBau->close();
        gBau.reset();
    }
    gPlatform = std::make_unique<RecordingPlatform>();
    auto ownedPhysical = std::make_unique<MockPhysicalLayer>();
    gPhysical = ownedPhysical.get();
    auto stackPort = knx::test::createTp1TestStackPort(*gPlatform, std::move(ownedPhysical));
    gBau = std::make_unique<knx::bau::BusAccessUnit>(*gPlatform, std::move(stackPort));
    TEST_ASSERT_TRUE(gBau->init(kUnaddressed).isOk());
    gBau->link().setPromiscuousMode(knx::datalink::PromiscuousMode::Enable);
    (void)pumpAndCollect(20);
}

/// setUp() started the device on kOwnAddress; reset tests need it started on
/// the default address, which is what Erase Codes 02h/03h go back to.
void startUnaddressed() {
    gBau->close();
    gBau.reset();
    clearPersistenceStore();
    rebootFromPersistence();
}

bool toolKeyCleared() {
    const auto& key = gBau->securityObject().getToolKey();
    return std::all_of(key.begin(), key.end(), [](uint8_t b) { return b == 0u; });
}

} // namespace

// 02h Factory Reset: back to ex-factory, including the address. The Tool Key
// is cleared so that the product's boot path installs the FDSK again, and the
// Sequence Number Sending survives (03/03/07 §5, "Sequence Number Sending and
// Master Reset": not influenced).
void test_master_reset_factory_reset_restores_ex_factory_state(void) {
    startUnaddressed();
    commissionForReset();

    const auto sent = masterReset(0x02);
    TEST_ASSERT_EQUAL_INT(0x00, restartErrorCode(sent));
    TEST_ASSERT_TRUE(gPlatform->restartRequested);

    rebootFromPersistence();
    TEST_ASSERT_EQUAL_HEX16(kUnaddressed.raw, gBau->deviceObject().readIndividualAddress().raw);
    TEST_ASSERT_EQUAL_UINT16(0u, gBau->addressTable().entryCount());
    TEST_ASSERT_EQUAL_UINT32(0u, gBau->applicationProgram().getProgramData().size());
    TEST_ASSERT_TRUE(toolKeyCleared());
    TEST_ASSERT_FALSE(gBau->securityObject().isSecurityEnabled());
    TEST_ASSERT_TRUE(gBau->securityObject().getSendingSequence() >= kSequenceNumberSending);
}

// 07h is 02h without the Individual Address (03/05/02 Table 6).
void test_master_reset_factory_reset_without_ia_keeps_the_address(void) {
    startUnaddressed();
    commissionForReset();

    TEST_ASSERT_EQUAL_INT(0x00, restartErrorCode(masterReset(0x07)));

    rebootFromPersistence();
    TEST_ASSERT_EQUAL_HEX16(kCommissionedAddress.raw, gBau->deviceObject().readIndividualAddress().raw);
    TEST_ASSERT_EQUAL_UINT16(0u, gBau->addressTable().entryCount());
    TEST_ASSERT_TRUE(toolKeyCleared());
}

// 03h ResetIA touches the address only.
void test_master_reset_reset_ia_resets_only_the_address(void) {
    startUnaddressed();
    commissionForReset();

    TEST_ASSERT_EQUAL_INT(0x00, restartErrorCode(masterReset(0x03)));

    rebootFromPersistence();
    TEST_ASSERT_EQUAL_HEX16(kUnaddressed.raw, gBau->deviceObject().readIndividualAddress().raw);
    TEST_ASSERT_EQUAL_UINT16(1u, gBau->addressTable().entryCount());
    TEST_ASSERT_EQUAL_UINT32(4u, gBau->applicationProgram().getProgramData().size());
    TEST_ASSERT_FALSE(toolKeyCleared());
}

// 04h ResetAP and 05h ResetParam: the downloaded parameter block goes, the
// links and the keys stay.
void test_master_reset_reset_ap_and_param_drop_the_parameters(void) {
    for (const uint8_t eraseCode : {uint8_t{0x04}, uint8_t{0x05}}) {
        startUnaddressed();
        commissionForReset();

        TEST_ASSERT_EQUAL_INT(0x00, restartErrorCode(masterReset(eraseCode)));

        rebootFromPersistence();
        TEST_ASSERT_EQUAL_UINT32(0u, gBau->applicationProgram().getProgramData().size());
        TEST_ASSERT_EQUAL_UINT16(1u, gBau->addressTable().entryCount());
        TEST_ASSERT_EQUAL_HEX16(kCommissionedAddress.raw, gBau->deviceObject().readIndividualAddress().raw);
        TEST_ASSERT_FALSE(toolKeyCleared());
    }
}

// 06h ResetLinks: the group address and association tables only.
void test_master_reset_reset_links_clears_the_tables(void) {
    startUnaddressed();
    commissionForReset();

    TEST_ASSERT_EQUAL_INT(0x00, restartErrorCode(masterReset(0x06)));

    rebootFromPersistence();
    TEST_ASSERT_EQUAL_UINT16(0u, gBau->addressTable().entryCount());
    TEST_ASSERT_EQUAL_UINT32(4u, gBau->applicationProgram().getProgramData().size());
    TEST_ASSERT_FALSE(toolKeyCleared());
}

// 01h Confirmed Restart resets nothing.
void test_master_reset_confirmed_restart_resets_nothing(void) {
    startUnaddressed();
    commissionForReset();

    TEST_ASSERT_EQUAL_INT(0x00, restartErrorCode(masterReset(0x01)));
    TEST_ASSERT_TRUE(gPlatform->restartRequested);

    rebootFromPersistence();
    TEST_ASSERT_EQUAL_HEX16(kCommissionedAddress.raw, gBau->deviceObject().readIndividualAddress().raw);
    TEST_ASSERT_EQUAL_UINT16(1u, gBau->addressTable().entryCount());
    TEST_ASSERT_FALSE(toolKeyCleared());
}

// Reserved (00h, 09h…FFh) and unimplemented (08h) erase codes: "Unsupported
// Erase Code", and neither a reset nor a restart.
void test_master_reset_unsupported_erase_codes_are_refused(void) {
    startUnaddressed();
    commissionForReset();

    for (const uint8_t eraseCode : {uint8_t{0x00}, uint8_t{0x08}, uint8_t{0x09}, uint8_t{0xFF}}) {
        TEST_ASSERT_EQUAL_INT(0x02, restartErrorCode(masterReset(eraseCode)));
    }
    TEST_ASSERT_FALSE(gPlatform->restartRequested);
    TEST_ASSERT_EQUAL_UINT16(1u, gBau->addressTable().entryCount());
}

// A Channel Number on a device without application channels: "Invalid
// Channel Number", nothing reset.
void test_master_reset_with_a_channel_is_refused(void) {
    startUnaddressed();
    commissionForReset();

    TEST_ASSERT_EQUAL_INT(0x03, restartErrorCode(masterReset(0x02, 0x01)));
    TEST_ASSERT_FALSE(gPlatform->restartRequested);
    TEST_ASSERT_EQUAL_UINT16(1u, gBau->addressTable().entryCount());
}

// Reserved bits 4..1 of octet 7 set: the request is ignored totally, without
// a negative response (03/03/07 §3.4.2.2).
void test_master_reset_with_reserved_bits_is_ignored(void) {
    startUnaddressed();
    commissionForReset();

    const auto sent = masterReset(0x02, 0x00, 0x02);
    TEST_ASSERT_EQUAL_INT(-1, restartErrorCode(sent));
    TEST_ASSERT_FALSE(gPlatform->restartRequested);
    TEST_ASSERT_EQUAL_UINT16(1u, gBau->addressTable().entryCount());
}

// Verify Mode does not outlive a reboot (03/05/01 §4.2.14.4: Device Control
// is 00h at start-up), even though the Device Object persists the property.
void test_device_control_is_cleared_at_start_up(void) {
    startUnaddressed();
    gBau->deviceObject().setDeviceControl(0x04);
    commissionForReset();  // persists the Device Object with it

    rebootFromPersistence();
    TEST_ASSERT_EQUAL_HEX8(0x00, gBau->deviceObject().getDeviceControl());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_full_ets_commissioning_sequence_without_verify_mode);
    RUN_TEST(test_full_ets_commissioning_sequence_with_verify_mode);
    RUN_TEST(test_management_writes_on_the_wrong_mode_are_ignored);
    RUN_TEST(test_master_reset_factory_reset_restores_ex_factory_state);
    RUN_TEST(test_master_reset_factory_reset_without_ia_keeps_the_address);
    RUN_TEST(test_master_reset_reset_ia_resets_only_the_address);
    RUN_TEST(test_master_reset_reset_ap_and_param_drop_the_parameters);
    RUN_TEST(test_master_reset_reset_links_clears_the_tables);
    RUN_TEST(test_master_reset_confirmed_restart_resets_nothing);
    RUN_TEST(test_master_reset_unsupported_erase_codes_are_refused);
    RUN_TEST(test_master_reset_with_a_channel_is_refused);
    RUN_TEST(test_master_reset_with_reserved_bits_is_ignored);
    RUN_TEST(test_device_control_is_cleared_at_start_up);
    RUN_TEST(test_ets_prologue_numbers_every_response_in_order);
    RUN_TEST(test_lost_ack_does_not_freeze_send_sequence);
    RUN_TEST(test_requests_are_acked_with_their_own_sequence_number);
    return UNITY_END();
}
