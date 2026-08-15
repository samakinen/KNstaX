// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/objects/security_interface_object.hpp"
#include "knx/security/secure_application_layer.hpp"

#include <array>
#include <cstring>
#include <optional>
#include <vector>

// ============================================================================
// KNX Data Secure S-AL known-answer tests
//
// Ground truth: KNX Standard v2.1, 03/03/07 "Application Layer" Annex C, which
// works the CCM calculation out octet by octet:
//
//   C.1.1  S-A_Data-PDU     (A_PropertyValue_Write, Tool Access, connectionless)
//   C.1.3  S-A_Sync.req     (Tool Access, on a transport connection)
//   C.1.4  S-A_Sync.res     (the answer to C.1.3, with Random = AA..AA)
//
// C.1.3 and C.1.4 are a matched pair, so feeding the request in and comparing
// the emitted response against the specified bytes exercises the whole sync
// service: SCF handling, serial-number check, CCM verification, the sequence
// number arithmetic and the Challenge XOR Random.
// ============================================================================

using namespace knx;
using namespace knx::security;

namespace {

const SecureApplicationLayer::Key kToolKey{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

struct Harness {
    /// Fixed Random and a clock under test control: the specification's worked
    /// example fixes Random = AA..AA, and the one-per-second rule needs time to
    /// stand still unless a test moves it.
    uint8_t randomValue{0xAA};
    uint32_t nowMs{1000};
    uint8_t responseTpci6{0x10};

    objects::SecurityInterfaceObject securityObject;
    std::optional<SecureApplicationLayer> sal;

    std::vector<uint8_t> sentTpdu;
    SecureFrameInfo sentInfo{};
    bool sent{false};

    Harness() {
        securityObject.setSecurityMode(objects::SecurityMode::Enabled);
        securityObject.setToolKey(kToolKey);
        sal.emplace(securityObject,
                    [this]() { return nowMs; },
                    [this](std::span<uint8_t> out) {
                        for (auto& byte : out) {
                            byte = randomValue;
                        }
                    });
        sal->setOwnAddress(IndividualAddress(0xFF00));
        sal->setSerialNumber({0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
        // C.1.4's response goes out on the connection with SeqNo 0, so its TPCI
        // is 010000b — the transport is what knows this on a real device.
        sal->setTpciResolver([this](const SecureFrameInfo& info) -> uint8_t {
            return info.connected ? responseTpci6 : 0u;
        });
        sal->setFrameSink([this](const SecureFrameInfo& info,
                                 std::span<const uint8_t> tpdu) -> util::Result<void> {
            sentInfo = info;
            sentTpdu.assign(tpdu.begin(), tpdu.end());
            sent = true;
            return util::Result<void>::ok();
        });
    }
};

std::string hex(std::span<const uint8_t> bytes) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0x0F]);
    }
    return out;
}

void assertBytes(const char* what, std::span<const uint8_t> expected, std::span<const uint8_t> actual) {
    if (expected.size() != actual.size()
            || std::memcmp(expected.data(), actual.data(), expected.size()) != 0) {
        std::string message = std::string(what) + ": expected " + hex(expected)
                            + " but got " + hex(actual);
        TEST_FAIL_MESSAGE(message.c_str());
    }
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

// --- C.1.1 -----------------------------------------------------------------
// A_PropertyValue_Write towards 15.15.0, secured with the Tool Key.
void test_spec_c11_data_apdu_is_verified_and_decrypted(void) {
    Harness harness;
    harness.sal->setOwnAddress(IndividualAddress(0xFF00));

    // 03 F1 | SCF 90 | SeqNr 000000000004 | cipher+MAC
    const std::vector<uint8_t> tpdu{
        0x03, 0xF1, 0x90,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
        0x67, 0x67, 0x24, 0x2a, 0x23, 0x08, 0xca, 0x76, 0xa1, 0x17, 0x74, 0x21,
        0x4e, 0xe4, 0xcf, 0x5d, 0x94, 0x90, 0x9f, 0x74, 0x3d, 0x05,
        0x0d, 0x8f, 0xc1, 0x68};

    SecureFrameInfo info;
    info.source = IndividualAddress(0xFF67);
    info.destination = 0xFF00;
    info.destinationType = AddressType::Individual;
    info.tpci6 = 0;

    std::array<uint8_t, 64> plain{};
    const auto result = harness.sal->processIncoming(info, tpdu, plain);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Unwrapped),
                          static_cast<int>(result.disposition));

    const std::vector<uint8_t> expectedPlain{
        0x03, 0xD7, 0x05, 0x35, 0x10, 0x01,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
    assertBytes("plain APDU", expectedPlain, std::span<const uint8_t>(plain.data(), result.plainLength));

    // The sequence number is now the last valid one for Tool Access.
    TEST_ASSERT_EQUAL_UINT64(4u, harness.securityObject.getToolAccessSequence());

    // SCF 90h is Tool Access with authentication and confidentiality. Reporting
    // that upwards is what lets the Access Policies distinguish this request
    // from a plain one that carries the same APDU (03/4/1 §6.2).
    TEST_ASSERT_TRUE(result.security.secured);
    TEST_ASSERT_TRUE(result.security.toolAccess);
    TEST_ASSERT_TRUE(result.security.confidentiality);
    TEST_ASSERT_TRUE(result.security.toolSecured());
}

void test_data_apdu_with_replayed_sequence_is_rejected(void) {
    Harness harness;
    harness.securityObject.setToolAccessSequence(4u);

    const std::vector<uint8_t> tpdu{
        0x03, 0xF1, 0x90,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
        0x67, 0x67, 0x24, 0x2a, 0x23, 0x08, 0xca, 0x76, 0xa1, 0x17, 0x74, 0x21,
        0x4e, 0xe4, 0xcf, 0x5d, 0x94, 0x90, 0x9f, 0x74, 0x3d, 0x05,
        0x0d, 0x8f, 0xc1, 0x68};

    SecureFrameInfo info;
    info.source = IndividualAddress(0xFF67);
    info.destination = 0xFF00;
    info.destinationType = AddressType::Individual;

    std::array<uint8_t, 64> plain{};
    const auto result = harness.sal->processIncoming(info, tpdu, plain);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Rejected),
                          static_cast<int>(result.disposition));
}

void test_tampered_data_apdu_is_rejected(void) {
    Harness harness;

    std::vector<uint8_t> tpdu{
        0x03, 0xF1, 0x90,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
        0x67, 0x67, 0x24, 0x2a, 0x23, 0x08, 0xca, 0x76, 0xa1, 0x17, 0x74, 0x21,
        0x4e, 0xe4, 0xcf, 0x5d, 0x94, 0x90, 0x9f, 0x74, 0x3d, 0x05,
        0x0d, 0x8f, 0xc1, 0x68};
    tpdu[12] ^= 0x01;  // flip a ciphertext bit

    SecureFrameInfo info;
    info.source = IndividualAddress(0xFF67);
    info.destination = 0xFF00;
    info.destinationType = AddressType::Individual;

    std::array<uint8_t, 64> plain{};
    const auto result = harness.sal->processIncoming(info, tpdu, plain);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Rejected),
                          static_cast<int>(result.disposition));
}

// --- C.1.3 / C.1.4 ---------------------------------------------------------
// The sync request of C.1.3 arrives on a transport connection (TPCI 010000b)
// from 15.15.103; C.1.4 is the response this device has to produce for it.
namespace {

std::vector<uint8_t> specSyncRequest() {
    return {
        0x43, 0xF1, 0x92,                          // TPCI/APCI 43F1, SCF 92
        0x00, 0x00, 0x00, 0x00, 0x00, 0x01,        // SeqNrlocal
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,        // KNX Serial Number
        0xc1, 0xcf, 0x45, 0x06, 0xf0, 0x9b,        // encrypted Challenge
        0xd7, 0x9f, 0xab, 0x55};                   // MAC
}

SecureFrameInfo specSyncRequestInfo() {
    SecureFrameInfo info;
    info.source = IndividualAddress(0xFF67);
    info.destination = 0xFF00;
    info.destinationType = AddressType::Individual;
    info.tpci6 = 0x10;   // 010000b, T_Data_Connected with SeqNo 0
    info.connected = true;
    return info;
}

} // namespace

void test_spec_c14_sync_response_matches_specification(void) {
    Harness harness;
    // C.1.4 states SeqNrremote = 3 as PID_SEQUENCE_NUMBER_SENDING, which is the
    // next number this device will use, and SeqNrlocal = 4 as the next number
    // it will accept from the tool (so 3 is the last valid one).
    harness.securityObject.setSendingSequence(2u);
    harness.securityObject.setToolAccessSequence(3u);
    harness.randomValue = 0xAA;

    std::array<uint8_t, 64> plain{};
    const auto result = harness.sal->processIncoming(specSyncRequestInfo(), specSyncRequest(), plain);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Consumed),
                          static_cast<int>(result.disposition));
    TEST_ASSERT_TRUE(harness.sent);  // an S-A_Sync_Response must have been emitted

    const std::vector<uint8_t> expected{
        0x03, 0xF1, 0x93,                          // SCF 93 = Tool Access, SyncResponse
        0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xa9,        // Challenge XOR Random
        0x9c, 0x02, 0x3a, 0xd2, 0x5e, 0x14,        // encrypted SeqNrremote
        0x64, 0x70, 0x69, 0x3e, 0x63, 0x8d,        // encrypted SeqNrlocal
        0x5b, 0x70, 0xca, 0xc4};                   // MAC
    assertBytes("S-A_Sync_Response", expected, harness.sentTpdu);

    // The answer goes back the way the request came: on the connection.
    TEST_ASSERT_TRUE(harness.sentInfo.connected);
    TEST_ASSERT_EQUAL_UINT16(0xFF67, harness.sentInfo.destination);
}

void test_sync_request_raises_stored_sequence_but_not_our_own(void) {
    Harness harness;
    harness.securityObject.setSendingSequence(2u);
    harness.securityObject.setToolAccessSequence(0u);

    std::array<uint8_t, 64> plain{};
    (void)harness.sal->processIncoming(specSyncRequestInfo(), specSyncRequest(), plain);

    // SeqNrlocal in the request is 1, i.e. the tool's *next* number, so the last
    // valid one becomes 0 — it must not go backwards and must not be raised
    // beyond what the tool announced.
    TEST_ASSERT_EQUAL_UINT64(0u, harness.securityObject.getToolAccessSequence());
    // Answering a sync must not consume our own sending sequence (NOTE 44).
    TEST_ASSERT_EQUAL_UINT64(2u, harness.securityObject.getSendingSequenceThreadSafe());
}

void test_second_sync_request_within_one_second_is_ignored(void) {
    Harness harness;
    harness.securityObject.setSendingSequence(2u);
    harness.securityObject.setToolAccessSequence(3u);

    std::array<uint8_t, 64> plain{};
    (void)harness.sal->processIncoming(specSyncRequestInfo(), specSyncRequest(), plain);
    TEST_ASSERT_TRUE(harness.sent);

    harness.sent = false;
    harness.securityObject.setToolAccessSequence(3u);
    (void)harness.sal->processIncoming(specSyncRequestInfo(), specSyncRequest(), plain);
    TEST_ASSERT_FALSE(harness.sent);  // must not answer twice inside one second
}

void test_broadcast_sync_request_for_another_serial_is_ignored(void) {
    Harness harness;
    harness.sal->setSerialNumber({0xE4, 0xB3, 0x23, 0x87, 0x9B, 0x84});

    // Same PDU shape as the spec example, but broadcast and carrying a serial
    // number that is not ours.
    std::vector<uint8_t> tpdu = specSyncRequest();
    tpdu[0] = 0x03;
    tpdu[9] = 0x11;

    SecureFrameInfo info;
    info.source = IndividualAddress(0xFF67);
    info.destination = 0x0000;
    info.destinationType = AddressType::Group;

    std::array<uint8_t, 64> plain{};
    const auto result = harness.sal->processIncoming(info, tpdu, plain);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Consumed),
                          static_cast<int>(result.disposition));
    TEST_ASSERT_FALSE(harness.sent);  // not addressed to this device
}

void test_broadcast_sync_request_without_serial_number_is_ignored(void) {
    Harness harness;
    harness.sal->setSerialNumber({0x00, 0x00, 0x00, 0x00, 0x00, 0x00});

    std::vector<uint8_t> tpdu = specSyncRequest();
    tpdu[0] = 0x03;

    SecureFrameInfo info;
    info.source = IndividualAddress(0xFF67);
    info.destination = 0x0000;
    info.destinationType = AddressType::Group;

    std::array<uint8_t, 64> plain{};
    const auto result = harness.sal->processIncoming(info, tpdu, plain);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Consumed),
                          static_cast<int>(result.disposition));
    // §5.3.2: on broadcast a zero serial number means "not addressed to anyone".
    TEST_ASSERT_FALSE(harness.sent);
}

// A device that ETS has never commissioned still transmits from the initial
// address 15.15.255, and its very first secured frame is the S-A_Sync_Response
// to ETS's broadcast S-A_Sync_Request — so 15.15.255 has to be usable as the
// source of a secured frame. The request below is a real capture from an ETS 6
// secure commissioning attempt (tool key = the device's FDSK); before the CCM
// blocks accepted the initial address the response failed to build with
// "Invalid address" and ETS timed out on "device does not respond".
void test_uncommissioned_device_answers_broadcast_sync_request(void) {
    Harness harness;
    harness.securityObject.setToolKey({0xF4, 0x81, 0x2F, 0x18, 0x5A, 0x48, 0xB0, 0xB2,
                                       0x7D, 0x0D, 0x71, 0xAB, 0x4F, 0xA6, 0x96, 0xC4});
    harness.sal->setOwnAddress(initialIndividualAddress());
    harness.sal->setSerialNumber({0xE4, 0xB3, 0x23, 0x87, 0x9B, 0x84});
    harness.securityObject.setSendingSequence(2u);
    harness.securityObject.setToolAccessSequence(0u);

    const std::vector<uint8_t> tpdu{
        0x03, 0xF1, 0x92,                          // T_Data_Broadcast, SCF 92
        0x00, 0x3F, 0x24, 0x84, 0x63, 0xC6,        // SeqNrlocal
        0xE4, 0xB3, 0x23, 0x87, 0x9B, 0x84,        // our KNX Serial Number
        0xA9, 0xED, 0xFF, 0xB3, 0xCD, 0x75,        // encrypted Challenge
        0x61, 0xA0, 0x0D, 0x35};                   // MAC

    SecureFrameInfo info;
    info.source = IndividualAddress(0x11FE);
    info.destination = 0x0000;
    info.destinationType = AddressType::Group;

    std::array<uint8_t, 64> plain{};
    const auto result = harness.sal->processIncoming(info, tpdu, plain);

    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Consumed),
                          static_cast<int>(result.disposition));
    TEST_ASSERT_TRUE(harness.sent);

    // Challenge XOR Random (AA..AA), then SeqNrremote = 3 and SeqNrlocal =
    // 003F248463C6 encrypted with Random as the nonce and 15.15.255 as SA.
    const std::vector<uint8_t> expected{
        0x03, 0xF1, 0x93,
        0xFA, 0x9C, 0x46, 0xC0, 0x10, 0x42,
        0x18, 0xD8, 0x6A, 0x48, 0x06, 0xFF,
        0x03, 0xCA, 0x14, 0x8D, 0x5B, 0xDF,
        0x1E, 0x4E, 0x00, 0x22};
    assertBytes("S-A_Sync_Response", expected, harness.sentTpdu);

    // The answer to a broadcast request goes back as a broadcast.
    TEST_ASSERT_EQUAL_UINT16(0x0000, harness.sentInfo.destination);
    TEST_ASSERT_FALSE(harness.sentInfo.connected);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, harness.sentInfo.source.raw);
}

void test_plain_apdu_passes_through(void) {
    Harness harness;

    const std::vector<uint8_t> tpdu{0x00, 0x81};
    SecureFrameInfo info;
    info.source = IndividualAddress(0xFF67);
    info.destination = 0x0F81;
    info.destinationType = AddressType::Group;

    std::array<uint8_t, 64> plain{};
    const auto result = harness.sal->processIncoming(info, tpdu, plain);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Plain),
                          static_cast<int>(result.disposition));
}

// --- Outbound --------------------------------------------------------------

void test_response_to_a_tool_access_request_is_secured_with_the_tool_key(void) {
    Harness harness;
    harness.securityObject.setSendingSequence(0x000000000010ULL);

    // Receive the C.1.1 request first: that is what marks 15.15.103 as a peer
    // this device talks to under Tool Access.
    const std::vector<uint8_t> request{
        0x03, 0xF1, 0x90,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
        0x67, 0x67, 0x24, 0x2a, 0x23, 0x08, 0xca, 0x76, 0xa1, 0x17, 0x74, 0x21,
        0x4e, 0xe4, 0xcf, 0x5d, 0x94, 0x90, 0x9f, 0x74, 0x3d, 0x05,
        0x0d, 0x8f, 0xc1, 0x68};

    SecureFrameInfo rx;
    rx.source = IndividualAddress(0xFF67);
    rx.destination = 0xFF00;
    rx.destinationType = AddressType::Individual;

    std::array<uint8_t, 64> plain{};
    const auto rxResult = harness.sal->processIncoming(rx, request, plain);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Unwrapped),
                          static_cast<int>(rxResult.disposition));

    // Now answer it. C.1.2 is exactly this response, so the wire bytes are
    // known: SeqNr 3, SCF 90, from 15.15.0 to 15.15.103.
    harness.securityObject.setSendingSequence(2u);
    const std::vector<uint8_t> responsePlain{
        0x03, 0xD6, 0x05, 0x35, 0x10, 0x01,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};

    SecureFrameInfo tx;
    tx.source = IndividualAddress(0xFF00);
    tx.destination = 0xFF67;
    tx.destinationType = AddressType::Individual;

    std::array<uint8_t, 128> secured{};
    const auto txResult = harness.sal->processOutgoing(tx, responsePlain, secured);
    TEST_ASSERT_TRUE(txResult.isOk());

    const std::vector<uint8_t> expected{
        0x03, 0xF1, 0x90,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
        0x70, 0x6f, 0x53, 0x31, 0x05, 0x50, 0x35, 0x57, 0xcb, 0x2b, 0x24, 0xf1,
        0xdd, 0x34, 0x1b, 0x60, 0xb7, 0xe0, 0x17, 0xec, 0xd6, 0xb0,
        0x68, 0x49, 0xa7, 0x2b};
    assertBytes("secured response",
                expected,
                std::span<const uint8_t>(secured.data(), txResult.value()));
}

void test_outgoing_frame_to_an_unknown_peer_stays_plain(void) {
    Harness harness;

    const std::vector<uint8_t> plainTpdu{0x00, 0x81};
    SecureFrameInfo tx;
    tx.source = IndividualAddress(0xFF00);
    tx.destination = 0x1234;
    tx.destinationType = AddressType::Individual;

    std::array<uint8_t, 128> secured{};
    const auto result = harness.sal->processOutgoing(tx, plainTpdu, secured);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(result.value()));
}

// --- Group-addressed S-A_Data ----------------------------------------------
// Secured multicast is not a special case: it uses the group key for the CCM
// and the sender's Last Valid SeqNr for replay protection, exactly like
// point-to-point traffic. The device must therefore be able to send on as many
// group addresses as it has group objects, with no per-destination state.

const SecureApplicationLayer::Key kGroupKey{
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

/// A secured group telegram, as @p source would emit it towards @p groupAddress.
std::vector<uint8_t> secureGroupTpdu(Harness& harness, uint16_t groupAddress,
                                     uint16_t source = 0xFF00) {
    const std::vector<uint8_t> plainTpdu{0x00, 0x80, 0x0C, 0x1A};
    SecureFrameInfo tx;
    tx.source = IndividualAddress(source);
    tx.destination = groupAddress;
    tx.destinationType = AddressType::Group;

    std::array<uint8_t, 128> secured{};
    const auto result = harness.sal->processOutgoing(tx, plainTpdu, secured);
    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(result.value() > 0u);
    return std::vector<uint8_t>(secured.begin(), secured.begin() + result.value());
}

void test_group_telegrams_are_secured_on_every_group_address(void) {
    Harness harness;

    // More distinct destinations than the old fixed session cache held (32),
    // and more than this product has group objects. Every one must be secured;
    // running out of a per-destination resource is what made a commissioned
    // device stop publishing.
    constexpr uint16_t kFirstGroup = 0x0930;
    constexpr size_t kGroupCount = 64;
    for (uint16_t i = 0; i < kGroupCount; ++i) {
        harness.securityObject.setGroupKey(GroupAddress(kFirstGroup + i), kGroupKey);
    }

    uint64_t previousSeq = 0;
    for (uint16_t i = 0; i < kGroupCount; ++i) {
        const auto tpdu = secureGroupTpdu(harness, static_cast<uint16_t>(kFirstGroup + i));

        // 03 F1 | SCF 10 (S-A_Data, A+C, no Tool Access) | SeqNr(6) | ...
        TEST_ASSERT_EQUAL_UINT8(0x03, tpdu[0]);
        TEST_ASSERT_EQUAL_UINT8(0xF1, tpdu[1]);
        TEST_ASSERT_EQUAL_UINT8(0x10, tpdu[2]);

        // One sending sequence for the whole device, not one per destination.
        uint64_t seq = 0;
        for (size_t b = 0; b < 6; ++b) seq = (seq << 8) | tpdu[3 + b];
        TEST_ASSERT_TRUE(seq > previousSeq);
        previousSeq = seq;
    }
}

void test_group_telegram_round_trips_and_replay_is_rejected(void) {
    Harness harness;
    constexpr uint16_t kGroup = 0x0930;
    harness.securityObject.setGroupKey(GroupAddress(kGroup), kGroupKey);

    const auto tpdu = secureGroupTpdu(harness, kGroup);

    // Feed it back as if a peer had sent it.
    SecureFrameInfo rx;
    rx.source = IndividualAddress(0xFF00);
    rx.destination = kGroup;
    rx.destinationType = AddressType::Group;

    std::array<uint8_t, 64> plain{};
    const auto first = harness.sal->processIncoming(rx, tpdu, plain);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Unwrapped),
                          static_cast<int>(first.disposition));
    const std::vector<uint8_t> expectedPlain{0x00, 0x80, 0x0C, 0x1A};
    assertBytes("plain group APDU", expectedPlain,
                std::span<const uint8_t>(plain.data(), first.plainLength));
    TEST_ASSERT_TRUE(first.security.secured);
    TEST_ASSERT_FALSE(first.security.toolAccess);

    // Replay protection is keyed on the sender, so the same telegram again is
    // rejected — and it is the sender's Last Valid SeqNr that recorded it.
    uint64_t seq = 0;
    for (size_t b = 0; b < 6; ++b) seq = (seq << 8) | tpdu[3 + b];
    TEST_ASSERT_EQUAL_UINT64(seq, harness.securityObject.getPeerSequence(IndividualAddress(0xFF00)));

    const auto replayed = harness.sal->processIncoming(rx, tpdu, plain);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Rejected),
                          static_cast<int>(replayed.disposition));
}

void test_group_replay_window_is_per_sender_not_per_group_address(void) {
    Harness harness;
    constexpr uint16_t kGroup = 0x0930;
    harness.securityObject.setGroupKey(GroupAddress(kGroup), kGroupKey);

    // Two senders on the same group address. The sending sequence is this
    // device's, so building the "other" sender's telegram first gives it the
    // lower SeqNr — and putting more than a replay window's worth of telegrams
    // between them is what tells the two designs apart: a single window shared
    // by group address would have slid past the older SeqNr and dropped it.
    const auto fromOther = secureGroupTpdu(harness, kGroup, 0x1101);
    for (int i = 0; i < 80; ++i) {
        (void)secureGroupTpdu(harness, kGroup);
    }
    const auto fromUs = secureGroupTpdu(harness, kGroup, 0xFF00);

    SecureFrameInfo rx;
    rx.destination = kGroup;
    rx.destinationType = AddressType::Group;

    std::array<uint8_t, 64> plain{};

    rx.source = IndividualAddress(0xFF00);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Unwrapped),
                          static_cast<int>(harness.sal->processIncoming(rx, fromUs, plain).disposition));

    // The far older telegram from the *other* sender is still fresh for that
    // sender, and must be accepted.
    rx.source = IndividualAddress(0x1101);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(SecureRxDisposition::Unwrapped),
                          static_cast<int>(harness.sal->processIncoming(rx, fromOther, plain).disposition));

    // Each sender ended up with its own Last Valid SeqNr.
    const uint64_t seqUs = harness.securityObject.getPeerSequence(IndividualAddress(0xFF00));
    const uint64_t seqOther = harness.securityObject.getPeerSequence(IndividualAddress(0x1101));
    TEST_ASSERT_TRUE(seqOther > 0u);
    TEST_ASSERT_TRUE(seqUs > seqOther);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_spec_c11_data_apdu_is_verified_and_decrypted);
    RUN_TEST(test_data_apdu_with_replayed_sequence_is_rejected);
    RUN_TEST(test_tampered_data_apdu_is_rejected);
    RUN_TEST(test_spec_c14_sync_response_matches_specification);
    RUN_TEST(test_sync_request_raises_stored_sequence_but_not_our_own);
    RUN_TEST(test_second_sync_request_within_one_second_is_ignored);
    RUN_TEST(test_broadcast_sync_request_for_another_serial_is_ignored);
    RUN_TEST(test_broadcast_sync_request_without_serial_number_is_ignored);
    RUN_TEST(test_uncommissioned_device_answers_broadcast_sync_request);
    RUN_TEST(test_plain_apdu_passes_through);
    RUN_TEST(test_response_to_a_tool_access_request_is_secured_with_the_tool_key);
    RUN_TEST(test_outgoing_frame_to_an_unknown_peer_stays_plain);
    RUN_TEST(test_group_telegrams_are_secured_on_every_group_address);
    RUN_TEST(test_group_telegram_round_trips_and_replay_is_rejected);
    RUN_TEST(test_group_replay_window_is_per_sender_not_per_group_address);
    return UNITY_END();
}
