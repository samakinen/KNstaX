// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/physical/ip_routing_physical.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/application/apci_services.hpp"
#include "knx/platform/linux_platform.hpp"

#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

using namespace knx;
using namespace knx::datalink;
using namespace knx::physical;

void setUp(void) {}
void tearDown(void) {}

static Tp1DataLinkConfig polledRxConfig() {
    auto config = Tp1DataLinkConfig::defaults();
    config.enableRxTask = false;
    return config;
}

static uint16_t pickPort() {
    if (const char* v = std::getenv("KNX_TEST_MCAST_PORT")) {
        const unsigned long parsed = std::strtoul(v, nullptr, 10);
        if (parsed > 0 && parsed <= 65535) {
            return static_cast<uint16_t>(parsed);
        }
    }
    const uint32_t pid = static_cast<uint32_t>(::getpid());
    return static_cast<uint16_t>(30000 + (pid % 20000));
}

static knx::IpAddress pickGroup() {
    if (const char* v = std::getenv("KNX_TEST_MCAST_GROUP")) {
        return knx::IpAddress::fromString(v);
    }
    const uint32_t pid = static_cast<uint32_t>(::getpid());
    const uint8_t octet = static_cast<uint8_t>(1 + (pid % 250));
    return knx::IpAddress::fromOctets(239, 255, 0, octet);
}

static knx::IpAddress pickIface() {
    if (const char* v = std::getenv("KNX_TEST_MCAST_IFACE")) {
        return knx::IpAddress::fromString(v);
    }
    return knx::IpAddress::fromOctets(127, 0, 0, 1);
}

void test_ip_routing_physical_send_receive(void) {
    const auto group = pickGroup();
    const auto port = pickPort();
    const auto iface = pickIface();

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpRoutingPhysical physA;
    physA.setNetworkInterface(net);
    physA.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkLayer dlA(platform, physA);
    TEST_ASSERT_TRUE(dlA.init(IndividualAddress(0x1101)).isOk());
    TEST_ASSERT_TRUE(dlA.addGroupAddress(GroupAddress(0x0100)).isOk());

    IpRoutingPhysical physB;
    physB.setNetworkInterface(net);
    physB.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkLayer dlB(platform, physB);
    TEST_ASSERT_TRUE(dlB.init(IndividualAddress(0x1102)).isOk());
    TEST_ASSERT_TRUE(dlB.addGroupAddress(GroupAddress(0x0100)).isOk());

    std::atomic<bool> got{false};
    LDataFrame received;

    dlB.setReceiveCallback([&](const LDataFrame& in) {
        received = in;
        got.store(true);
    });

    LDataFrame out;
    out.standardFrame = true;
    out.repeated = false;
    out.priority = Priority::Low;
    out.ackRequested = false;
    out.confirmation = false;
    out.source = IndividualAddress(0x1101);
    out.destination = GroupAddress(0x0100);
    out.destinationType = AddressType::Group;
    out.hopCount = 6;
    out.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueRead, {});

    TEST_ASSERT_TRUE(dlA.sendFrame(out).isOk());

    for (int i = 0; i < 50 && !got.load(); ++i) {
        ::usleep(10 * 1000);
    }

    TEST_ASSERT_TRUE(got.load());
    TEST_ASSERT_TRUE(received.destinationType == AddressType::Group);
    TEST_ASSERT_EQUAL_HEX16(out.destination.raw, received.destination.raw);
    TEST_ASSERT_EQUAL_HEX16(out.source.raw, received.source.raw);
    TEST_ASSERT_EQUAL_UINT8(out.hopCount, received.hopCount);
    TEST_ASSERT_EQUAL_UINT8(out.tpdu.size(), received.tpdu.size());
    for (size_t i = 0; i < out.tpdu.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(out.tpdu[i], received.tpdu[i]);
    }

    dlA.close();
    dlB.close();
}

void test_ip_routing_physical_send_receive_with_explicit_progression(void) {
    const auto group = pickGroup();
    const auto port = pickPort();
    const auto iface = pickIface();

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpRoutingPhysical physA;
    physA.setNetworkInterface(net);
    physA.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkLayer dlA(platform, physA, nullptr, polledRxConfig());
    TEST_ASSERT_TRUE(dlA.init(IndividualAddress(0x1101)).isOk());
    TEST_ASSERT_TRUE(dlA.addGroupAddress(GroupAddress(0x0100)).isOk());

    IpRoutingPhysical physB;
    physB.setNetworkInterface(net);
    physB.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkLayer dlB(platform, physB, nullptr, polledRxConfig());
    TEST_ASSERT_TRUE(dlB.init(IndividualAddress(0x1102)).isOk());
    TEST_ASSERT_TRUE(dlB.addGroupAddress(GroupAddress(0x0100)).isOk());

    std::atomic<bool> got{false};
    LDataFrame received;

    dlB.setReceiveCallback([&](const LDataFrame& in) {
        received = in;
        got.store(true);
    });

    LDataFrame out;
    out.standardFrame = true;
    out.repeated = false;
    out.priority = Priority::Low;
    out.ackRequested = false;
    out.confirmation = false;
    out.source = IndividualAddress(0x1101);
    out.destination = GroupAddress(0x0100);
    out.destinationType = AddressType::Group;
    out.hopCount = 6;
    out.setTpdu(knx::protocol::TPCI::UnnumberedData, knx::application::APCIService::GroupValueRead, {});

    TEST_ASSERT_TRUE(dlA.sendFrame(out).isOk());

    for (int i = 0; i < 50 && !got.load(); ++i) {
        (void)dlA.processRxAvailable(0);
        (void)dlB.processRxAvailable(0);
        ::usleep(10 * 1000);
    }

    TEST_ASSERT_TRUE(got.load());
    TEST_ASSERT_TRUE(received.destinationType == AddressType::Group);
    TEST_ASSERT_EQUAL_HEX16(out.destination.raw, received.destination.raw);
    TEST_ASSERT_EQUAL_HEX16(out.source.raw, received.source.raw);
    TEST_ASSERT_EQUAL_UINT8(out.hopCount, received.hopCount);
    TEST_ASSERT_EQUAL_UINT8(out.tpdu.size(), received.tpdu.size());
    for (size_t i = 0; i < out.tpdu.size(); ++i) {
        TEST_ASSERT_EQUAL_HEX8(out.tpdu[i], received.tpdu[i]);
    }

    dlA.close();
    dlB.close();
}

// The counters exist to tell "the telegram never arrived" from "it arrived and
// was dropped in here" — the question a capture taken on the network cannot
// answer. A device that has gone deaf shows datagramsReceived standing still
// while msSinceLastInbound climbs.
namespace {

/// A clock the test moves by hand, so "silent for a minute" costs no wall time.
class SteppedClock final : public knx::platform::TimingPlatform {
public:
    uint32_t millis() const override { return _ms; }
    uint64_t micros() const override { return static_cast<uint64_t>(_ms) * 1000u; }
    void delay(uint32_t ms) override { _ms += ms; }
    void delayMicroseconds(uint32_t us) override { _ms += us / 1000u; }

    void advance(uint32_t ms) { _ms += ms; }

private:
    uint32_t _ms{1000};
};

} // namespace

void test_ip_routing_physical_counts_inbound_datagrams(void) {
    const auto group = pickGroup();
    const auto port = pickPort();
    const auto iface = pickIface();

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpRoutingPhysical sender;
    sender.setNetworkInterface(net);
    sender.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkConfig manualRx = Tp1DataLinkConfig::defaults();
    manualRx.enableRxTask = false;  // this test drives the port itself
    Tp1DataLinkLayer dlSender(platform, sender, manualRx);
    TEST_ASSERT_TRUE(dlSender.init(IndividualAddress(0x1101)).isOk());

    IpRoutingPhysical listener;
    listener.setNetworkInterface(net);
    listener.setTimingPlatform(&platform);
    listener.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkLayer dlListener(platform, listener, manualRx);
    TEST_ASSERT_TRUE(dlListener.init(IndividualAddress(0x1102)).isOk());
    TEST_ASSERT_TRUE(dlListener.addGroupAddress(GroupAddress(0x0100)).isOk());

    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)listener.linkStatistics().datagramsReceived);

    LDataFrame out;
    out.source = IndividualAddress(0x1101);
    out.destination = GroupAddress(0x0100);
    out.destinationType = AddressType::Group;
    out.hopCount = 6;
    out.tpdu.assign(std::vector<uint8_t>{0x00, 0x81});
    TEST_ASSERT_TRUE(dlSender.sendFrame(out).isOk());

    for (int i = 0; i < 200 && listener.linkStatistics().datagramsReceived == 0; ++i) {
        (void)dlListener.processRxAvailable(0);
        ::usleep(10 * 1000);
    }

    const auto stats = listener.linkStatistics();
    TEST_ASSERT_GREATER_OR_EQUAL(1, (uint32_t)stats.datagramsReceived);
    // Nothing was dropped, and the silence clock was reset by the arrival.
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)stats.datagramsDropped);
    TEST_ASSERT_LESS_THAN(5000, stats.msSinceLastInbound);

    dlSender.close();
    dlListener.close();
}

// A port that has heard nothing for a long time re-issues its group membership,
// because that is the one thing it can do about a switch or access point that
// has aged the group out and stopped forwarding it. Silence is the trigger
// rather than a plain interval: refreshing leaves the group before rejoining
// it, and on a busy group that would drop telegrams for no reason.
void test_ip_routing_physical_refreshes_membership_after_silence(void) {
    const auto group = pickGroup();
    const auto port = pickPort();
    const auto iface = pickIface();

    SteppedClock clock;
    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpRoutingPhysical phys;
    phys.setNetworkInterface(net);
    phys.setTimingPlatform(&clock);
    phys.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkConfig manualRx = Tp1DataLinkConfig::defaults();
    manualRx.enableRxTask = false;  // the virtual clock only moves when we say so
    Tp1DataLinkLayer dl(platform, phys, manualRx);
    TEST_ASSERT_TRUE(dl.init(IndividualAddress(0x1103)).isOk());
    TEST_ASSERT_TRUE(phys.multicastJoined());

    // First pass only starts the clock; a device that has never heard anything
    // must not read as having been silent since the epoch.
    (void)dl.processRxAvailable(0);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)phys.linkStatistics().membershipRefreshes);

    // Well short of the idle threshold: still nothing to do.
    clock.advance(30000);
    (void)dl.processRxAvailable(0);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)phys.linkStatistics().membershipRefreshes);

    // Past it: the membership is re-issued, and the device is still joined.
    clock.advance(31000);
    (void)dl.processRxAvailable(0);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)phys.linkStatistics().membershipRefreshes);
    TEST_ASSERT_TRUE(phys.multicastJoined());

    // And it does not then refresh on every poll.
    (void)dl.processRxAvailable(0);
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)phys.linkStatistics().membershipRefreshes);

    dl.close();
}

// The port waits inside the socket for the deadline it was given, instead of
// returning Pending and being asked again. This is what lets the data link's
// receive task sleep in the medium rather than polling it: beginReceive() set
// that deadline all along and pollReceive() ignored it.
void test_ip_routing_physical_receive_waits_for_the_deadline(void) {
    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpRoutingPhysical phys;
    phys.setNetworkInterface(net);
    phys.setTimingPlatform(&platform);
    phys.setMulticast(pickGroup(), NetIpPort(pickPort()), pickIface());
    Tp1DataLinkConfig manualRx = Tp1DataLinkConfig::defaults();
    manualRx.enableRxTask = false;
    Tp1DataLinkLayer dl(platform, phys, manualRx);
    TEST_ASSERT_TRUE(dl.init(IndividualAddress(0x1104)).isOk());

    // Nothing is being sent, so this must spend the whole deadline asleep.
    const uint32_t before = platform.millis();
    TEST_ASSERT_TRUE(phys.beginReceive(200).isOk());
    const auto progress = phys.pollReceive();
    const uint32_t elapsed = platform.millis() - before;

    TEST_ASSERT_TRUE(progress.isOk());
    TEST_ASSERT_EQUAL(knx::util::OperationProgressState::Timeout, progress.value());
    // Lower bound only. An upper bound would be a scheduling assertion, and
    // what matters is that it waited at all rather than returning at once.
    TEST_ASSERT_GREATER_OR_EQUAL(180, elapsed);

    dl.close();
}

// A client that sends two telegrams back to back — which is exactly how ETS
// opens a connection, T_Connect and the first request a millisecond apart —
// must not leave the second one sitting in the socket. One poll takes
// everything that has arrived; the socket buffer is the one queue in the
// system this device cannot size, and whatever overflows it is gone before any
// KNX layer sees it.
void test_ip_routing_physical_takes_a_whole_burst_in_one_poll(void) {
    const auto group = pickGroup();
    const auto port = pickPort();
    const auto iface = pickIface();

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());

    IpRoutingPhysical sender;
    sender.setNetworkInterface(net);
    sender.setMulticast(group, NetIpPort(port), iface);
    Tp1DataLinkConfig manualRx = Tp1DataLinkConfig::defaults();
    manualRx.enableRxTask = false;
    Tp1DataLinkLayer dlSender(platform, sender, manualRx);
    TEST_ASSERT_TRUE(dlSender.init(IndividualAddress(0x1105)).isOk());

    // The listener is the bare port: no data link layer above it, so the only
    // thing that ever reads this socket is the single poll below.
    IpRoutingPhysical listener;
    listener.setNetworkInterface(net);
    listener.setTimingPlatform(&platform);
    listener.setMulticast(group, NetIpPort(port), iface);
    TEST_ASSERT_TRUE(listener.init().isOk());

    LDataFrame out;
    out.source = IndividualAddress(0x1105);
    out.destination = GroupAddress(0x0100);
    out.destinationType = AddressType::Group;
    out.hopCount = 6;
    out.tpdu.assign(std::vector<uint8_t>{0x00, 0x81});

    // Two, as ETS sends them. The gap is the sender's egress budget, not the
    // property under test: what matters is that both datagrams are sitting in
    // the listener's socket before it reads, which the wait below guarantees.
    constexpr int kBurst = 2;
    for (int i = 0; i < kBurst; ++i) {
        TEST_ASSERT_TRUE(dlSender.sendFrame(out).isOk());
        ::usleep(50 * 1000);
    }

    // Let the whole burst land in the socket before anything reads it, so the
    // first read really does have the rest waiting behind it.
    ::usleep(300 * 1000);

    TEST_ASSERT_TRUE(listener.beginReceive(200).isOk());
    const auto progress = listener.pollReceive();
    TEST_ASSERT_TRUE(progress.isOk());

    // One poll, the whole burst off the socket. The socket is what is being
    // asserted, not what the layers above made of the frames: a datagram left
    // in the buffer is one this device can still lose, and only this counter
    // sees it.
    TEST_ASSERT_EQUAL_UINT32(
        kBurst, static_cast<uint32_t>(listener.linkStatistics().datagramsReceived));
    TEST_ASSERT_EQUAL_UINT32(
        0, static_cast<uint32_t>(listener.linkStatistics().datagramsDropped));

    dlSender.close();
    listener.close();
}

// The capability the data link reads to decide whether its receive task sleeps
// in the medium or polls it. Silently losing this reverts the whole change.
void test_ip_routing_physical_advertises_medium_wait(void) {
    TEST_ASSERT_TRUE(IpRoutingPhysical::receiveWaitsForMedium());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ip_routing_physical_send_receive);
    RUN_TEST(test_ip_routing_physical_send_receive_with_explicit_progression);
    RUN_TEST(test_ip_routing_physical_counts_inbound_datagrams);
    RUN_TEST(test_ip_routing_physical_refreshes_membership_after_silence);
    RUN_TEST(test_ip_routing_physical_receive_waits_for_the_deadline);
    RUN_TEST(test_ip_routing_physical_takes_a_whole_burst_in_one_poll);
    RUN_TEST(test_ip_routing_physical_advertises_medium_wait);
    return UNITY_END();
}
