// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_data_secure_virtual_tp1.cpp
 * @brief KNX Data Secure end to end over the virtual TP1 bus
 *
 * The other Data Secure tests stop short of the medium: the S-AL known-answer
 * tests call SecureApplicationLayer directly, and test_bau_onwire drives a BAU
 * through a frame-level mock physical. This one runs a whole device the way a
 * TP1 firmware does -- BusAccessUnit on createTp1StackPort(), Tp1MacPhysical,
 * the bit-bang ISR core on the virtual bus, the data link RX task on a
 * VirtualPlatform fibre -- and talks to it in bit cells:
 *
 *  - a group telegram secured with the group key (S-A_Data, SCF 10h) reaches
 *    the group object, and the same telegram in plain is refused;
 *  - a management request secured with the Tool Key (A_PropertyValue_Read,
 *    SCF 90h) is answered with an A_PropertyValue_Response secured the same
 *    way, which the peer verifies and decrypts; the same request in plain is
 *    answered in plain.
 *
 * The peer's request is CCM-protected here with the stack's public block and
 * cipher primitives, not by the S-AL under test: the S-AL only ever answers
 * Tool Access, so using it for the request would test it against itself.
 */

#include "unity.h"

#include "knx/bau/bau.hpp"
#include "knx/datalink/frame_codec.hpp"
#include "knx/objects/object_persistence.hpp"
#include "knx/objects/security_interface_object.hpp"
#include "knx/physical/bitbang_medium_backend_adapter.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/virtual_tp1_bus_peer.hpp"
#include "knx/physical/virtual_tp1_test_runtime.hpp"
#include "knx/security/aes128_cbc_mac.hpp"
#include "knx/security/aes128_ctr.hpp"
#include "knx/security/data_secure.hpp"
#include "knx/security/secure_application_layer.hpp"

#include "../common/virtual_bitbang_tp1_driver.hpp"
#include "../common/virtual_tp1_test_scheduler.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace knx;
using namespace knx::testsupport;

namespace {

constexpr IndividualAddress kDevice(1, 1, 10);
constexpr IndividualAddress kPeer(1, 1, 20);
constexpr GroupAddress kSecuredGroup(0, 1, 0);

const std::array<uint8_t, 6> kSerial{0x00, 0xFA, 0x12, 0x34, 0x56, 0x78};

const security::SecureApplicationLayer::Key kToolKey{
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
const security::SecureApplicationLayer::Key kGroupKey{
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};

constexpr uint8_t kScfGroupData = 0x10;      // S-A_Data, CCM A+C
constexpr uint8_t kScfToolAccessData = 0x90; // S-A_Data, CCM A+C, Tool Access

physical::VirtualTp1TestRuntime* runtime = nullptr;
std::unique_ptr<physical::VirtualTp1BusPeer> busPeer;
VirtualBitBangTp1Driver* driver = nullptr;
std::unique_ptr<VirtualBitBangTp1Driver> ownedDriver;
std::unique_ptr<bau::BusAccessUnit> device;
std::unique_ptr<VirtualTp1TestScheduler> scheduler;

std::vector<std::vector<uint8_t>> groupWrites;

void clearPersistenceStore() {
    std::error_code ec;
    (void)std::filesystem::remove_all(objects::persistenceNamespaceDir("knx_objects"), ec);
}

std::vector<uint8_t> withChecksum(std::vector<uint8_t> frame) {
    frame.push_back(datalink::FrameCodec::calculateChecksum(frame));
    return frame;
}

/// A TP1 L_Data frame carrying @p tpdu, low priority, hop count 6: standard
/// format when the TPDU fits (at most 16 octets), extended otherwise.
std::vector<uint8_t> tp1Frame(IndividualAddress source, uint16_t destination, bool group,
                              std::span<const uint8_t> tpdu) {
    std::vector<uint8_t> frame;
    const uint8_t length = static_cast<uint8_t>(tpdu.size() - 1u);
    if (tpdu.size() <= 16u) {
        frame = {0xBC,
                 static_cast<uint8_t>(source.raw >> 8), static_cast<uint8_t>(source.raw),
                 static_cast<uint8_t>(destination >> 8), static_cast<uint8_t>(destination),
                 static_cast<uint8_t>((group ? 0x80u : 0x00u) | 0x60u | length)};
    } else {
        frame = {0x3C,
                 static_cast<uint8_t>((group ? 0x80u : 0x00u) | 0x60u),
                 static_cast<uint8_t>(source.raw >> 8), static_cast<uint8_t>(source.raw),
                 static_cast<uint8_t>(destination >> 8), static_cast<uint8_t>(destination),
                 length};
    }
    frame.insert(frame.end(), tpdu.begin(), tpdu.end());
    return withChecksum(std::move(frame));
}

/// The TPDU of a TP1 frame the device sent (checksum stripped).
std::vector<uint8_t> tpduOf(const std::vector<uint8_t>& frame) {
    const bool standard = (frame[0] & 0x80u) != 0u;
    const size_t offset = standard ? 6u : 7u;
    TEST_ASSERT_TRUE(frame.size() > offset + 1u);
    return std::vector<uint8_t>(frame.begin() + static_cast<std::ptrdiff_t>(offset), frame.end() - 1);
}

/// Secure a plain TPDU the way a peer holding @p key does (03/03/07 §5.1):
/// 03 F1 | SCF | SeqNr(6) | ENC(APDU) | ENC(MAC). Connectionless, so TPCI 0.
std::vector<uint8_t> secureTpdu(const security::SecureApplicationLayer::Key& key,
                                uint8_t scf,
                                uint64_t sequence,
                                IndividualAddress source,
                                uint16_t destination,
                                bool group,
                                std::span<const uint8_t> plainTpdu) {
    security::SecureBlockContext ctx;
    ctx.srcIa = source;
    ctx.dstRaw = destination;
    ctx.atOctet = group ? 0x80 : 0x00;
    ctx.tpci6 = 0;

    std::array<uint8_t, 6> nonce{};
    security::writeSecureU48BE(std::span<uint8_t, 6>(nonce), sequence);

    std::array<uint8_t, 16> block0{};
    TEST_ASSERT_TRUE(security::buildSecureBlock0(ctx, nonce, plainTpdu.size(), block0).isOk());
    const std::array<uint8_t, 1> associated{scf};
    security::Aes128CbcMac::Block mac{};
    TEST_ASSERT_TRUE(security::Aes128CbcMac::compute(key, block0, associated, plainTpdu, mac).isOk());

    std::array<uint8_t, 16> counter0{};
    TEST_ASSERT_TRUE(security::buildSecureCounter0(ctx, nonce, counter0).isOk());
    std::vector<uint8_t> clear(mac.begin(), mac.begin() + 4);
    clear.insert(clear.end(), plainTpdu.begin(), plainTpdu.end());
    std::vector<uint8_t> enciphered(clear.size());
    TEST_ASSERT_TRUE(security::Aes128Ctr::crypt(key, counter0, clear, enciphered).isOk());

    std::vector<uint8_t> tpdu{security::DataSecureSession::APCI_SEC_HIGH,
                              security::DataSecureSession::APCI_SEC_LOW, scf};
    tpdu.insert(tpdu.end(), nonce.begin(), nonce.end());
    tpdu.insert(tpdu.end(), enciphered.begin() + 4, enciphered.end());   // payload first
    tpdu.insert(tpdu.end(), enciphered.begin(), enciphered.begin() + 4); // then the MAC
    return tpdu;
}

/// Run the device -- its RX fibre and its owner loop -- for @p ms of virtual
/// time, or until @p done.
template <typename Done>
bool runDeviceUntil(Done done, uint32_t ms = 500u) {
    for (uint32_t i = 0; i < ms; ++i) {
        device->loop();
        if (done()) {
            return true;
        }
        runtime->advanceTimeMs(1u);
    }
    device->loop();
    return done();
}

/// Put @p frame on the bus once the line has been idle long enough: the
/// device may still be sending the acknowledgement of the previous frame.
void inject(const std::vector<uint8_t>& frame) {
    runtime->advanceTimeMs(10u);
    TEST_ASSERT_TRUE(scheduler->injectRawFrame(frame));
}

/// Wait for the device's next frame, acknowledge it the way the addressed
/// peer would, and return it.
std::vector<uint8_t> takeAcknowledgedFrame() {
    driver->clearSentFrames();
    TEST_ASSERT_TRUE(runDeviceUntil([] { return driver->sentFrameCount() > 0u; }));
    std::vector<uint8_t> sent = driver->sentFrame(0);
    TEST_ASSERT_TRUE(scheduler->driveTxOutcomeForSentFrame(0u, VirtualTp1PeerResponse::Ack));
    (void)runDeviceUntil([] { return false; }, 20u);
    return sent;
}

} // namespace

void setUp(void) {
    clearPersistenceStore();
    groupWrites.clear();

    runtime = new physical::VirtualTp1TestRuntime();
    busPeer = std::make_unique<physical::VirtualTp1BusPeer>(runtime->bus());
    ownedDriver = std::make_unique<VirtualBitBangTp1Driver>(runtime->bus());
    driver = ownedDriver.get();

    auto macPhysical = std::make_unique<physical::Tp1MacPhysical>(
        std::make_unique<physical::BitBangMediumBackendAdapter>(*driver, *driver));
    macPhysical->setTimingPlatform(&runtime->platform());

    device = std::make_unique<bau::BusAccessUnit>(
        runtime->platform(), bau::createTp1StackPort(runtime->platform(), std::move(macPhysical)));
    device->setGroupObjectWriteCallback([](GroupObjectIndex, std::span<const uint8_t> data) {
        groupWrites.emplace_back(data.begin(), data.end());
    });
    TEST_ASSERT_TRUE(device->init(kDevice).isOk());

    device->deviceObject().setSerialNumber(kSerial);
    TEST_ASSERT_TRUE(device->addGroupObject(kSecuredGroup, application::dptids::Bool,
                                            true, true, true, true).isValid());

    // The state ETS leaves behind after a secure download: Tool Key and group
    // key in force, Security Mode on.
    device->securityObject().setToolKey(kToolKey);
    device->securityObject().setGroupKey(kSecuredGroup, kGroupKey);
    device->securityObject().setSecurityMode(objects::SecurityMode::Enabled);

    scheduler = std::make_unique<VirtualTp1TestScheduler>(*runtime, *busPeer, *driver);
    // Let the RX fibre reach its first wait and the bus settle.
    runtime->advanceTimeMs(10u);
    driver->clearSentFrames();
}

void tearDown(void) {
    if (device) {
        device->close();
        device.reset();
    }
    scheduler.reset();
    ownedDriver.reset();
    driver = nullptr;
    busPeer.reset();
    delete runtime;
    runtime = nullptr;
    clearPersistenceStore();
}

// A group telegram secured with the group key arrives as bit cells, is
// acknowledged by the ISR, decrypted on the RX task and lands in the group
// object. With Security Mode on and a key for the address, the same write in
// plain must not.
void test_secured_group_write_over_virtual_tp1_reaches_the_group_object(void) {
    const std::vector<uint8_t> plainWrite{0x00, 0x81};  // A_GroupValue_Write, 1

    // Plain first: refused, because the address has a group key.
    inject(tp1Frame(kPeer, kSecuredGroup.raw, true, plainWrite));
    TEST_ASSERT_FALSE(runDeviceUntil([] { return !groupWrites.empty(); }, 100u));
    TEST_ASSERT_EQUAL_UINT32(1u, device->securityObject().getFailureCount());

    const auto secured = secureTpdu(kGroupKey, kScfGroupData, 0x000000000020ULL, kPeer,
                                    kSecuredGroup.raw, true, plainWrite);
    TEST_ASSERT_EQUAL_UINT32(15u, static_cast<uint32_t>(secured.size()));  // standard frame
    inject(tp1Frame(kPeer, kSecuredGroup.raw, true, secured));

    TEST_ASSERT_TRUE(runDeviceUntil([] { return !groupWrites.empty(); }));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(groupWrites.size()));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(groupWrites[0].size()));
    TEST_ASSERT_EQUAL_HEX8(0x01, groupWrites[0][0]);

    // The sender's Last Valid SeqNr moved, so the same telegram again is a
    // replay and is dropped.
    TEST_ASSERT_EQUAL_UINT64(0x20u, device->securityObject().getPeerSequence(kPeer));
    const uint32_t failuresBefore = device->securityObject().getFailureCount();
    inject(tp1Frame(kPeer, kSecuredGroup.raw, true, secured));
    TEST_ASSERT_FALSE(runDeviceUntil([] { return groupWrites.size() > 1u; }, 100u));
    // Dropped by the S-AL as a replay, not lost on the bus.
    TEST_ASSERT_EQUAL_UINT32(failuresBefore + 1u, device->securityObject().getFailureCount());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(objects::SecurityFailure::SequenceNumberInvalid),
                          static_cast<int>(device->securityObject().getLastFailure()));
}

// ETS reads a property under the Tool Key; the device answers on the same
// protection, and the answer goes out over the bus as an extended frame that
// the peer can verify and decrypt.
void test_secured_management_request_over_virtual_tp1_gets_a_secured_answer(void) {
    // A_PropertyValue_Read: Device Object (index 0), PID_SERIAL_NUMBER (11),
    // one element from index 1. Connectionless T_Data_Individual.
    const std::vector<uint8_t> plainRead{0x03, 0xD5, 0x00, 0x0B, 0x10, 0x01};

    // In plain it is answered in plain: the Device Object's serial number is
    // readable without security, and an answer mirrors the request (§5.5).
    inject(tp1Frame(kPeer, kDevice.raw, false, plainRead));
    const auto plainAnswer = tpduOf(takeAcknowledgedFrame());
    TEST_ASSERT_EQUAL_HEX8(0x03, plainAnswer[0]);
    TEST_ASSERT_EQUAL_HEX8(0xD6, plainAnswer[1]);  // A_PropertyValue_Response, not 03 F1

    const auto secured = secureTpdu(kToolKey, kScfToolAccessData, 0x000000000010ULL, kPeer,
                                    kDevice.raw, false, plainRead);
    TEST_ASSERT_EQUAL_UINT32(19u, static_cast<uint32_t>(secured.size()));  // extended frame
    inject(tp1Frame(kPeer, kDevice.raw, false, secured));
    const std::vector<uint8_t> sent = takeAcknowledgedFrame();

    // Addressed to the peer, from the device, individually.
    const bool standard = (sent[0] & 0x80u) != 0u;
    TEST_ASSERT_FALSE(standard);  // 6 + 6 octets of serial do not fit a standard frame once secured
    TEST_ASSERT_EQUAL_HEX8(0x00, sent[1] & 0x80u);
    TEST_ASSERT_EQUAL_HEX16(kDevice.raw, static_cast<uint16_t>((sent[2] << 8) | sent[3]));
    TEST_ASSERT_EQUAL_HEX16(kPeer.raw, static_cast<uint16_t>((sent[4] << 8) | sent[5]));
    TEST_ASSERT_EQUAL_HEX8(datalink::FrameCodec::calculateChecksum(
                               std::vector<uint8_t>(sent.begin(), sent.end() - 1)),
                           sent.back());

    const auto tpdu = tpduOf(sent);
    TEST_ASSERT_EQUAL_HEX8(0x03, tpdu[0]);  // Secure APCI
    TEST_ASSERT_EQUAL_HEX8(0xF1, tpdu[1]);
    TEST_ASSERT_EQUAL_HEX8(kScfToolAccessData, tpdu[2]);

    // The peer side: an S-AL holding the same Tool Key verifies and decrypts.
    objects::SecurityInterfaceObject peerSecurity;
    peerSecurity.setToolKey(kToolKey);
    peerSecurity.setSecurityMode(objects::SecurityMode::Enabled);
    security::SecureApplicationLayer peer(peerSecurity, [] { return 1000u; },
                                          [](std::span<uint8_t> out) { std::memset(out.data(), 0x55, out.size()); });
    peer.setOwnAddress(kPeer);

    security::SecureFrameInfo info;
    info.source = kDevice;
    info.destination = kPeer.raw;
    info.destinationType = AddressType::Individual;
    info.tpci6 = 0;
    std::array<uint8_t, 64> plain{};
    const auto result = peer.processIncoming(info, tpdu, plain);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(security::SecureRxDisposition::Unwrapped),
                          static_cast<int>(result.disposition));
    TEST_ASSERT_TRUE(result.security.toolAccess);

    // A_PropertyValue_Response: same object, PID, count and index, then the
    // serial number.
    const std::vector<uint8_t> expected{0x03, 0xD6, 0x00, 0x0B, 0x10, 0x01,
                                        kSerial[0], kSerial[1], kSerial[2],
                                        kSerial[3], kSerial[4], kSerial[5]};
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(expected.size()),
                             static_cast<uint32_t>(result.plainLength));
    for (size_t i = 0; i < expected.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(expected[i], plain[i]);
    }

    // The request's sequence number is now the device's Tool Access SeqNr, so
    // the request cannot be replayed.
    TEST_ASSERT_EQUAL_UINT64(0x10u, device->securityObject().getToolAccessSequence());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_secured_group_write_over_virtual_tp1_reaches_the_group_object);
    RUN_TEST(test_secured_management_request_over_virtual_tp1_gets_a_secured_answer);
    return UNITY_END();
}
