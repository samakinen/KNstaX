// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file physical_factory.cpp
 * @brief Factory implementations for physical layers
 */

#include "knx/physical/physical_factory.hpp"
#include "knx/physical/bitbang_medium_backend_adapter.hpp"
#include "knx/physical/ip_routing_physical.hpp"
#include "knx/physical/bitbang_driver_tp1_interface.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/util/log.hpp"


namespace knx {
namespace physical {

std::unique_ptr<Tp1MacPhysical> createBorrowedTp1BitbangPhysical(BitBangDriverInterface& driver,
                                                                 BitBangDriverTp1Interface& tp1Driver) {
    return std::make_unique<Tp1MacPhysical>(std::make_unique<BitBangMediumBackendAdapter>(driver, tp1Driver));
}

#if defined(ESP_PLATFORM)
util::Result<std::unique_ptr<Tp1MacPhysical>> createTp1PhysicalForPlatform(
    const Tp1PlatformDependencies& dependencies) {
    if (!dependencies.bitbangDriver || !dependencies.bitbangTp1Driver) {
        return util::ErrorCode::InvalidParameter;
    }
    auto physical = createBorrowedTp1BitbangPhysical(*dependencies.bitbangDriver,
                                                     *dependencies.bitbangTp1Driver);
    if (dependencies.platform) {
        physical->setTimingPlatform(dependencies.platform);
    }
    return physical;
}
#endif

#if KNX_IP_TUNNELING
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
#endif

#if KNX_SECURE_IP_TUNNELING
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
        phys->setSecureRoutingSerial(secureConfig.serial);
        const auto applied = phys->applySecureRouting(secureConfig.groupKey, IpRoutingPhysical::KeySource::Restored);
        if (applied.isError()) {
            KNX_LOGW("KNX.PhysFactory", "Secure Routing key not applied, routing stays plain: %s",
                     util::errorCodeToString(applied.error()));
        }
    }
#else
    (void)secureConfig;
#endif
    return phys;
}

} // namespace physical
} // namespace knx
