// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/routing_endpoint.hpp"
#include "knx/netip/cemi.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/util/result.hpp"

#include "knx/platform/linux_platform.hpp"
#include "knx/netip/netip_config.hpp"

#include <chrono>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace knx;

static void usage(const char* argv0) {
    std::cerr
        << "Usage: " << argv0
        << " [--group <mcast>] [--port <port>] [--iface <ip>] [--timeout-ms <ms>] [--count <n>]\n"
        << "Defaults: --group 224.0.23.12 --port " << knx::netip::config::kDefaultPort
        << " --iface 0.0.0.0 --timeout-ms 1000 --count 0(unlimited)\n";
}

static bool parseArg(int& i, int argc, char** argv, const char* name, std::string& out) {
    if (std::string(argv[i]) != name) return false;
    if (i + 1 >= argc) return false;
    out = argv[++i];
    return true;
}

static bool parseArgInt(int& i, int argc, char** argv, const char* name, uint32_t& out) {
    if (std::string(argv[i]) != name) return false;
    if (i + 1 >= argc) return false;
    out = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
    return true;
}

int main(int argc, char** argv) {
    std::string group = "224.0.23.12";
    uint32_t portU32 = knx::netip::config::kDefaultPort;
    std::string iface = "0.0.0.0";
    uint32_t timeoutMs = 1000;
    uint32_t count = 0;

    for (int i = 1; i < argc; ++i) {
        std::string s;
        uint32_t v = 0;
        if (parseArg(i, argc, argv, "--group", s)) {
            group = s;
        } else if (parseArgInt(i, argc, argv, "--port", v)) {
            portU32 = v;
        } else if (parseArg(i, argc, argv, "--iface", s)) {
            iface = s;
        } else if (parseArgInt(i, argc, argv, "--timeout-ms", v)) {
            timeoutMs = v;
        } else if (parseArgInt(i, argc, argv, "--count", v)) {
            count = v;
        } else if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown arg: " << argv[i] << "\n";
            usage(argv[0]);
            return 2;
        }
    }

    if (portU32 > 65535) {
        std::cerr << "Invalid --port\n";
        return 2;
    }

    netip::RoutingEndpoint ep;

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    if (!net || !net->init()) {
        std::cerr << "Failed to init platform network interface\n";
        return 1;
    }

    netip::RoutingEndpoint::Options opt;
    opt.multicastGroup = knx::IpAddress::fromString(group);
    opt.port = knx::NetIpPort(static_cast<uint16_t>(portU32));
    opt.interfaceAddress = knx::IpAddress::fromString(iface);

    if (!ep.open(*net, opt)) {
        std::cerr << "Failed to open routing endpoint (group=" << group << " port=" << portU32
                  << " iface=" << iface << ")\n";
        return 1;
    }

    std::cout << "Listening for KNXnet/IP ROUTING_INDICATION on " << group << ":" << portU32
              << " (iface=" << iface << ")\n";

    uint32_t receivedCount = 0;
    for (;;) {
        std::array<uint8_t, netip::kMaxCemiLDataSize> cemi{};
        const auto recvRes = ep.receiveRoutingIndication(cemi, timeoutMs);
        if (recvRes.isError()) {
            if (count != 0) {
                std::cout << "[timeout] received=" << receivedCount << " expected=" << count
                          << " (no packet within " << timeoutMs << "ms)\n";
                return 3;
            }
            continue;
        }

        datalink::LDataFrame frame;
        uint8_t msgCode = 0;
        if (!netip::decodeCemiLData(std::span<const uint8_t>(cemi).first(recvRes.value()), frame, msgCode)) {
            std::cout << "[drop] invalid cEMI len=" << recvRes.value() << "\n";
            continue;
        }

        const auto src = frame.source;
        const auto dst = frame.destination;

        std::cout << "cEMI msg=0x" << std::hex << static_cast<unsigned>(msgCode) << std::dec
                  << " src=" << static_cast<unsigned>(src.area()) << "." << static_cast<unsigned>(src.line())
                  << "." << static_cast<unsigned>(src.device());

        if (isGroupAddress(frame.destinationType)) {
            std::cout << " dst(group)=" << static_cast<unsigned>(dst.main()) << "/"
                      << static_cast<unsigned>(dst.middle()) << "/" << static_cast<unsigned>(dst.sub());
        } else {
            IndividualAddress ind(dst.raw);
            std::cout << " dst(ind)=" << static_cast<unsigned>(ind.area()) << "." << static_cast<unsigned>(ind.line())
                      << "." << static_cast<unsigned>(ind.device());
        }

        std::cout << " hop=" << static_cast<unsigned>(frame.hopCount)
                  << " apci=0x" << std::hex << static_cast<unsigned>(frame.apci().raw) << std::dec
                  << " tpduLen=" << frame.tpdu.size() << " payloadLen=" << frame.payload().size()
                  << "\n";

        ++receivedCount;
        if (count != 0 && receivedCount >= count) {
            return 0;
        }
    }

    return 0;
}
