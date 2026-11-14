// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

// Interop peer: a KNXnet/IP interface (TunnelingServerEndpoint +
// Tp1IpInterfaceBridge) on a scripted TP1 line, for xknx to tunnel through.
//
// The line reports every transmission with the outcome given by --tx-outcome
// and prints it ("TP1_TX src=a.b.c dst=... group=0|1"). With --inject-ga the
// line "receives" a GroupValueWrite(1) from 1.1.20 once a client is connected.

#include "knx/application/apci_services.hpp"
#include "knx/datalink/tp1_data_link_layer.hpp"
#include "knx/netip/tunneling_server_endpoint.hpp"
#include "knx/network/tp1_ip_interface_bridge.hpp"
#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/platform/linux_platform.hpp"
#include "knx/protocol/tpci.hpp"

#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace knx;

namespace {

uint64_t steadyNowMs()
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
}

bool parseIndividualAddress(const std::string& s, IndividualAddress& out)
{
    int a = 0, b = 0, c = 0;
    if (std::sscanf(s.c_str(), "%d.%d.%d", &a, &b, &c) != 3) {
        return false;
    }
    out = IndividualAddress(static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c));
    return true;
}

bool parseGroupAddress(const std::string& s, GroupAddress& out)
{
    int main = 0, mid = 0, sub = 0;
    if (std::sscanf(s.c_str(), "%d/%d/%d", &main, &mid, &sub) != 3) {
        return false;
    }
    return out.setAddress(static_cast<uint8_t>(main), static_cast<uint8_t>(mid), static_cast<uint8_t>(sub)).isOk();
}

/// A TP1 line with no medium: every transmission ends with a fixed outcome.
class ScriptedTp1Line final : public datalink::Tp1DataLinkLayer {
public:
    ScriptedTp1Line(platform::TaskingPlatform& platform, physical::Tp1MacPhysical& physical, TxProgressState outcome)
        : datalink::Tp1DataLinkLayer(platform, physical)
        , outcome_(outcome)
    {
    }

    util::Result<void> init(const IndividualAddress&) override { return util::Result<void>::ok(); }
    void close() override {}
    bool isOpen() const override { return true; }

    util::Result<void> beginTransmit(const datalink::LDataFrame& frame) override
    {
        const bool group = frame.destinationType == AddressType::Group;
        std::cout << "TP1_TX src=" << static_cast<unsigned>(frame.source.area()) << "."
                  << static_cast<unsigned>(frame.source.line()) << "."
                  << static_cast<unsigned>(frame.source.device());
        if (group) {
            std::cout << " dst=" << static_cast<unsigned>(frame.destination.main()) << "/"
                      << static_cast<unsigned>(frame.destination.middle()) << "/"
                      << static_cast<unsigned>(frame.destination.sub()) << " group=1\n";
        } else {
            const IndividualAddress dst(frame.destination.raw);
            std::cout << " dst=" << static_cast<unsigned>(dst.area()) << "." << static_cast<unsigned>(dst.line())
                      << "." << static_cast<unsigned>(dst.device()) << " group=0\n";
        }
        active_ = true;
        return util::Result<void>::ok();
    }

    util::Result<TxProgressState> pollTransmit() override
    {
        if (!active_) {
            return util::ErrorCode::OperationNotReady;
        }
        active_ = false;
        return outcome_;
    }

    void setReceiveCallback(datalink::LDataCallback callback) override { rx_ = std::move(callback); }

    void receive(const datalink::LDataFrame& frame)
    {
        if (rx_) {
            rx_(frame);
        }
    }

private:
    TxProgressState outcome_;
    bool active_{false};
    datalink::LDataCallback rx_{};
};

} // namespace

int main(int argc, char** argv)
{
    std::cout.setf(std::ios::unitbuf);

    uint16_t port = 3671;
    std::vector<IndividualAddress> tunnels;
    auto outcome = datalink::Tp1DataLinkLayer::TxProgressState::Success;
    std::string injectGa;
    uint32_t injectAfterMs = 300;
    uint32_t timeoutMs = 10000;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (arg == "--port") {
            port = static_cast<uint16_t>(std::stoul(next()));
        } else if (arg == "--tunnel") {
            IndividualAddress address(0);
            if (!parseIndividualAddress(next(), address)) {
                std::cerr << "Invalid --tunnel\n";
                return 2;
            }
            tunnels.push_back(address);
        } else if (arg == "--tx-outcome") {
            const std::string value = next();
            if (value == "success") {
                outcome = datalink::Tp1DataLinkLayer::TxProgressState::Success;
            } else if (value == "timeout") {
                outcome = datalink::Tp1DataLinkLayer::TxProgressState::Timeout;
            } else if (value == "nak") {
                outcome = datalink::Tp1DataLinkLayer::TxProgressState::TransmissionFailed;
            } else {
                std::cerr << "Invalid --tx-outcome\n";
                return 2;
            }
        } else if (arg == "--inject-ga") {
            injectGa = next();
        } else if (arg == "--inject-after-ms") {
            injectAfterMs = static_cast<uint32_t>(std::stoul(next()));
        } else if (arg == "--timeout-ms") {
            timeoutMs = static_cast<uint32_t>(std::stoul(next()));
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return 2;
        }
    }
    if (tunnels.empty()) {
        tunnels = {IndividualAddress(1, 1, 10), IndividualAddress(1, 1, 11)};
    }
    GroupAddress injectAddress;
    if (!injectGa.empty() && !parseGroupAddress(injectGa, injectAddress)) {
        std::cerr << "Invalid --inject-ga\n";
        return 2;
    }

    platform::LinuxPlatform platform;
    auto* net = platform.network();
    if (net == nullptr || net->init().isError()) {
        std::cerr << "Failed to init LinuxPlatform network interface\n";
        return 3;
    }

    physical::Tp1MacPhysical mac(std::make_unique<physical::NullTp1MediumBackend>());
    ScriptedTp1Line line(platform, mac, outcome);
    (void)line.init(IndividualAddress(1, 1, 0));

    netip::TunnelingServerEndpoint server;
    server.setTimingPlatform(&platform);
    auto openResult = server.open(*net, netip::TunnelingServerEndpoint::Options{
                                            .port = NetIpPort(port),
                                            .tunnelAddresses = tunnels,
                                            .dataEndpointAddress = IpAddress::fromOctets(127, 0, 0, 1),
                                        });
    if (openResult.isError()) {
        std::cerr << "Tunnelling server open failed: " << static_cast<int>(openResult.error()) << "\n";
        return 4;
    }

    network::Tp1IpInterfaceBridge bridge(line, server);
    if (bridge.init().isError()) {
        std::cerr << "Bridge init failed\n";
        return 5;
    }

    std::cout << "READY\n";

    const uint64_t start = steadyNowMs();
    uint64_t connectedAt = 0;
    bool injected = false;
    while (steadyNowMs() - start < timeoutMs) {
        (void)server.poll(5);
        bridge.poll();

        if (connectedAt == 0 && server.activeChannelCount() > 0) {
            connectedAt = steadyNowMs();
            std::cout << "CONNECTED\n";
        }
        if (!injectGa.empty() && !injected && connectedAt != 0 && steadyNowMs() - connectedAt >= injectAfterMs) {
            datalink::LDataFrame frame;
            frame.source = IndividualAddress(1, 1, 20);
            frame.setDestination(injectAddress);
            frame.setTpdu(protocol::TPCI::UnnumberedData, application::APCIService::GroupValueWrite, {0x01});
            line.receive(frame);
            injected = true;
            std::cout << "INJECTED " << injectGa << "\n";
        }
    }

    const auto stats = bridge.statistics();
    std::cout << "STATS ip_to_tp1=" << stats.ipToTp1Forwarded << " tp1_to_ip=" << stats.tp1ToIpForwarded
              << " con_pos=" << stats.positiveConfirmations << " con_neg=" << stats.negativeConfirmations << "\n";
    bridge.close();
    server.close();
    return 0;
}
