// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file physical_factory.hpp
 * @brief Factory for creating physical layer instances
 */

#pragma once

#include "knx/config.hpp"
#include "knx/physical/ip_routing_physical.hpp"
#if KNX_IP_TUNNELING
#include "knx/physical/ip_tunneling_physical.hpp"
#endif
#if KNX_SECURE_IP_TUNNELING
#include "knx/physical/ip_secure_tunneling_physical.hpp"
#endif
#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/netip/netip_security.hpp"
#include "knx/types.hpp"
#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/bitbang_driver_tp1_interface.hpp"
#include "knx/util/result.hpp"
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace knx {
namespace platform {
class Platform;
class NetworkInterface;
}
}

namespace knx {
namespace physical {

#if defined(ESP_PLATFORM)
/// What `createTp1PhysicalForPlatform()` needs. TP1 has one backend, the
/// software bit-bang driver; a transceiver chip with its own MAC (an NCN5120,
/// say) would be another `Tp1MediumBackend` and add its own dependencies here.
struct Tp1PlatformDependencies {
	BitBangDriverInterface* bitbangDriver{nullptr};
	BitBangDriverTp1Interface* bitbangTp1Driver{nullptr};
	/// Optional: timing source for the MAC; nullptr keeps the default.
	platform::Platform* platform{nullptr};
};
#endif

/**
 * @brief Create TP1 bit-bang physical layer with injected driver
 * 
 * Creates a platform-independent TP1 physical layer that uses the
 * provided driver for platform-specific operations.
 * 
 * @param driver Platform-specific bit-bang driver (must outlive returned object)
 * @return TP1 physical layer instance
 */
std::unique_ptr<Tp1MacPhysical> createBorrowedTp1BitbangPhysical(BitBangDriverInterface& driver,
						BitBangDriverTp1Interface& tp1Driver);

#if defined(ESP_PLATFORM)
/**
 * @brief Create the TP1 physical layer for an ESP-IDF build
 *
 * `bitbangDriver` and `bitbangTp1Driver` are required.
 */
util::Result<std::unique_ptr<Tp1MacPhysical>> createTp1PhysicalForPlatform(
									const Tp1PlatformDependencies& dependencies);
#endif

#if KNX_IP_TUNNELING
/**
 * @brief Create an IP Tunneling physical adapter
 * @param host KNXnet/IP gateway host
 * @param port KNXnet/IP gateway port
 */
std::unique_ptr<IpTunnelingPhysical> createIpTunnelingPhysical(IpAddress host, NetIpPort port);

/**
 * @brief Create and configure an IP Tunneling physical adapter for immediate use.
 * @param network Network interface used by the physical
 * @param host KNXnet/IP gateway host
 * @param port KNXnet/IP gateway port
 * @param security Optional KNXnet/IP datagram security wrapper
 */
std::unique_ptr<IpTunnelingPhysical> createConfiguredIpTunnelingPhysical(platform::NetworkInterface& network,
										  IpAddress host,
										  NetIpPort port,
										  netip::NetIpSecurity* security = nullptr);
#endif

#if KNX_SECURE_IP_TUNNELING
/**
 * @brief Create an IP Secure Tunneling physical adapter (TCP + KNX/IP Secure)
 *
 * NOTE: The returned adapter requires an injected NetworkInterface via
 * `setNetworkInterface()` before it can be initialized.
 */
std::unique_ptr<IpSecureTunnelingPhysical> createIpSecureTunnelingPhysical(IpAddress host, NetIpPort port);

struct IpSecureTunnelingConfiguration {
	UserId userId{UserId(1)};
	std::vector<uint8_t> passwordLatin1;
	std::array<uint8_t, 32> clientPrivateKey{};
	std::array<uint8_t, 6> clientSerial{};
	/// "initialized to zero on connection setup" (03/08/09 §2.2.3.3).
	uint64_t initialSeq{0};
	/// The server's Device Authentication Code (key), to verify its
	/// SESSION_RESPONSE MAC; unset skips the check.
	std::optional<std::array<uint8_t, 16>> deviceAuthenticationCode{};
};

/**
 * @brief Create and configure an IP Secure Tunneling physical adapter for immediate use.
 * @param network Network interface used by the physical
 * @param host KNXnet/IP gateway host
 * @param port KNXnet/IP gateway port
 * @param config Secure tunneling credential and identity configuration
 */
std::unique_ptr<IpSecureTunnelingPhysical> createConfiguredIpSecureTunnelingPhysical(platform::NetworkInterface& network,
												 IpAddress host,
												 NetIpPort port,
												 const IpSecureTunnelingConfiguration& config);
#endif

/**
 * @brief Create an IP Routing (multicast) physical adapter
 * @param multicastGroup Multicast group (default KNX: 224.0.23.12)
 * @param port UDP port (default KNX: 3671)
 * @param interfaceAddress IPv4 address of egress/join interface (0.0.0.0 for default)
 */
std::unique_ptr<IpRoutingPhysical> createIpRoutingPhysical(IpAddress multicastGroup,
					 NetIpPort port,
									 IpAddress interfaceAddress = IpAddress(0));

struct IpRoutingSecureConfiguration {
#if KNX_SECURE_ENABLED
	bool enabled{false};
	std::array<uint8_t, 16> groupKey{};
	/// The device's KNX serial number. The tag is random per frame and the
	/// sequence information is the multicast timer, so neither is configured.
	std::array<uint8_t, 6> serial{};
#endif
};

/**
 * @brief Create and configure an IP Routing physical adapter for immediate use.
 * @param network Network interface used by the physical
 * @param multicastGroup Multicast group (default KNX: 224.0.23.12)
 * @param port UDP port (default KNX: 3671)
 * @param interfaceAddress IPv4 address of egress/join interface
 * @param secureConfig Optional KNX/IP Secure routing settings
 */
std::unique_ptr<IpRoutingPhysical> createConfiguredIpRoutingPhysical(platform::NetworkInterface& network,
											IpAddress multicastGroup,
											NetIpPort port,
											IpAddress interfaceAddress = IpAddress(0),
											const IpRoutingSecureConfiguration& secureConfig = {});

} // namespace physical
} // namespace knx
