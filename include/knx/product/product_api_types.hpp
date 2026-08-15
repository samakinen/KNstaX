// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file product_api_types.hpp
 * @brief Product-layer public types for medium selection and IP configuration.
 *
 * The product layer should expose product-owned public types wherever practical.
 * Preferred product-facing APIs should not leak facade-owned type names.
 */

#pragma once

#include "knx/application/dpt.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/types.hpp"

#include <array>
#include <variant>
#include <vector>

namespace knx::product {

enum class AutomaticResponseMode : uint8_t {
	Immediate,
	Deferred,
};

struct DatapointBindingConfig {
	GroupAddress address;
	application::DptId dpt;
	bool readable{false};
	bool writable{false};
	bool transmit{false};
	bool receivable{false};
};

enum class Medium : uint8_t {
	TP1,
	IP_Tunneling,
	IP_Routing,
#if KNX_SECURE_ENABLED
	IP_Secure_Tunneling,
#endif
};

struct IpTunnelingConfig {
	IpAddress gatewayHost = IpAddress::fromOctets(127, 0, 0, 1);
	NetIpPort gatewayPort = NetIpPort(netip::config::kDefaultPort);
};

struct IpRoutingConfig {
	IpAddress multicastGroup = IpAddress::fromOctets(224, 0, 23, 12);
	NetIpPort port = NetIpPort(netip::config::kDefaultPort);
	IpAddress interfaceAddress = IpAddress(0);
#if KNX_SECURE_ENABLED
	bool secureRoutingEnabled = false;
	std::array<uint8_t, 16> secureRoutingGroupKey{};
	std::array<uint8_t, 6> secureRoutingSerial{};
	std::array<uint8_t, 2> secureRoutingTag{};
	uint64_t secureRoutingInitialSeq = 1;
#endif
};

#if KNX_SECURE_ENABLED
struct IpSecureTunnelingConfig {
	IpAddress gatewayHost = IpAddress::fromOctets(127, 0, 0, 1);
	NetIpPort gatewayPort = NetIpPort(netip::config::kDefaultPort);

	UserId userId = UserId(1);
	std::vector<uint8_t> passwordLatin1;
	std::array<uint8_t, 32> clientPrivateKey{};
	std::array<uint8_t, 6> clientSerial{};
	uint64_t initialSeq = 1;
};
#endif

#if KNX_SECURE_ENABLED
using NetworkBuildConfig = std::variant<IpTunnelingConfig, IpRoutingConfig, IpSecureTunnelingConfig>;
#else
using NetworkBuildConfig = std::variant<IpTunnelingConfig, IpRoutingConfig>;
#endif

constexpr Medium mediumForConfig(const IpTunnelingConfig&) noexcept
{
	return Medium::IP_Tunneling;
}

constexpr Medium mediumForConfig(const IpRoutingConfig&) noexcept
{
	return Medium::IP_Routing;
}

#if KNX_SECURE_ENABLED
constexpr Medium mediumForConfig(const IpSecureTunnelingConfig&) noexcept
{
	return Medium::IP_Secure_Tunneling;
}
#endif

inline Medium mediumForConfig(const NetworkBuildConfig& config) noexcept
{
	return std::visit([](const auto& concreteConfig) constexpr noexcept {
		return mediumForConfig(concreteConfig);
	}, config);
}

constexpr bool isNetworkMedium(Medium medium) noexcept
{
	switch (medium) {
		case Medium::IP_Tunneling:
		case Medium::IP_Routing:
#if KNX_SECURE_ENABLED
		case Medium::IP_Secure_Tunneling:
#endif
			return true;
		case Medium::TP1:
			return false;
	}

	return false;
}

} // namespace knx::product