// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * KNX IP Secure Routing end to end, below the socket (03_08_09 §2.2.2).
 *
 * What the review found was a Secure Routing context that numbered its frames
 * with a counter from 1 under a serial of zero and a fixed tag — the same
 * nonces after every restart and on every device holding the key — and that
 * accepted any frame whose MAC verified, however old. These tests pin the
 * replacement down:
 *
 *  - the nonce is mc_timer | KNX serial | per-frame tag, and never repeats,
 *    including across a restart (the persisted timer);
 *  - the MAC is checked before the timer value is believed, and a frame older
 *    than the latency tolerance is discarded;
 *  - TIMER_NOTIFY encodes byte-for-byte like a captured one and keeps the
 *    timer in step;
 *  - a new key restarts the timer, the same key re-applied does not.
 */

#include "unity.h"

#if KNX_SECURE_ENABLED

#include "knx/netip/ip_secure/secure_routing_security.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/netip/routing.hpp"
#include "knx/physical/ip_routing_physical.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/security/aes_block.hpp"
#include "knx/security/x25519.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <vector>

using namespace knx;
using namespace knx::netip;
using namespace knx::netip::ip_secure;

void setUp(void) {}
void tearDown(void) {}

namespace {

class FakeClock final : public platform::TimingPlatform {
public:
    uint32_t millis() const override { return now; }
    uint64_t micros() const override { return static_cast<uint64_t>(now) * 1000u; }
    void delay(uint32_t ms) override { now += ms; }
    void delayMicroseconds(uint32_t) override {}
    uint32_t now{1000};
};

class FakeStore final : public McTimerStore {
public:
    std::optional<Record> load() override { return stored; }
    util::Result<void> save(const Record& record) override
    {
        stored = record;
        ++saves;
        return util::Result<void>::ok();
    }
    std::optional<Record> stored;
    int saves{0};
};

const SecureRoutingSecurity::Key kKey{0x6d, 0xa8, 0x09, 0xee, 0xd8, 0xdb, 0xdb, 0xd0,
                                      0xa0, 0x68, 0x0f, 0xb8, 0x8a, 0xfe, 0x2b, 0xb2};
const SecureRoutingSecurity::Key kOtherKey{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const SecureRoutingSecurity::Serial kSerialA{0x00, 0xFA, 0x00, 0x00, 0x00, 0x0A};
const SecureRoutingSecurity::Serial kSerialB{0x00, 0xFA, 0x00, 0x00, 0x00, 0x0B};

constexpr uint64_t kD = SecureRoutingSecurity::kPersistIntervalMs;

/// A routing indication worth wrapping.
const std::vector<uint8_t> kPlain{0x06, 0x10, 0x05, 0x30, 0x00, 0x11, 0x29, 0x00, 0xBC, 0xE0,
                                  0x11, 0x0A, 0x23, 0x01, 0x01, 0x00, 0x81};

SecureRoutingOptions options(FakeClock& clock, FakeStore* store = nullptr, uint32_t draw = 0x4242)
{
    SecureRoutingOptions o;
    o.timing = &clock;
    o.store = store;
    o.random = [draw](std::span<uint8_t> out) {
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<uint8_t>(draw >> (8 * ((out.size() - 1 - i) % 4)));
        }
    };
    o.startReason = McTimerSync::StartReason::Reconfigured;
    return o;
}

std::array<uint8_t, 4> keyCheck(const SecureRoutingSecurity::Key& key)
{
    security::Aes128BlockCipher cipher;
    (void)cipher.setKey(key);
    return SecureRoutingSecurity::keyCheckValue(cipher);
}

uint64_t timerOf(std::span<const uint8_t> wrapper)
{
    uint64_t t = 0;
    for (size_t i = 8; i < 14; ++i) t = (t << 8) | wrapper[i];
    return t;
}

std::vector<uint8_t> protect(SecureRoutingSecurity& sec, std::span<const uint8_t> plain = kPlain)
{
    std::vector<uint8_t> out(plain.size() + SecureWrapper::kOverhead);
    auto res = sec.protect(plain, out);
    TEST_ASSERT_TRUE(res.isOk());
    out.resize(res.value());
    return out;
}

std::array<uint8_t, 36> notifyFrame(const SecureRoutingSecurity::Key& key, uint64_t timer,
                                    const SecureRoutingSecurity::Serial& serial,
                                    SecureRoutingSecurity::Tag tag = {0x12, 0x34})
{
    security::Aes128BlockCipher cipher;
    (void)cipher.setKey(key);
    SecureWrapper::TimerNotify n{};
    for (size_t i = 0; i < 6; ++i) n.timer[i] = static_cast<uint8_t>(timer >> (8 * (5 - i)));
    n.serial = serial;
    n.tag = tag;
    std::array<uint8_t, 36> out{};
    TEST_ASSERT_TRUE(SecureWrapper::encodeTimerNotify(cipher, n, out).isOk());
    return out;
}

} // namespace

// --- TIMER_NOTIFY codec (A2) ------------------------------------------------

// Frame 26 of a KNXUltimate capture, test/vectors/knxnetip_secure_wrapper/
// timernotify_0955_capture.vec.
const std::array<uint8_t, 36> kCapturedNotify{
    0x06, 0x10, 0x09, 0x55, 0x00, 0x24, 0x00, 0x00, 0x00, 0xca, 0x02, 0x79,
    0x00, 0xa2, 0x07, 0x16, 0x40, 0x12, 0x6f, 0x8c, 0xfd, 0xfa, 0x2c, 0xed,
    0x10, 0x57, 0x8e, 0x5a, 0xff, 0x68, 0xdd, 0xde, 0xa8, 0x10, 0xe1, 0x7d};

void test_timer_notify_encodes_like_the_captured_frame(void)
{
    const auto frame = notifyFrame(kKey, 0x0000000000ca0279ULL, {0x00, 0xa2, 0x07, 0x16, 0x40, 0x12}, {0x6f, 0x8c});
    TEST_ASSERT_EQUAL_MEMORY(kCapturedNotify.data(), frame.data(), frame.size());
}

void test_timer_notify_decodes_and_rejects_tampering_and_bad_length(void)
{
    security::Aes128BlockCipher cipher;
    (void)cipher.setKey(kKey);
    auto decoded = SecureWrapper::decodeTimerNotify(cipher, kCapturedNotify);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL_UINT8(0x6f, decoded.value().tag[0]);

    auto tampered = kCapturedNotify;
    tampered[11] ^= 0x01;  // timer
    TEST_ASSERT_TRUE(SecureWrapper::decodeTimerNotify(cipher, tampered).isError());

    // §2.2.2.4.4: anything but 36 octets is discarded.
    std::vector<uint8_t> longer(kCapturedNotify.begin(), kCapturedNotify.end());
    longer.push_back(0);
    longer[5] = 0x25;
    TEST_ASSERT_TRUE(SecureWrapper::decodeTimerNotify(cipher, longer).isError());
}

// --- Nonce construction (A1, P1) --------------------------------------------

void test_wrapper_carries_mc_timer_device_serial_and_random_tag(void)
{
    FakeClock clock;
    SecureRoutingSecurity sec(kKey, kSerialA, options(clock, nullptr, 0xABCD));
    clock.now += 1234;
    const auto w = protect(sec);
    TEST_ASSERT_EQUAL_UINT64(1234, timerOf(w));
    TEST_ASSERT_EQUAL_MEMORY(kSerialA.data(), w.data() + 14, 6);
    TEST_ASSERT_EQUAL_UINT8(0xAB, w[20]);
    TEST_ASSERT_EQUAL_UINT8(0xCD, w[21]);
}

void test_frames_in_the_same_millisecond_never_share_a_nonce(void)
{
    FakeClock clock;
    SecureRoutingSecurity sec(kKey, kSerialA, options(clock));  // the "random" draw is constant
    std::set<std::vector<uint8_t>> nonces;
    for (int i = 0; i < 5; ++i) {
        const auto w = protect(sec);
        nonces.insert(std::vector<uint8_t>(w.begin() + 8, w.begin() + 22));
    }
    TEST_ASSERT_EQUAL_UINT32(5, static_cast<uint32_t>(nonces.size()));
}

void test_restart_resumes_above_every_timer_sent_before_it(void)
{
    FakeClock clock;
    FakeStore store;
    uint64_t highestSent = 0;
    {
        SecureRoutingSecurity sec(kKey, kSerialA, options(clock, &store));
        // Two and a half hours of traffic: the stored value has to follow.
        for (int i = 0; i < 10; ++i) {
            clock.now += 15 * 60 * 1000;
            highestSent = std::max(highestSent, timerOf(protect(sec)));
        }
    }
    // Power lost; the clock restarts from a small value.
    clock.now = 5;
    SecureRoutingSecurity rebooted(kKey, kSerialA, options(clock, &store));
    const auto w = protect(rebooted);
    TEST_ASSERT_TRUE(timerOf(w) > highestSent);
}

void test_store_is_written_only_once_per_interval(void)
{
    FakeClock clock;
    FakeStore store;
    SecureRoutingSecurity sec(kKey, kSerialA, options(clock, &store));
    TEST_ASSERT_EQUAL_INT(1, store.saves);  // at start
    for (int i = 0; i < 100; ++i) {
        clock.now += 1000;
        (void)protect(sec);
    }
    TEST_ASSERT_EQUAL_INT(1, store.saves);
    clock.now += static_cast<uint32_t>(kD);
    (void)protect(sec);
    TEST_ASSERT_EQUAL_INT(2, store.saves);
}

void test_stored_timer_is_resumed_only_for_the_same_key(void)
{
    FakeClock clock;
    FakeStore store;
    store.stored = McTimerStore::Record{5'000'000, keyCheck(kKey)};
    SecureRoutingSecurity same(kKey, kSerialA, options(clock, &store));
    TEST_ASSERT_EQUAL_UINT64(5'000'000 + kD, same.mcTimer());
    TEST_ASSERT_EQUAL_UINT64(5'000'000 + kD, store.stored->timer);

    FakeStore other;
    other.stored = McTimerStore::Record{5'000'000, keyCheck(kOtherKey)};
    SecureRoutingSecurity fresh(kKey, kSerialA, options(clock, &other));
    TEST_ASSERT_EQUAL_UINT64(0, fresh.mcTimer());  // first use of this key
}

// --- Receiving (A1, P2) ------------------------------------------------------

void test_receiver_adopts_a_newer_timer(void)
{
    FakeClock clock;
    FakeStore senderStore;
    senderStore.stored = McTimerStore::Record{9'000'000, keyCheck(kKey)};
    SecureRoutingSecurity sender(kKey, kSerialA, options(clock, &senderStore));
    SecureRoutingSecurity receiver(kKey, kSerialB, options(clock));

    const auto w = protect(sender);
    std::vector<uint8_t> plain(w.size());
    auto res = receiver.unprotect(w, plain);
    TEST_ASSERT_TRUE(res.isOk());
    TEST_ASSERT_EQUAL_UINT32(kPlain.size(), res.value());
    TEST_ASSERT_EQUAL_MEMORY(kPlain.data(), plain.data(), kPlain.size());
    TEST_ASSERT_EQUAL_UINT64(timerOf(w), receiver.mcTimer());
}

void test_replayed_frame_is_discarded_and_the_plaintext_wiped(void)
{
    FakeClock clock;
    SecureRoutingSecurity sender(kKey, kSerialA, options(clock));
    SecureRoutingSecurity receiver(kKey, kSerialB, options(clock));

    const auto w = protect(sender);
    std::vector<uint8_t> plain(w.size());
    TEST_ASSERT_TRUE(receiver.unprotect(w, plain).isOk());

    // The same datagram, captured and played back once the latency tolerance
    // (2000 ms by default) has passed.
    clock.now += 2000;
    std::fill(plain.begin(), plain.end(), 0xEE);
    auto replay = receiver.unprotect(w, plain);
    TEST_ASSERT_TRUE(replay.isError());
    TEST_ASSERT_EQUAL_UINT64(1, receiver.outdatedDiscarded());
    for (size_t i = 0; i < kPlain.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, plain[i]);
    }
}

void test_mac_is_checked_before_the_timer_is_believed(void)
{
    FakeClock clock;
    FakeStore senderStore;
    senderStore.stored = McTimerStore::Record{9'000'000, keyCheck(kKey)};
    SecureRoutingSecurity sender(kKey, kSerialA, options(clock, &senderStore));
    SecureRoutingSecurity receiver(kKey, kSerialB, options(clock));

    auto w = protect(sender);
    w[13] ^= 0x01;  // timer altered in flight
    std::vector<uint8_t> plain(w.size(), 0xEE);
    TEST_ASSERT_TRUE(receiver.unprotect(w, plain).isError());
    TEST_ASSERT_EQUAL_UINT64(0, receiver.mcTimer());  // not moved
    TEST_ASSERT_EQUAL_UINT64(0, receiver.outdatedDiscarded());
    for (size_t i = 0; i < kPlain.size(); ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, plain[i]);  // A7: no unauthenticated plaintext left behind
    }
}

void test_adopting_a_timer_beyond_the_interval_persists_it_at_once(void)
{
    FakeClock clock;
    FakeStore store;
    SecureRoutingSecurity receiver(kKey, kSerialB, options(clock, &store));
    const int savesBefore = store.saves;
    const uint64_t far = 10 * kD;
    TEST_ASSERT_TRUE(receiver.handleTimerNotify(notifyFrame(kKey, far, kSerialA)).isOk());
    TEST_ASSERT_EQUAL_UINT64(far, receiver.mcTimer());
    TEST_ASSERT_EQUAL_INT(savesBefore + 1, store.saves);
    TEST_ASSERT_EQUAL_UINT64(far, store.stored->timer);
}

void test_forged_timer_notify_is_ignored(void)
{
    FakeClock clock;
    SecureRoutingSecurity receiver(kKey, kSerialB, options(clock));
    TEST_ASSERT_TRUE(receiver.handleTimerNotify(notifyFrame(kOtherKey, 10 * kD, kSerialA)).isError());
    TEST_ASSERT_EQUAL_UINT64(0, receiver.mcTimer());
}

void test_due_timer_notify_is_authentic_and_carries_own_serial(void)
{
    FakeClock clock;
    SecureRoutingSecurity sec(kKey, kSerialA, options(clock));
    clock.now += 77;
    std::array<uint8_t, 36> out{};
    auto due = sec.takeDueTimerNotify(out);  // Reconfigured: due at once
    TEST_ASSERT_TRUE(due.isOk());
    TEST_ASSERT_EQUAL_UINT32(36, due.value());

    security::Aes128BlockCipher cipher;
    (void)cipher.setKey(kKey);
    auto decoded = SecureWrapper::decodeTimerNotify(cipher, out);
    TEST_ASSERT_TRUE(decoded.isOk());
    TEST_ASSERT_EQUAL_MEMORY(kSerialA.data(), decoded.value().serial.data(), 6);
    TEST_ASSERT_EQUAL_UINT8(77, decoded.value().timer[5]);

    // Not due again until the periodic window.
    TEST_ASSERT_EQUAL_UINT32(0, sec.takeDueTimerNotify(out).value());
    TEST_ASSERT_TRUE(sec.millisecondsUntilTimerNotify() >= 10000u);
}

// --- Rekeying through the physical (A1 E11, A4) ------------------------------

namespace {

class NullSocket final : public platform::UdpSocket {
public:
    util::Result<void> open(uint16_t) override { open_ = true; return util::Result<void>::ok(); }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    util::Result<void> joinMulticast(IpAddress, IpAddress) override { return util::Result<void>::ok(); }
    void leaveMulticast(IpAddress, IpAddress) override {}
    util::Result<void> setMulticastInterface(IpAddress) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastLoopback(platform::MulticastLoopbackMode) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastTtl(uint8_t) override { return util::Result<void>::ok(); }
    int send(IpAddress, uint16_t, std::span<const uint8_t> data) override { return static_cast<int>(data.size()); }
    int receive(std::span<uint8_t>) override { return -1; }
    int receive(std::span<uint8_t>, IpAddress&, uint16_t&) override { return -1; }
    size_t available() const override { return 0; }
    uint16_t localPort() const override { return 3671; }

private:
    bool open_{false};
};

class NullNetwork final : public platform::NetworkInterface {
public:
    util::Result<void> init() override { return util::Result<void>::ok(); }
    bool isConnected() const override { return true; }
    IpAddress ipAddress() const override { return IpAddress::fromOctets(127, 0, 0, 1); }
    IpAddress subnetMask() const override { return IpAddress(0); }
    IpAddress gateway() const override { return IpAddress(0); }
    void macAddress(std::span<uint8_t> mac) const override { std::fill(mac.begin(), mac.end(), 0); }
    std::unique_ptr<platform::UdpSocket> createUdpSocket() override { return std::make_unique<NullSocket>(); }
};

} // namespace

void test_same_key_reapplied_keeps_the_timer_and_a_new_key_restarts_it(void)
{
    FakeClock clock;
    FakeStore store;
    store.stored = McTimerStore::Record{5'000'000, keyCheck(kKey)};
    NullNetwork net;

    physical::IpRoutingPhysical phys;
    phys.setNetworkInterface(&net);
    phys.setTimingPlatform(&clock);
    phys.setMcTimerStore(&store);
    phys.setSecureRoutingSerial(kSerialA);
    TEST_ASSERT_TRUE(phys.init().isOk());

    // Restored at boot: the stored timer plus D, not 0.
    TEST_ASSERT_TRUE(phys.applySecureRouting(kKey, physical::IpRoutingPhysical::KeySource::Restored).isOk());
    TEST_ASSERT_EQUAL_UINT64(5'000'000 + kD, phys.secureRoutingTimer());

    // ETS writing the same key again is not E11.
    clock.now += 10;
    TEST_ASSERT_TRUE(phys.applySecureRouting(kKey).isOk());
    TEST_ASSERT_EQUAL_UINT64(5'000'000 + kD + 10, phys.secureRoutingTimer());

    // A different key is a new domain: mc_timer restarts at 0 (§2.2.2.2.2).
    TEST_ASSERT_TRUE(phys.applySecureRouting(kOtherKey).isOk());
    TEST_ASSERT_EQUAL_UINT64(0, phys.secureRoutingTimer());
    TEST_ASSERT_EQUAL_MEMORY(keyCheck(kOtherKey).data(), store.stored->keyCheck.data(), 4);

    phys.close();
}

// --- E11 from a new routing multicast address (A7) ---------------------------

void test_restart_sync_notifies_at_once_and_keeps_the_timer(void)
{
    FakeClock clock;
    SecureRoutingSecurity sec(kKey, kSerialA, options(clock));
    std::array<uint8_t, 64> out{};

    // The Reconfigured start's notify, then the periodic wait (>= 10 s).
    TEST_ASSERT_TRUE(sec.takeDueTimerNotify(out).value() > 0);
    clock.now += 1000;
    TEST_ASSERT_TRUE(sec.millisecondsUntilTimerNotify() > 0);
    const uint64_t before = sec.mcTimer();

    sec.restartSync();

    // notify_timer = 0 (§2.2.2.3.2.8, not a power-up) ...
    TEST_ASSERT_EQUAL_UINT32(0, sec.millisecondsUntilTimerNotify());
    // ... and mc_timer goes on from where it was, not from 0.
    TEST_ASSERT_EQUAL_UINT64(before, sec.mcTimer());
}

namespace {

/// Counts the TIMER_NOTIFY frames (0955h) a physical puts on the group.
class NotifyCountingSocket final : public platform::UdpSocket {
public:
    util::Result<void> open(uint16_t) override { open_ = true; return util::Result<void>::ok(); }
    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }
    util::Result<void> joinMulticast(IpAddress, IpAddress) override { return util::Result<void>::ok(); }
    void leaveMulticast(IpAddress, IpAddress) override {}
    util::Result<void> setMulticastInterface(IpAddress) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastLoopback(platform::MulticastLoopbackMode) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastTtl(uint8_t) override { return util::Result<void>::ok(); }
    int send(IpAddress, uint16_t, std::span<const uint8_t> data) override
    {
        if (data.size() >= 4 && data[2] == 0x09 && data[3] == 0x55) {
            ++*notifies;
        }
        return static_cast<int>(data.size());
    }
    int receive(std::span<uint8_t>) override { return -1; }
    int receive(std::span<uint8_t>, IpAddress&, uint16_t&) override { return -1; }
    size_t available() const override { return 0; }
    uint16_t localPort() const override { return 3671; }

    int* notifies{nullptr};

private:
    bool open_{false};
};

class NotifyCountingNetwork final : public platform::NetworkInterface {
public:
    util::Result<void> init() override { return util::Result<void>::ok(); }
    bool isConnected() const override { return true; }
    IpAddress ipAddress() const override { return IpAddress::fromOctets(127, 0, 0, 1); }
    IpAddress subnetMask() const override { return IpAddress(0); }
    IpAddress gateway() const override { return IpAddress(0); }
    void macAddress(std::span<uint8_t> mac) const override { std::fill(mac.begin(), mac.end(), 0); }
    std::unique_ptr<platform::UdpSocket> createUdpSocket() override
    {
        auto socket = std::make_unique<NotifyCountingSocket>();
        socket->notifies = &notifies;
        return socket;
    }
    int notifies{0};
};

/// One receive pass of the data link layer: due TIMER_NOTIFYs go out here.
void pumpReceive(physical::IpRoutingPhysical& phys)
{
    (void)phys.beginReceive(0);
    (void)phys.pollReceive();
}

}  // namespace

void test_new_domain_restarts_the_sync_and_the_same_domain_does_not(void)
{
    FakeClock clock;
    NotifyCountingNetwork net;
    physical::IpRoutingPhysical phys;
    phys.setNetworkInterface(&net);
    phys.setTimingPlatform(&clock);
    phys.setSecureRoutingSerial(kSerialA);
    TEST_ASSERT_TRUE(phys.init().isOk());
    TEST_ASSERT_TRUE(phys.applySecureRouting(kKey).isOk());

    pumpReceive(phys);
    TEST_ASSERT_EQUAL_INT(1, net.notifies);  // E11 from the key: notify at once
    clock.now += 1000;
    pumpReceive(phys);
    TEST_ASSERT_EQUAL_INT(1, net.notifies);  // and then not for >= 10 s
    const uint64_t timer = phys.secureRoutingTimer();

    // E11 from PID_ROUTING_MULTICAST_ADDRESS (the BusAccessUnit reports only a
    // new and different value; the same value is E12 and never gets here).
    phys.restartMulticastTimerSync();
    pumpReceive(phys);
    TEST_ASSERT_EQUAL_INT(2, net.notifies);
    TEST_ASSERT_TRUE(phys.secureRoutingTimer() >= timer);

    phys.close();
}

void test_restarting_the_sync_without_secure_routing_does_nothing(void)
{
    FakeClock clock;
    NotifyCountingNetwork net;
    physical::IpRoutingPhysical phys;
    phys.setNetworkInterface(&net);
    phys.setTimingPlatform(&clock);
    TEST_ASSERT_TRUE(phys.init().isOk());

    phys.restartMulticastTimerSync();
    pumpReceive(phys);
    TEST_ASSERT_EQUAL_INT(0, net.notifies);
    TEST_ASSERT_FALSE(phys.isSecureRoutingActive());
    phys.close();
}

void test_key_applied_before_init_is_installed_at_init(void)
{
    FakeClock clock;
    NullNetwork net;
    physical::IpRoutingPhysical phys;
    phys.setNetworkInterface(&net);
    phys.setTimingPlatform(&clock);
    TEST_ASSERT_TRUE(phys.applySecureRouting(kKey, physical::IpRoutingPhysical::KeySource::Restored).isOk());
    TEST_ASSERT_EQUAL_UINT64(0, phys.secureRoutingTimer());  // no context before init
    TEST_ASSERT_TRUE(phys.init().isOk());
    clock.now += 5;
    TEST_ASSERT_EQUAL_UINT64(5, phys.secureRoutingTimer());
    phys.close();
}

void test_latency_settings_reach_the_context(void)
{
    FakeClock clock;
    SecureRoutingSecurity sender(kKey, kSerialA, options(clock));
    SecureRoutingSecurity receiver(kKey, kSerialB, options(clock));
    receiver.setLatencyTolerance(10000, 0x1A);

    const auto w = protect(sender);
    clock.now += 5000;  // late by the default 2 s, fine by 10 s
    std::vector<uint8_t> plain(w.size());
    TEST_ASSERT_TRUE(receiver.unprotect(w, plain).isOk());
}

// --- Crypto hygiene (A7) -----------------------------------------------------

void test_wrap_refuses_an_inner_datagram_that_overflows_the_length_field(void)
{
    // 0xFFFF - 38 + 1 octets: the 16-bit total would wrap to a small value.
    std::vector<uint8_t> inner(0xFFFF - SecureWrapper::kOverhead + 1, 0);
    std::vector<uint8_t> out(64);
    const std::array<uint8_t, 6> seq{};
    const std::array<uint8_t, 6> serial{};
    const std::array<uint8_t, 2> tag{};
    const std::array<uint8_t, 4> suffix{0, 0, 0xFF, 0};
    auto res = SecureWrapper::wrap(kKey, SessionId::invalid(), seq, serial, tag, suffix, inner, out);
    TEST_ASSERT_TRUE(res.isError());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(util::ErrorCode::InvalidFrameSize), static_cast<int>(res.error()));
}

// A7: a low-order peer point (u = 0 or 1) yields the all-zero secret for
// every private key. A session keyed from it would be readable by anyone.
void test_x25519_refuses_an_all_zero_shared_secret(void)
{
    security::X25519::Scalar priv{};
    priv.fill(0x5A);
    security::X25519::SharedSecret secret{};
    for (const uint8_t u : {uint8_t{0}, uint8_t{1}}) {
        security::X25519::PublicKey peer{};
        peer[0] = u;
        TEST_ASSERT_TRUE(security::X25519::sharedSecret(priv, peer, secret).isError());
    }
    security::X25519::PublicKey base{};
    base[0] = 9;
    TEST_ASSERT_TRUE(security::X25519::sharedSecret(priv, base, secret).isOk());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_x25519_refuses_an_all_zero_shared_secret);
    RUN_TEST(test_timer_notify_encodes_like_the_captured_frame);
    RUN_TEST(test_timer_notify_decodes_and_rejects_tampering_and_bad_length);
    RUN_TEST(test_wrapper_carries_mc_timer_device_serial_and_random_tag);
    RUN_TEST(test_frames_in_the_same_millisecond_never_share_a_nonce);
    RUN_TEST(test_restart_resumes_above_every_timer_sent_before_it);
    RUN_TEST(test_store_is_written_only_once_per_interval);
    RUN_TEST(test_stored_timer_is_resumed_only_for_the_same_key);
    RUN_TEST(test_receiver_adopts_a_newer_timer);
    RUN_TEST(test_replayed_frame_is_discarded_and_the_plaintext_wiped);
    RUN_TEST(test_mac_is_checked_before_the_timer_is_believed);
    RUN_TEST(test_adopting_a_timer_beyond_the_interval_persists_it_at_once);
    RUN_TEST(test_forged_timer_notify_is_ignored);
    RUN_TEST(test_due_timer_notify_is_authentic_and_carries_own_serial);
    RUN_TEST(test_same_key_reapplied_keeps_the_timer_and_a_new_key_restarts_it);
    RUN_TEST(test_restart_sync_notifies_at_once_and_keeps_the_timer);
    RUN_TEST(test_new_domain_restarts_the_sync_and_the_same_domain_does_not);
    RUN_TEST(test_restarting_the_sync_without_secure_routing_does_nothing);
    RUN_TEST(test_key_applied_before_init_is_installed_at_init);
    RUN_TEST(test_latency_settings_reach_the_context);
    RUN_TEST(test_wrap_refuses_an_inner_datagram_that_overflows_the_length_field);
    return UNITY_END();
}

#else

void setUp(void) {}
void tearDown(void) {}
int main(void) { return 0; }

#endif
