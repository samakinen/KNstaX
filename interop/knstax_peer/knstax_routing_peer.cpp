// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/bau/bau.hpp"
#include "knx/physical/physical_factory.hpp"
#include "knx/util/result.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/platform/network_interface.hpp"
#include "knx/netip/netip_config.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace knx;
using namespace knx::bau;
using namespace knx::physical;
using namespace knx::application;

namespace {

static uint64_t steadyNowMs()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

static bool parseIndividualAddress(const std::string& s, IndividualAddress& out)
{
    // Accept either raw hex (e.g. 0x1122) or dotted (a.b.c).
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
        uint32_t v = 0;
        std::stringstream ss;
        ss << std::hex << s;
        ss >> v;
        out = IndividualAddress(static_cast<uint16_t>(v & 0xFFFF));
        return true;
    }

    int a = 0, b = 0, c = 0;
    if (std::sscanf(s.c_str(), "%d.%d.%d", &a, &b, &c) == 3) {
        out = IndividualAddress(static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c));
        return true;
    }
    return false;
}

static bool parseGroupAddress3(const std::string& s, GroupAddress& out)
{
    int main = 0, mid = 0, sub = 0;
    if (std::sscanf(s.c_str(), "%d/%d/%d", &main, &mid, &sub) == 3) {
        return out.setAddress(static_cast<uint8_t>(main), static_cast<uint8_t>(mid), static_cast<uint8_t>(sub)).isOk();
    }
    // Also accept raw hex.
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
        uint32_t v = 0;
        std::stringstream ss;
        ss << std::hex << s;
        ss >> v;
        out = GroupAddress(static_cast<uint16_t>(v & 0xFFFF));
        return true;
    }
    return false;
}

struct Args {
    std::string group{"239.255.4.1"};
    uint16_t port{knx::netip::config::kDefaultPort};
    std::string iface{"127.0.0.1"};

    std::string own{"1.1.1"};
    std::string ga{"1/0/0"};
    uint8_t dptType{1};

    int timeoutMs{2000};
    int stayAliveMs{0};

    bool send{false};
    uint8_t sendByte{0x01};
    std::vector<uint8_t> sendData;

    bool expectWrite{true};
    bool expectRead{false};

    bool initSet{false};
    uint8_t initByte{0x00};
    bool initFloatSet{false};
    float initFloat{0.0f};
    bool dataSecure{false};
    bool dataSecureKeySet{false};
    std::array<uint8_t, 16> dataSecureKey{};
    uint64_t dataSecureSendSeq{1};
};

static void usage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0
        << " --group 239.255.4.1 --port " << knx::netip::config::kDefaultPort << " --iface-address 127.0.0.1 --own 1.1.1 --ga 1/0/0"
        << " [--dpt-type 1] [--send 01] [--send-hex a1b2] [--init 00] [--init-float 21.5] [--expect-read] [--no-expect-rx]"
    << " [--data-secure --data-secure-key-hex <32-hex>] [--data-secure-send-seq <n>]"
    << " [--timeout-ms 2000] [--stay-alive-ms 0]\n";
}

static bool parseHexBytes(const std::string& s, std::vector<uint8_t>& out)
{
    out.clear();
    std::string hex;
    hex.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == ':' || c == '-') continue;
        hex.push_back(c);
    }
    if (hex.empty() || (hex.size() % 2) != 0) {
        return false;
    }
    out.reserve(hex.size() / 2);
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = hexval(hex[i]);
        const int lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>(((hi & 0x0F) << 4) | (lo & 0x0F)));
    }
    return true;
}

static bool parseArgs(int argc, char** argv, Args& args)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--group") {
            const char* v = need("--group");
            if (!v) return false;
            args.group = v;
        } else if (a == "--port") {
            const char* v = need("--port");
            if (!v) return false;
            args.port = static_cast<uint16_t>(std::stoi(v));
        } else if (a == "--iface-address") {
            const char* v = need("--iface-address");
            if (!v) return false;
            args.iface = v;
        } else if (a == "--own") {
            const char* v = need("--own");
            if (!v) return false;
            args.own = v;
        } else if (a == "--ga") {
            const char* v = need("--ga");
            if (!v) return false;
            args.ga = v;
        } else if (a == "--dpt-type") {
            const char* v = need("--dpt-type");
            if (!v) return false;
            const int dt = std::stoi(v);
            if (dt < 0 || dt > 255) {
                std::cerr << "Invalid --dpt-type: " << v << "\n";
                return false;
            }
            args.dptType = static_cast<uint8_t>(dt);
        } else if (a == "--timeout-ms") {
            const char* v = need("--timeout-ms");
            if (!v) return false;
            args.timeoutMs = std::stoi(v);
        } else if (a == "--stay-alive-ms") {
            const char* v = need("--stay-alive-ms");
            if (!v) return false;
            args.stayAliveMs = std::stoi(v);
        } else if (a == "--send") {
            const char* v = need("--send");
            if (!v) return false;
            uint32_t bv = 0;
            std::stringstream ss;
            ss << std::hex << v;
            ss >> bv;
            args.send = true;
            args.sendByte = static_cast<uint8_t>(bv & 0xFF);
            args.sendData = {args.sendByte};
        } else if (a == "--send-hex") {
            const char* v = need("--send-hex");
            if (!v) return false;
            args.send = true;
            if (!parseHexBytes(v, args.sendData) || args.sendData.empty()) {
                std::cerr << "Invalid --send-hex: " << v << "\n";
                return false;
            }
        } else if (a == "--data-secure") {
            args.dataSecure = true;
        } else if (a == "--data-secure-key-hex") {
            const char* v = need("--data-secure-key-hex");
            if (!v) return false;
            const std::string hex(v);
            if (hex.size() != 32) {
                std::cerr << "--data-secure-key-hex must be 32 hex chars (16 bytes)\n";
                return false;
            }
            auto hexNibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            for (size_t bi = 0; bi < 16; ++bi) {
                const int hi = hexNibble(hex[2 * bi]);
                const int lo = hexNibble(hex[2 * bi + 1]);
                if (hi < 0 || lo < 0) {
                    std::cerr << "Invalid hex in --data-secure-key-hex\n";
                    return false;
                }
                args.dataSecureKey[bi] = static_cast<uint8_t>((hi << 4) | lo);
            }
            args.dataSecureKeySet = true;
        } else if (a == "--data-secure-send-seq") {
            const char* v = need("--data-secure-send-seq");
            if (!v) return false;
            args.dataSecureSendSeq = static_cast<uint64_t>(std::stoull(v));
        } else if (a == "--init") {
            const char* v = need("--init");
            if (!v) return false;
            uint32_t bv = 0;
            std::stringstream ss;
            ss << std::hex << v;
            ss >> bv;
            args.initSet = true;
            args.initByte = static_cast<uint8_t>(bv & 0xFF);
        } else if (a == "--init-float") {
            const char* v = need("--init-float");
            if (!v) return false;
            args.initFloatSet = true;
            args.initFloat = std::stof(v);
        } else if (a == "--expect-read") {
            args.expectRead = true;
        } else if (a == "--no-expect-rx") {
            args.expectWrite = false;
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return false;
        } else {
            std::cerr << "Unknown arg: " << a << "\n";
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Args args;
    if (!parseArgs(argc, argv, args)) {
        return 2;
    }

    // Print parsed args for debugging (to stderr so tests can still read READY from stdout)
    try {
        std::cerr << "PEER(routing) ARGS: group=" << args.group << " port=" << args.port
                  << " iface=" << args.iface << " own=" << args.own << " ga=" << args.ga
                  << " dptType=" << int(args.dptType)
                  << " dataSecure=" << (args.dataSecure ? 1 : 0)
                  << " timeoutMs=" << args.timeoutMs << " stayAliveMs=" << args.stayAliveMs << "\n";
    } catch (...) {
    }

    // Make stdout usable for synchronization in tests (stdout is a pipe there).
    std::cout.setf(std::ios::unitbuf);

    IndividualAddress own;
    if (!parseIndividualAddress(args.own, own)) {
        std::cerr << "Invalid --own: " << args.own << "\n";
        return 2;
    }

    GroupAddress ga;
    if (!parseGroupAddress3(args.ga, ga)) {
        std::cerr << "Invalid --ga: " << args.ga << "\n";
        return 2;
    }

    knx::platform::LinuxPlatform platform;
    auto* net = platform.network();
    if (!net || !net->init()) {
        std::cerr << "Failed to init LinuxPlatform network interface\n";
        return 3;
    }

    auto stackPort = createTp1StackPort(platform,
                                        createConfiguredIpRoutingPhysical(*net,
                                                                          IpAddress::fromString(args.group),
                                                                          NetIpPort(args.port),
                                                                          IpAddress::fromString(args.iface)));
    BusAccessUnit bau(platform, std::move(stackPort));

#if KNX_SECURE_ENABLED
    if (args.dataSecure || args.dataSecureKeySet) {
        if (!args.dataSecureKeySet) {
            std::cerr << "--data-secure requires --data-secure-key-hex\n";
            return 2;
        }
        bau.securityObject().setSecurityMode(objects::SecurityMode::Enabled);
        bau.securityObject().setGroupKey(ga, args.dataSecureKey);
        bau.securityObject().setSendingSequence(args.dataSecureSendSeq);
        bau.securityObject().setReceivingSequence(0);
    }
#endif

    std::atomic<bool> gotWrite{false};
    std::atomic<bool> gotRead{false};
    std::atomic<uint64_t> lastProgressMs{0};
    std::vector<uint8_t> gotData;

    auto startRes = bau.init(own);
    if (startRes.isError()) {
        std::cerr << "Failed to start KNstaX routing peer (" << util::errorCodeToString(startRes.error())
                  << ")\n";
        return 3;
    }
    try {
        std::cerr << "PEER(routing): bau started with own=" << args.own << "\n";
    } catch (...) {
    }

    const knx::GroupObjectIndex idx = bau.addGroupObject(ga,
                                                         application::makeDptId(args.dptType),
                                                         true,
                                                         true,
                                                         true,
                                                         true);
    if (!idx.isValid()) {
        std::cerr << "Failed to create group object\n";
        bau.close();
        return 3;
    }

    bau.setGroupObjectWriteCallback([&](knx::GroupObjectIndex, std::span<const uint8_t> data) {
        gotData.assign(data.begin(), data.end());
        gotWrite.store(true);
        lastProgressMs.store(steadyNowMs());
        std::cout << "EVENT group_write " << args.ga << " size=" << data.size() << "\n";
    });

    bau.setGroupObjectReadCallback([&](knx::GroupObjectIndex) {
        gotRead.store(true);
        lastProgressMs.store(steadyNowMs());
        std::cout << "EVENT group_read " << args.ga << "\n";
    });

    if (args.initSet || args.initFloatSet) {
        util::Result<void> initRes = util::Result<void>::ok();
        if (args.dptType == 1) {
            initRes = bau.setGroupObjectValue(idx, application::DptValue(args.initByte != 0));
        } else if (args.dptType == 5) {
            initRes = bau.setGroupObjectValue(idx, application::DptValue(args.initByte));
        } else if (args.dptType == 9) {
            const float v = args.initFloatSet ? args.initFloat : static_cast<float>(args.initByte);
            initRes = bau.setGroupObjectValue(idx, application::DptValue(v));
        }

        if (initRes.isError()) {
            std::cerr << "Failed to initialize group object value ("
                      << util::errorCodeToString(initRes.error()) << ")\n";
        }
    }

    std::cout << "READY\n";
    std::cout.flush();

    lastProgressMs.store(steadyNowMs());

    if (args.send) {
        if (args.sendData.empty()) {
            args.sendData = {args.sendByte};
        }
        (void)bau.sendGroupValue(ga, args.sendData);
        std::cout << "SENT " << args.ga << "\n";
        lastProgressMs.store(steadyNowMs());
    }

    if (args.expectWrite || args.expectRead) {
        bool brokeOnIdleTimeout = false;
        while (true) {
            bau.loop();

            const uint64_t nowMs = steadyNowMs();
            const uint64_t lastMs = lastProgressMs.load();

            const bool okWrite = (!args.expectWrite) || gotWrite.load();
            const bool okRead = (!args.expectRead) || gotRead.load();
            if (okWrite && okRead) {
                break;
            }

            if (lastMs != 0 && nowMs >= lastMs && (nowMs - lastMs) >= static_cast<uint64_t>(args.timeoutMs)) {
                brokeOnIdleTimeout = true;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        bau.close();

        if (brokeOnIdleTimeout) {
            const uint64_t nowMs = steadyNowMs();
            const uint64_t lastMs = lastProgressMs.load();
            const uint64_t deltaMs = (nowMs >= lastMs) ? (nowMs - lastMs) : 0;
            std::cerr << "[DEBUG] idle-timeout fired: nowMs=" << nowMs << " lastMs=" << lastMs
                      << " deltaMs=" << deltaMs << " timeoutMs=" << args.timeoutMs
                      << " expectWrite=" << (args.expectWrite ? 1 : 0)
                      << " expectRead=" << (args.expectRead ? 1 : 0)
                      << " gotWrite=" << (gotWrite.load() ? 1 : 0)
                      << " gotRead=" << (gotRead.load() ? 1 : 0) << "\n";
        }

        if (args.expectWrite && !gotWrite.load()) {
            std::cerr << "Timeout waiting for group write\n";
            return 4;
        }
        if (args.expectWrite && gotData.empty()) {
            std::cerr << "Received empty payload\n";
            return 5;
        }
        if (args.expectRead && !gotRead.load()) {
            std::cerr << "Timeout waiting for group read\n";
            return 6;
        }
        return 0;
    }

    if (args.stayAliveMs > 0) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.stayAliveMs);
        while (std::chrono::steady_clock::now() < deadline) {
            bau.loop();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (std::chrono::steady_clock::now() < deadline) {
            bau.loop();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    bau.close();
    return 0;
}
