// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/bau/bau.hpp"
#include "knx/physical/physical_factory.hpp"
#include "knx/physical/ip_tunneling_physical.hpp"
#include "knx/util/result.hpp"

#include "knx/platform/linux_platform.hpp"
#include "knx/netip/netip_config.hpp"

#if KNX_SECURE_ENABLED
#include "knx/physical/ip_secure_tunneling_physical.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
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
    std::string gwHost{"127.0.0.1"};
    uint16_t gwPort{knx::netip::config::kDefaultPort};
    std::string own{"1.1.1"};
    std::string ga{"1/0/0"};
    uint8_t dptType{1};
    int timeoutMs{2000};
    int stayAliveMs{0};
    bool send{false};
    uint8_t sendByte{0x01};
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

    bool ipSecure{false};
    UserId ipSecureUserId{UserId(1)};
    std::string ipSecurePassword{"password"};
    bool ipSecureClientPrivateKeySet{false};
    std::array<uint8_t, 32> ipSecureClientPrivateKey{};
    bool ipSecureClientSerialSet{false};
    std::array<uint8_t, 6> ipSecureClientSerial{};
    uint64_t ipSecureInitialSeq{1};
};

static void usage(const char* argv0)
{
    std::cerr
    << "Usage: " << argv0 << " --gw-host 127.0.0.1 --gw-port " << knx::netip::config::kDefaultPort << " --own 1.1.1 --ga 1/0/0"
    << " [--dpt-type 1] [--send 01] [--init 00] [--init-float 21.5] [--expect-read] [--no-expect-rx]"
    << " [--data-secure --data-secure-key-hex <32-hex>] [--data-secure-send-seq <n>]"
    << " [--ip-secure --ip-secure-user-id <n> --ip-secure-password <latin1> --ip-secure-client-private-key-hex <64-hex> --ip-secure-client-serial-hex <12-hex> --ip-secure-initial-seq <n>]"
    << " [--timeout-ms 2000] [--stay-alive-ms 0]\n";
}

static bool parseArgs(int argc, char** argv, Args& args)
{
    auto hexNibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "--gw-host") {
            const char* v = need("--gw-host");
            if (!v) return false;
            args.gwHost = v;
        } else if (a == "--gw-port") {
            const char* v = need("--gw-port");
            if (!v) return false;
            args.gwPort = static_cast<uint16_t>(std::stoi(v));
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
        } else if (a == "--ip-secure") {
            args.ipSecure = true;
        } else if (a == "--ip-secure-user-id") {
            const char* v = need("--ip-secure-user-id");
            if (!v) return false;
            const int uid = std::stoi(v);
            if (uid < 0 || uid > 255) {
                std::cerr << "Invalid --ip-secure-user-id: " << v << "\n";
                return false;
            }
            args.ipSecureUserId = UserId(static_cast<uint8_t>(uid));
        } else if (a == "--ip-secure-password") {
            const char* v = need("--ip-secure-password");
            if (!v) return false;
            args.ipSecurePassword = v;
        } else if (a == "--ip-secure-client-private-key-hex") {
            const char* v = need("--ip-secure-client-private-key-hex");
            if (!v) return false;
            const std::string hex(v);
            if (hex.size() != 64) {
                std::cerr << "--ip-secure-client-private-key-hex must be 64 hex chars (32 bytes)\n";
                return false;
            }
            for (size_t bi = 0; bi < 32; ++bi) {
                const int hi = hexNibble(hex[2 * bi]);
                const int lo = hexNibble(hex[2 * bi + 1]);
                if (hi < 0 || lo < 0) {
                    std::cerr << "Invalid hex in --ip-secure-client-private-key-hex\n";
                    return false;
                }
                args.ipSecureClientPrivateKey[bi] = static_cast<uint8_t>((hi << 4) | lo);
            }
            args.ipSecureClientPrivateKeySet = true;
        } else if (a == "--ip-secure-client-serial-hex") {
            const char* v = need("--ip-secure-client-serial-hex");
            if (!v) return false;
            const std::string hex(v);
            if (hex.size() != 12) {
                std::cerr << "--ip-secure-client-serial-hex must be 12 hex chars (6 bytes)\n";
                return false;
            }
            for (size_t bi = 0; bi < 6; ++bi) {
                const int hi = hexNibble(hex[2 * bi]);
                const int lo = hexNibble(hex[2 * bi + 1]);
                if (hi < 0 || lo < 0) {
                    std::cerr << "Invalid hex in --ip-secure-client-serial-hex\n";
                    return false;
                }
                args.ipSecureClientSerial[bi] = static_cast<uint8_t>((hi << 4) | lo);
            }
            args.ipSecureClientSerialSet = true;
        } else if (a == "--ip-secure-initial-seq") {
            const char* v = need("--ip-secure-initial-seq");
            if (!v) return false;
            args.ipSecureInitialSeq = static_cast<uint64_t>(std::stoull(v));
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
        std::cerr << "PEER(tunnel) ARGS: gwHost=" << args.gwHost << " gwPort=" << args.gwPort
                  << " own=" << args.own << " ga=" << args.ga << " dptType=" << int(args.dptType)
                  << " ipSecure=" << (args.ipSecure ? 1 : 0) << " dataSecure=" << (args.dataSecure ? 1 : 0)
                  << " timeoutMs=" << args.timeoutMs << " stayAliveMs=" << args.stayAliveMs << "\n";
    } catch (...) {}

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

    // Network interface is required for TCP secure tunneling.
    knx::platform::LinuxPlatform platform;

    std::unique_ptr<BusAccessStackPort> stackPort;

#if KNX_SECURE_ENABLED
    if (args.ipSecure) {
        if (!args.ipSecureClientPrivateKeySet || !args.ipSecureClientSerialSet) {
            std::cerr << "--ip-secure requires --ip-secure-client-private-key-hex and --ip-secure-client-serial-hex\n";
            return 2;
        }
        try {
            std::cerr << "PEER(tunnel): created IP Secure physical; client_serial=";
            for (auto b : args.ipSecureClientSerial) {
                char buf[3];
                std::snprintf(buf, sizeof(buf), "%02x", b);
                std::cerr << buf;
            }
            std::cerr << " user_id=" << int(args.ipSecureUserId.value()) << " initial_seq=" << args.ipSecureInitialSeq << "\n";
        } catch (...) {}

        std::vector<uint8_t> pwd;
        pwd.assign(args.ipSecurePassword.begin(), args.ipSecurePassword.end());

        IpSecureTunnelingConfiguration secureConfig;
        secureConfig.userId = args.ipSecureUserId;
        secureConfig.passwordLatin1 = std::move(pwd);
        secureConfig.clientPrivateKey = args.ipSecureClientPrivateKey;
        secureConfig.clientSerial = args.ipSecureClientSerial;
        secureConfig.initialSeq = args.ipSecureInitialSeq;

        stackPort = createTp1StackPort(platform,
                                       createConfiguredIpSecureTunnelingPhysical(*platform.network(),
                                                                                 IpAddress::fromString(args.gwHost),
                                                                                 NetIpPort(args.gwPort),
                                                                                 secureConfig));
    } else {
        stackPort = createTp1StackPort(platform,
                                       createConfiguredIpTunnelingPhysical(*platform.network(),
                                                                           IpAddress::fromString(args.gwHost),
                                                                           NetIpPort(args.gwPort)));
    }
#else
    stackPort = createTp1StackPort(platform,
                                   createConfiguredIpTunnelingPhysical(*platform.network(),
                                                                       IpAddress::fromString(args.gwHost),
                                                                       NetIpPort(args.gwPort)));
#endif

    if (!args.ipSecure) {
        try {
            std::cerr << "PEER(tunnel): created IP Tunneling physical; gw=" << args.gwHost << ":" << args.gwPort << "\n";
        } catch (...) {}
    }

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
        // Receiving sequence starts at 0; replay window accepts first seen seq.
        bau.securityObject().setReceivingSequence(0);
    }
#endif

    std::atomic<bool> gotWrite{false};
    std::atomic<bool> gotRead{false};
    // Set this once we're fully ready to wait for traffic (after printing READY).
    std::atomic<uint64_t> lastProgressMs{0};
    std::vector<uint8_t> gotData;

    auto startRes = bau.init(own);
    if (startRes.isError()) {
        std::cerr << "Failed to start KNstaX tunnel peer (" << util::errorCodeToString(startRes.error())
                  << ")\n";
        return 3;
    }
    try { std::cerr << "PEER(tunnel): bau started with own=" << args.own << "\n"; } catch(...) {}

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
        // Initialize the group object value so GroupValueRead yields a deterministic response.
        // For DPT1/DPT5 we use the byte form; for DPT9 we support an explicit float.
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

    // From this point on we expect to receive relevant traffic.
    lastProgressMs.store(steadyNowMs());

    if (args.send) {
        (void)bau.sendGroupValue(ga, std::vector<uint8_t>{args.sendByte});
        std::cout << "SENT " << args.ga << "\n";
        lastProgressMs.store(steadyNowMs());
    }

    try {
        std::cerr << "PEER(tunnel): entering wait loop expectWrite=" << (args.expectWrite?1:0) << " expectRead=" << (args.expectRead?1:0) << "\n";
    } catch(...) {}

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

            // Idle timeout: reset by callbacks on relevant events.
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
