// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/config.hpp"

#include <cstddef>
#include <cstdint>

namespace knx {
namespace netip {
namespace config {

inline constexpr uint16_t kDefaultPort = knx::config::IP_PORT;
inline constexpr size_t kUdpBufferSize = knx::config::NETIP_UDP_BUFFER_SIZE;
inline constexpr size_t kDeviceManagementBufferSize = knx::config::NETIP_DEVICE_MANAGEMENT_BUFFER_SIZE;
inline constexpr size_t kTcpBufferSize = knx::config::NETIP_TCP_BUFFER_SIZE;
inline constexpr uint32_t kTunnelingKeepaliveIntervalMs = knx::config::NETIP_TUNNELING_KEEPALIVE_INTERVAL_MS;

} // namespace config
} // namespace netip
} // namespace knx