// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "unity.h"

#include "knx/netip/cemi.hpp"
#include "knx/netip/routing_endpoint.hpp"
#include "knx/platform/linux_platform.hpp"

#include "knx/application/apci_services.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace knx;
using namespace knx::netip;
using namespace knx::datalink;

void setUp(void) {}
void tearDown(void) {}

static std::string g_snifferPath;

static LDataFrame makeFrame()
{
    LDataFrame f;
    f.standardFrame = true;
    f.repeated = false;
    f.priority = Priority::Normal;
    f.ackRequested = true;
    f.confirmation = true;
    f.source = IndividualAddress(0x110A);
    f.destination = GroupAddress(0x2301);
    f.destinationType = AddressType::Group;
    f.hopCount = 6;
    f.setTpdu(knx::protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
    return f;
}

static bool read_with_timeout(int fd, std::string& out, int timeoutMs)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char buf[512];

    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        const int rc = ::poll(&pfd, 1, remaining);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) {
            return true; // timeout
        }

        if (pfd.revents & POLLIN) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) {
                return true; // EOF
            }
            out.append(buf, buf + n);
        }
    }

    return true;
}

static bool wait_child(pid_t childPid, int timeoutMs, int& status)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const pid_t rc = ::waitpid(childPid, &status, WNOHANG);
        if (rc == childPid) return true;
        if (rc < 0) return false;

        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void test_knxnetip_routing_sniffer_e2e(void)
{
    TEST_ASSERT_FALSE(g_snifferPath.empty());
    const std::string snifferPath = g_snifferPath;
    const std::string iface = "127.0.0.1";

    // Pick a PID-derived group/port to reduce interference across concurrent test runs.
    const uint32_t pid = static_cast<uint32_t>(::getpid());
    const uint8_t octet = static_cast<uint8_t>(1 + (pid % 250));
    const std::string group = std::string("239.255.2.") + std::to_string(octet);
    const uint16_t port = static_cast<uint16_t>(33000 + (pid % 20000));

    int pipefd[2];
    TEST_ASSERT_EQUAL_INT(0, ::pipe(pipefd));

    const pid_t child = ::fork();
    TEST_ASSERT_TRUE(child >= 0);

    if (child == 0) {
        // Child: exec sniffer; redirect stdout+stderr to pipe.
        (void)::close(pipefd[0]);
        (void)::dup2(pipefd[1], STDOUT_FILENO);
        (void)::dup2(pipefd[1], STDERR_FILENO);
        (void)::close(pipefd[1]);

        const std::string portStr = std::to_string(port);
        const std::string timeoutStr = "2000";

        std::vector<char*> args;
        args.push_back(const_cast<char*>(snifferPath.c_str()));
        args.push_back(const_cast<char*>("--group"));
        args.push_back(const_cast<char*>(group.c_str()));
        args.push_back(const_cast<char*>("--port"));
        args.push_back(const_cast<char*>(portStr.c_str()));
        args.push_back(const_cast<char*>("--iface"));
        args.push_back(const_cast<char*>(iface.c_str()));
        args.push_back(const_cast<char*>("--count"));
        args.push_back(const_cast<char*>("1"));
        args.push_back(const_cast<char*>("--timeout-ms"));
        args.push_back(const_cast<char*>(timeoutStr.c_str()));
        args.push_back(nullptr);

        ::execv(args[0], args.data());
        _exit(127);
    }

    // Parent
    (void)::close(pipefd[1]);

    // Give the sniffer a moment to bind/join multicast.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Send one ROUTING_INDICATION.
    RoutingEndpoint sender;
    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    TEST_ASSERT_NOT_NULL(net);
    TEST_ASSERT_TRUE(net->init());
    const knx::IpAddress groupAddr = knx::IpAddress::fromString(group);
    const knx::IpAddress ifaceAddr = knx::IpAddress::fromString(iface);
    knx::netip::RoutingEndpoint::Options senderOpts;
    senderOpts.multicastGroup = groupAddr;
    senderOpts.port = knx::NetIpPort(port);
    senderOpts.interfaceAddress = ifaceAddr;
    TEST_ASSERT_TRUE(sender.open(*net, senderOpts).isOk());

    const LDataFrame frame = makeFrame();
    std::array<uint8_t, kMaxCemiLDataSize> cemi{};
    auto cemiResult = encodeCemiLData(frame, 0x29, cemi);
    TEST_ASSERT_TRUE(cemiResult.isOk());
    const auto sendRes = sender.sendRoutingIndication(std::span<const uint8_t>(cemi.data(), cemiResult.value()));
    TEST_ASSERT_TRUE(sendRes.isOk());
    sender.close();

    // Read sniffer output.
    std::string output;
    TEST_ASSERT_TRUE(read_with_timeout(pipefd[0], output, 4000));
    (void)::close(pipefd[0]);

    // Wait for sniffer to exit.
    int status = 0;
    if (!wait_child(child, 4000, status)) {
        (void)::kill(child, SIGKILL);
        (void)::waitpid(child, &status, 0);
        TEST_FAIL();
    }

    TEST_ASSERT_TRUE(WIFEXITED(status));
    TEST_ASSERT_EQUAL_INT(0, WEXITSTATUS(status));

    // Assert the sniffer actually decoded the packet.
    TEST_ASSERT_TRUE(output.find("ROUTING_INDICATION") != std::string::npos);
    TEST_ASSERT_TRUE(output.find("cEMI msg=0x29") != std::string::npos);
}

int main(int argc, char** argv)
{
    UNITY_BEGIN();
    if (argc < 2) {
        TEST_FAIL();
        return UNITY_END();
    }

    g_snifferPath = argv[1];

    RUN_TEST(test_knxnetip_routing_sniffer_e2e);
    return UNITY_END();
}
