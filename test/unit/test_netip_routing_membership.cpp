// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/*
 * The routing group membership, and what happens to it when the interface is
 * not there yet.
 *
 * A KNX IP device on Wi-Fi routinely opens its socket before the association
 * completes: the join fails, and if that took the whole open() down with it the
 * device would run on compiled defaults instead of the parameters ETS gave it —
 * or, on a stack that ignored the failure, would sit silently off the group
 * forever. Neither is acceptable, so open() succeeds without the membership and
 * the host completes it when the interface arrives.
 */
#include "unity.h"

#include "knx/netip/routing_endpoint.hpp"
#include "knx/platform/network_interface.hpp"

#include <cstring>
#include <memory>
#include <vector>

using namespace knx;
using namespace knx::netip;

namespace {

class TestUdpSocket final : public platform::UdpSocket {
public:
    util::Result<void> open(uint16_t port = 0) override {
        open_ = true;
        port_ = port;
        return util::Result<void>::ok();
    }

    void close() override { open_ = false; }
    bool isOpen() const override { return open_; }

    util::Result<void> joinMulticast(IpAddress, IpAddress) override {
        ++joinAttempts;
        if (!interfaceUp) return util::ErrorCode::OperationFailed;
        ++joinsAccepted;
        return util::Result<void>::ok();
    }

    void leaveMulticast(IpAddress, IpAddress) override { ++leaves; }

    util::Result<void> setMulticastInterface(IpAddress) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastLoopback(platform::MulticastLoopbackMode) override { return util::Result<void>::ok(); }
    util::Result<void> setMulticastTtl(uint8_t) override { return util::Result<void>::ok(); }

    int send(IpAddress, uint16_t, std::span<const uint8_t> data) override {
        return open_ ? static_cast<int>(data.size()) : -1;
    }

    int receive(std::span<uint8_t>) override { return -1; }
    int receive(std::span<uint8_t>, IpAddress&, uint16_t&) override { return -1; }

    size_t available() const override { return 0; }
    uint16_t localPort() const override { return port_; }

    /// Stands in for "the station has associated and holds an address".
    bool interfaceUp{false};
    int joinAttempts{0};
    int joinsAccepted{0};
    int leaves{0};

private:
    bool open_{false};
    uint16_t port_{0};
};

class TestNetwork final : public platform::NetworkInterface {
public:
    util::Result<void> init() override { return util::Result<void>::ok(); }
    bool isConnected() const override { return sock != nullptr && sock->interfaceUp; }

    IpAddress ipAddress() const override { return address; }
    IpAddress subnetMask() const override { return IpAddress(0); }
    IpAddress gateway() const override { return IpAddress(0); }
    void macAddress(std::span<uint8_t> mac) const override {
        if (mac.data() == nullptr || mac.size() < 6) return;
        for (int i = 0; i < 6; ++i) mac[i] = 0;
    }

    std::unique_ptr<platform::UdpSocket> createUdpSocket() override {
        auto s = std::make_unique<TestUdpSocket>();
        sock = s.get();
        return s;
    }

    IpAddress address{IpAddress(0)};
    TestUdpSocket* sock{nullptr};
};

RoutingEndpoint::Options defaultOptions()
{
    RoutingEndpoint::Options opt;
    opt.multicastGroup = IpAddress::fromOctets(224, 0, 23, 12);
    opt.port = NetIpPort(netip::config::kDefaultPort);
    return opt;
}

} // namespace

void setUp(void) {}
void tearDown(void) {}

void test_open_succeeds_without_an_interface_to_join_on(void)
{
    TestNetwork net;
    RoutingEndpoint ep;

    // No interface: the join fails, and the endpoint opens anyway.
    TEST_ASSERT_TRUE(ep.open(net, defaultOptions()).isOk());
    TEST_ASSERT_TRUE(ep.isOpen());
    TEST_ASSERT_FALSE(ep.multicastJoined());
    TEST_ASSERT_EQUAL_INT(1, net.sock->joinAttempts);

    ep.close();
}

void test_refresh_completes_the_membership_once_the_interface_arrives(void)
{
    TestNetwork net;
    RoutingEndpoint ep;

    TEST_ASSERT_TRUE(ep.open(net, defaultOptions()).isOk());
    TEST_ASSERT_FALSE(ep.multicastJoined());

    net.sock->interfaceUp = true;
    net.address = IpAddress::fromOctets(192, 168, 1, 42);

    TEST_ASSERT_TRUE(ep.refreshMulticastMembership().isOk());
    TEST_ASSERT_TRUE(ep.multicastJoined());
    TEST_ASSERT_EQUAL_INT(1, net.sock->joinsAccepted);
    // Nothing was joined before, so nothing was left behind either.
    TEST_ASSERT_EQUAL_INT(0, net.sock->leaves);

    ep.close();
}

void test_refresh_rejoins_a_membership_that_already_exists(void)
{
    TestNetwork net;
    RoutingEndpoint ep;

    // The socket only exists from open(), so the interface is brought up
    // through the first refresh rather than before the endpoint sees it.
    TEST_ASSERT_TRUE(ep.open(net, defaultOptions()).isOk());
    net.sock->interfaceUp = true;
    TEST_ASSERT_TRUE(ep.refreshMulticastMembership().isOk());
    TEST_ASSERT_TRUE(ep.multicastJoined());

    const int leavesBefore = net.sock->leaves;

    // A DHCP renewal into another subnet: the membership that exists points at
    // a route that no longer carries the group, so it is dropped before the
    // rejoin rather than joined on top of.
    TEST_ASSERT_TRUE(ep.refreshMulticastMembership().isOk());
    TEST_ASSERT_TRUE(ep.multicastJoined());
    TEST_ASSERT_EQUAL_INT(leavesBefore + 1, net.sock->leaves);
    TEST_ASSERT_EQUAL_INT(2, net.sock->joinsAccepted);

    ep.close();
}

void test_refresh_reports_a_still_absent_interface(void)
{
    TestNetwork net;
    RoutingEndpoint ep;

    TEST_ASSERT_TRUE(ep.open(net, defaultOptions()).isOk());

    // Called on every address event, including ones that do not help.
    TEST_ASSERT_TRUE(ep.refreshMulticastMembership().isError());
    TEST_ASSERT_FALSE(ep.multicastJoined());

    ep.close();
}

void test_refresh_on_a_closed_endpoint_is_not_initialized(void)
{
    RoutingEndpoint ep;

    const auto result = ep.refreshMulticastMembership();
    TEST_ASSERT_TRUE(result.isError());
    TEST_ASSERT_TRUE(result.error() == util::ErrorCode::NotInitialized);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_open_succeeds_without_an_interface_to_join_on);
    RUN_TEST(test_refresh_completes_the_membership_once_the_interface_arrives);
    RUN_TEST(test_refresh_rejoins_a_membership_that_already_exists);
    RUN_TEST(test_refresh_reports_a_still_absent_interface);
    RUN_TEST(test_refresh_on_a_closed_endpoint_is_not_initialized);
    return UNITY_END();
}
