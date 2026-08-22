// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file test_netip_socket_wait.cpp
 * @brief detail::waitUntilReadable over a socket that can wait, and one that cannot
 *
 * The receive path sleeps in the socket where the platform can do that, and
 * falls back to asking on an interval where it cannot. Both halves have to keep
 * working: the first is what takes the polling out of a KNXnet/IP device, and
 * the second is what every platform without a readiness primitive still runs.
 */

#include "unity.h"

#include "knx/netip/detail/polling.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/platform/linux_platform.hpp"

#include <array>
#include <cstdint>

using namespace knx;

namespace {

/// A socket that answers only the two questions the wait helper asks.
class FakeSocket : public platform::UdpSocket {
public:
    /// Becomes readable once the clock has passed this mark.
    uint32_t readableAtMs{0};
    /// Whether this platform can wait on the socket at all.
    bool canWait{true};
    /// How the caller reached readiness, which is the whole point of the test.
    unsigned waitCalls{0};
    unsigned availableCalls{0};

    explicit FakeSocket(platform::TimingPlatform& clock) : _clock(clock) {}

    size_t available() const override {
        ++const_cast<FakeSocket*>(this)->availableCalls;
        return _clock.millis() >= readableAtMs ? 1u : 0u;
    }

    util::Result<bool> waitReadable(uint32_t timeoutMs) override {
        ++waitCalls;
        if (!canWait) {
            return util::ErrorCode::OperationNotSupported;
        }
        const uint32_t deadline = _clock.millis() + timeoutMs;
        if (readableAtMs <= deadline) {
            // A real socket returns when the datagram lands; the fake charges
            // the caller the same time so the elapsed clock stays honest.
            if (readableAtMs > _clock.millis()) {
                _clock.delay(readableAtMs - _clock.millis());
            }
            return true;
        }
        _clock.delay(timeoutMs);
        return false;
    }

    // Not exercised here.
    util::Result<void> open(uint16_t) override { return util::Result<void>::ok(); }
    void close() override {}
    bool isOpen() const override { return true; }
    util::Result<void> joinMulticast(IpAddress, IpAddress) override { return util::Result<void>::ok(); }
    void leaveMulticast(IpAddress, IpAddress) override {}
    util::Result<void> setMulticastInterface(IpAddress) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastLoopback(platform::MulticastLoopbackMode) override {
        return util::Result<void>::ok();
    }
    util::Result<void> setMulticastTtl(uint8_t) override { return util::Result<void>::ok(); }
    int send(IpAddress, uint16_t, std::span<const uint8_t>) override { return 0; }
    int receive(std::span<uint8_t>) override { return 0; }
    int receive(std::span<uint8_t>, IpAddress&, uint16_t&) override { return 0; }
    uint16_t localPort() const override { return 0; }

private:
    platform::TimingPlatform& _clock;
};

/// A clock the test moves by hand, so a timeout costs no wall time.
class SteppedClock final : public platform::TimingPlatform {
public:
    uint32_t millis() const override { return _ms; }
    uint64_t micros() const override { return static_cast<uint64_t>(_ms) * 1000u; }
    void delay(uint32_t ms) override { _ms += ms; }
    void delayMicroseconds(uint32_t us) override { _ms += us / 1000u; }

private:
    uint32_t _ms{1000};
};

} // namespace

void setUp(void) {}
void tearDown(void) {}

// A socket that can wait is waited on, not polled.
void test_wait_uses_the_socket_when_it_can_wait(void) {
    SteppedClock clock;
    FakeSocket sock(clock);
    sock.readableAtMs = clock.millis() + 40u;

    const auto result = netip::detail::waitUntilReadable(&clock, 500, sock);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(result.value());
    TEST_ASSERT_EQUAL_UINT32(1, sock.waitCalls);
    // The interval poll is what this replaces, so it must not have run.
    TEST_ASSERT_EQUAL_UINT32(0, sock.availableCalls);
}

// And a timeout is a timeout, not an error.
void test_wait_reports_timeout_without_error(void) {
    SteppedClock clock;
    FakeSocket sock(clock);
    sock.readableAtMs = clock.millis() + 5000u;  // long after the deadline

    const auto result = netip::detail::waitUntilReadable(&clock, 100, sock);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_FALSE(result.value());
}

// A platform without the primitive keeps the behaviour it always had.
void test_wait_falls_back_to_polling(void) {
    SteppedClock clock;
    FakeSocket sock(clock);
    sock.canWait = false;
    sock.readableAtMs = clock.millis() + 20u;

    const auto result = netip::detail::waitUntilReadable(&clock, 500, sock);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_TRUE(result.value());
    TEST_ASSERT_EQUAL_UINT32(1, sock.waitCalls);       // asked once, refused
    TEST_ASSERT_GREATER_THAN(1, sock.availableCalls);  // then polled
}

// Zero means "look now": neither waiting nor sleeping.
void test_zero_timeout_never_waits(void) {
    SteppedClock clock;
    FakeSocket sock(clock);
    sock.readableAtMs = clock.millis() + 1u;

    const auto result = netip::detail::waitUntilReadable(&clock, 0, sock);

    TEST_ASSERT_TRUE(result.isOk());
    TEST_ASSERT_FALSE(result.value());
    TEST_ASSERT_EQUAL_UINT32(0, sock.waitCalls);
    TEST_ASSERT_EQUAL_UINT32(1, sock.availableCalls);
    TEST_ASSERT_EQUAL_UINT32(1000, clock.millis());  // no time passed
}

// The Linux socket really does implement the primitive; a platform that only
// inherits the default would silently keep polling.
void test_linux_udp_socket_can_wait(void) {
    platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    auto sock = net->createUdpSocket();
    TEST_ASSERT_NOT_NULL(sock.get());
    TEST_ASSERT_TRUE(sock->open(0).isOk());

    const uint32_t before = platform.millis();
    const auto waited = sock->waitReadable(120);
    const uint32_t elapsed = platform.millis() - before;

    TEST_ASSERT_TRUE(waited.isOk());
    TEST_ASSERT_FALSE(waited.value());
    // Lower bound only: it must actually have slept rather than returned at once.
    TEST_ASSERT_GREATER_OR_EQUAL(100, elapsed);

    sock->close();
}

// And it returns as soon as a datagram lands, rather than sitting out the
// deadline. This is the whole benefit: the receive task sleeps until there is
// something to do, instead of asking on an interval.
void test_linux_udp_socket_wakes_on_arrival(void) {
    platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    auto receiver = net->createUdpSocket();
    TEST_ASSERT_NOT_NULL(receiver.get());
    TEST_ASSERT_TRUE(receiver->open(0).isOk());
    const uint16_t boundPort = receiver->localPort();
    TEST_ASSERT_GREATER_THAN(0, boundPort);

    auto sender = net->createUdpSocket();
    TEST_ASSERT_NOT_NULL(sender.get());
    TEST_ASSERT_TRUE(sender->open(0).isOk());

    const std::array<uint8_t, 4> payload{0xDE, 0xAD, 0xBE, 0xEF};
    TEST_ASSERT_GREATER_THAN(
        0, sender->send(IpAddress::fromOctets(127, 0, 0, 1), boundPort, payload));

    // Generous deadline the wait must not spend.
    const uint32_t before = platform.millis();
    const auto waited = receiver->waitReadable(4000);
    const uint32_t elapsed = platform.millis() - before;

    TEST_ASSERT_TRUE(waited.isOk());
    TEST_ASSERT_TRUE(waited.value());
    TEST_ASSERT_LESS_THAN(2000, elapsed);

    sender->close();
    receiver->close();
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_wait_uses_the_socket_when_it_can_wait);
    RUN_TEST(test_wait_reports_timeout_without_error);
    RUN_TEST(test_wait_falls_back_to_polling);
    RUN_TEST(test_zero_timeout_never_waits);
    RUN_TEST(test_linux_udp_socket_can_wait);
    RUN_TEST(test_linux_udp_socket_wakes_on_arrival);
    return UNITY_END();
}
