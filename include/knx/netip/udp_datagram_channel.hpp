// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/platform/network_interface.hpp"
#include "knx/platform/platform.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

struct UdpDatagramEndpoint {
    IpAddress addr{IpAddress(0)};
    NetIpPort port{NetIpPort::invalid()};
};

class UdpDatagramChannel {
public:
    static util::Result<bool> waitReadable(platform::UdpSocket& socket,
                                           int timeoutMs,
                                           platform::TimingPlatform* timingPlatform = nullptr);

    static util::Result<size_t> exchange(platform::UdpSocket& socket,
                                         const UdpDatagramEndpoint& remote,
                                         std::span<const uint8_t> request,
                                         std::span<uint8_t> responseBuffer,
                                         int timeoutMs,
                                         platform::TimingPlatform* timingPlatform = nullptr);
};

} // namespace netip
} // namespace knx