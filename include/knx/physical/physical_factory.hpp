// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file physical_factory.hpp
 * @brief Factory for creating physical layer instances
 */

#pragma once

#include "knx/physical/ip_routing_physical.hpp"
#include "knx/physical/ip_secure_tunneling_physical.hpp"
#include "knx/physical/ip_tunneling_physical.hpp"
#include "knx/physical/null_tp1_medium_backend.hpp"
#include "knx/physical/tp1_mac_physical.hpp"
#include "knx/physical/tpuart_medium_backend_adapter.hpp"
#include "knx/netip/netip_security.hpp"
#include "knx/types.hpp"
#include "knx/physical/bitbang_driver_interface.hpp"
#include "knx/physical/bitbang_driver_tp1_interface.hpp"
#include "knx/util/result.hpp"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace knx {
namespace platform {
class Platform;
class NetworkInterface;
class UartInterface;
}
}

namespace knx {
namespace physical {

enum class Tp1BackendFamily : uint8_t {
	Bitbang,
	Tpuart,
};

struct Tp1BackendSelection {
	Tp1BackendFamily family{Tp1BackendFamily::Bitbang};
};

#if defined(ESP_PLATFORM)
struct Tp1PlatformDependencies {
	BitBangDriverInterface* bitbangDriver{nullptr};
	BitBangDriverTp1Interface* bitbangTp1Driver{nullptr};
	platform::Platform* platform{nullptr};
	platform::UartInterface* uart{nullptr};
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

/**
 * @brief Create TP1 TPUART-composed physical layer over a low-level TP1 physical implementation
 *
 * Wraps a TPUART-class TP1 physical transport behind `Tp1MacController` + `Tp1MediumBackend`
 * composition to share medium-neutral TP1 MAC policy flow.
 *
 * @param physical Low-level TP1 physical implementation (must outlive returned object)
 * @param frameSource Non-owning receive view provider for the same physical path
 * @return TP1 physical layer instance
 */
template <TpuartCompatiblePhysical PhysicalT>
std::unique_ptr<Tp1MacPhysical> createBorrowedTp1TpuartPhysical(PhysicalT& physical,
									Tp1PhysicalFrameSource* frameSource = nullptr)
{
	return std::make_unique<Tp1MacPhysical>(
		std::make_unique<TpuartMediumBackendAdapter>(physical, frameSource));
}

#if defined(ESP_PLATFORM)
/**
 * @brief Create TP1 TPUART-composed physical layer with owned low-level TPUART transport
 *
 * Allocates and owns both low-level TPUART physical transport and MAC-composed adapter,
 * providing a single TP1 physical instance for runtime selection paths.
 *
 * @param platform Platform abstraction used by TPUART transport
 * @param uart UART interface used by TPUART transport
 * @return TP1 physical layer instance
 */
std::unique_ptr<Tp1MacPhysical> createOwnedTp1TpuartPhysical(platform::Platform& platform,
							   platform::UartInterface& uart);

/**
 * @brief Create TP1 physical via backend-family selector using platform-owned TPUART construction
 *
 * For `Tp1BackendFamily::Bitbang`, `bitbangDriver` is required.
 * For `Tp1BackendFamily::Tpuart`, `platform` and `uart` are required.
 */
util::Result<std::unique_ptr<Tp1MacPhysical>> createTp1PhysicalForPlatform(
									const Tp1BackendSelection& selection,
									const Tp1PlatformDependencies& dependencies);
#endif

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

#if KNX_SECURE_ENABLED
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
	uint64_t initialSeq{1};
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
	std::array<uint8_t, 6> serial{};
	std::array<uint8_t, 2> tag{};
	uint64_t initialSeq{1};
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
