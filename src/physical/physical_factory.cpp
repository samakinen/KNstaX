// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file physical_factory.cpp
 * @brief Factory implementations for physical layers
 */

#include "knx/physical/physical_factory.hpp"
#include "knx/physical/bitbang_medium_backend_adapter.hpp"
#include "knx/physical/ip_tunneling_physical.hpp"
#include "knx/physical/ip_routing_physical.hpp"
#include "knx/physical/bitbang_driver_tp1_interface.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tpuart_medium_backend_adapter.hpp"

#if defined(ESP_PLATFORM)
#include "tpuart_physical.hpp"
#endif

#if KNX_SECURE_ENABLED
#include "knx/physical/ip_secure_tunneling_physical.hpp"
#endif

namespace knx {
namespace physical {

#if defined(ESP_PLATFORM)
namespace {

class OwnedTpuartMediumBackend final : public Tp1MediumBackend {
public:
    OwnedTpuartMediumBackend(platform::Platform& platform, platform::UartInterface& uart)
        : _lowLevel(std::make_unique<TpuartPhysical>(platform, platform, uart))
        , _backend(std::make_unique<TpuartMediumBackendAdapter>(*_lowLevel, _lowLevel.get()))
    {
    }

    util::Result<void> init(const Tp1MediumConfig& config) override {
        return _backend->init(config);
    }

    void close() override { _backend->close(); }

    util::Result<size_t> sendFrame(std::span<const uint8_t> frame) override {
        return _backend->sendFrame(frame);
    }

    void setEventCallback(Tp1EventCallback callback, void* context) override {
        _backend->setEventCallback(std::move(callback), context);
    }

    Tp1MediumState getState() const override { return _backend->getState(); }
    Tp1CapabilityProfile getCapabilities() const override { return _backend->getCapabilities(); }
    LinkState getLinkState() const override { return _backend->getLinkState(); }
    util::Result<void> setBusMonitorMode(bool enabled) override { return _backend->setBusMonitorMode(enabled); }
    util::Result<void> service() override { return _backend->service(); }
    void setAckGroupAddresses(std::span<const uint16_t> addresses) override {
        _backend->setAckGroupAddresses(addresses);
    }
    void setLocalBusy(bool busy) override { _backend->setLocalBusy(busy); }
    Tp1MediumDiagnostics* diagnostics() override { return _backend->diagnostics(); }
    const Tp1MediumDiagnostics* diagnostics() const override { return _backend->diagnostics(); }

private:
    std::unique_ptr<TpuartPhysical> _lowLevel;
    std::unique_ptr<TpuartMediumBackendAdapter> _backend;
};

} // namespace
#endif

std::unique_ptr<Tp1MacPhysical> createBorrowedTp1BitbangPhysical(BitBangDriverInterface& driver,
                                                                 BitBangDriverTp1Interface& tp1Driver) {
    return std::make_unique<Tp1MacPhysical>(std::make_unique<BitBangMediumBackendAdapter>(driver, tp1Driver));
}

#if defined(ESP_PLATFORM)
std::unique_ptr<Tp1MacPhysical> createOwnedTp1TpuartPhysical(platform::Platform& platform,
                                                             platform::UartInterface& uart) {
    auto physical = std::make_unique<Tp1MacPhysical>(std::make_unique<OwnedTpuartMediumBackend>(platform, uart));
    physical->setTimingPlatform(&platform);
    return physical;
}

util::Result<std::unique_ptr<Tp1MacPhysical>> createTp1PhysicalForPlatform(
    const Tp1BackendSelection& selection,
    const Tp1PlatformDependencies& dependencies) {
    switch (selection.family) {
        case Tp1BackendFamily::Bitbang:
            if (!dependencies.bitbangDriver || !dependencies.bitbangTp1Driver) {
                return util::ErrorCode::InvalidParameter;
            }
            {
                auto physical = createBorrowedTp1BitbangPhysical(*dependencies.bitbangDriver,
                                                                 *dependencies.bitbangTp1Driver);
                if (dependencies.platform) {
                    physical->setTimingPlatform(dependencies.platform);
                }
                return physical;
            }

        case Tp1BackendFamily::Tpuart:
            if (!dependencies.platform || !dependencies.uart) {
                return util::ErrorCode::InvalidParameter;
            }
            return createOwnedTp1TpuartPhysical(*dependencies.platform, *dependencies.uart);

        default:
            return util::ErrorCode::OperationNotSupported;
    }
}
#endif

std::unique_ptr<IpTunnelingPhysical> createIpTunnelingPhysical(IpAddress host, NetIpPort port) {
    auto phys = std::make_unique<IpTunnelingPhysical>();
    phys->setGateway(host, port);
    return phys;
}

std::unique_ptr<IpTunnelingPhysical> createConfiguredIpTunnelingPhysical(platform::NetworkInterface& network,
                                                                         IpAddress host,
                                                                         NetIpPort port,
                                                                         netip::NetIpSecurity* security) {
    auto phys = std::make_unique<IpTunnelingPhysical>();
    phys->setNetworkInterface(&network);
    phys->setGateway(host, port);
    if (security) {
        phys->setNetIpSecurity(security);
    }
    return phys;
}

#if KNX_SECURE_ENABLED
std::unique_ptr<IpSecureTunnelingPhysical> createIpSecureTunnelingPhysical(IpAddress host, NetIpPort port) {
    auto phys = std::make_unique<IpSecureTunnelingPhysical>();
    phys->setGateway(host, port);
    return phys;
}

std::unique_ptr<IpSecureTunnelingPhysical> createConfiguredIpSecureTunnelingPhysical(platform::NetworkInterface& network,
                                                                                     IpAddress host,
                                                                                     NetIpPort port,
                                                                                     const IpSecureTunnelingConfiguration& config) {
    auto phys = std::make_unique<IpSecureTunnelingPhysical>();
    phys->setNetworkInterface(&network);
    phys->setGateway(host, port);
    phys->setCredentials(config.userId,
                         config.passwordLatin1,
                         config.clientPrivateKey,
                         config.clientSerial,
                         config.initialSeq);
    return phys;
}
#endif

std::unique_ptr<IpRoutingPhysical> createIpRoutingPhysical(IpAddress multicastGroup,
                                                           NetIpPort port,
                                                           IpAddress interfaceAddress) {
    auto phys = std::make_unique<IpRoutingPhysical>();
    phys->setMulticast(multicastGroup, port, interfaceAddress);
    return phys;
}

std::unique_ptr<IpRoutingPhysical> createConfiguredIpRoutingPhysical(platform::NetworkInterface& network,
                                                                     IpAddress multicastGroup,
                                                                     NetIpPort port,
                                                                     IpAddress interfaceAddress,
                                                                     const IpRoutingSecureConfiguration& secureConfig) {
    auto phys = std::make_unique<IpRoutingPhysical>();
    phys->setNetworkInterface(&network);
    phys->setMulticast(multicastGroup, port, interfaceAddress);
#if KNX_SECURE_ENABLED
    if (secureConfig.enabled) {
        phys->setSecureRouting(secureConfig.groupKey,
                               secureConfig.serial,
                               secureConfig.tag,
                               secureConfig.initialSeq);
    }
#else
    (void)secureConfig;
#endif
    return phys;
}

} // namespace physical
} // namespace knx
