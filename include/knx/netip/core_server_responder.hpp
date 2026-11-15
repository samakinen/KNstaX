// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/config.hpp"
#include "knx/netip/core_discovery_codec.hpp"
#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/netip_config.hpp"
#include "knx/types.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

/**
 * @file core_server_responder.hpp
 * @brief Answers "who is on this network?" on behalf of a KNX IP device.
 *
 * Decides whether a received discovery or self-description request deserves an
 * answer and builds it. It owns no socket: `handle()` takes a datagram and
 * hands back the reply plus where to send it, which keeps the transport free to
 * be the routing socket the device already has open — discovery arrives on the
 * same multicast group and port, so a second listener would be a second bind on
 * 3671 and a coin toss over which one the kernel wakes.
 *
 * Search Request Parameters (03/08/02 §7.6.3.3) are honoured, including the
 * rule that decides how a device behaves when it does not understand one: an
 * unknown SRP with the M bit set means stay silent, without it means ignore the
 * parameter. Getting that backwards makes a device either invisible or noisy in
 * exactly the installations that use the feature.
 */
class CoreServerResponder {
public:
    /// Sampled fresh per request: programming mode and the current IP address
    /// both change while the device is running, and a cached description would
    /// answer ETS with the state at boot.
    using DescriptionProvider = util::InplaceFunction<CoreDeviceDescription(), ::knx::config::callbackBytes(64)>;

    static constexpr size_t kMaxServiceFamilies = 8;
    /// A search response is a control endpoint plus three DIBs; the largest is
    /// the 54-octet device information one. 256 leaves room for the extended
    /// response without ever coming near the MTU.
    static constexpr size_t kResponseCapacity = 256;

    struct Reply {
        IpAddress address{IpAddress(0)};
        NetIpPort port{NetIpPort::invalid()};
        std::span<const uint8_t> datagram{};
    };

    CoreServerResponder() = default;

    void setDescriptionProvider(DescriptionProvider provider) { provider_ = std::move(provider); }

    /// The families this device announces. Security (09h) may be included: it
    /// is filtered out of the plain SEARCH_RESPONSE and DESCRIPTION_RESPONSE
    /// automatically, where Table 3 does not allow it.
    util::Result<void> setServiceFamilies(std::span<const ServiceFamilySupport> families);

    /// The device's own control endpoint, as reported in a search response.
    void setControlEndpoint(IpAddress address, NetIpPort port) noexcept
    {
        controlAddress_ = address;
        controlPort_ = port;
    }

    /// True when `datagram` was a Core request this responder consumed. `reply`
    /// is filled only when an answer is due — a request that is well-formed but
    /// filtered out by an SRP is still consumed, just silently.
    bool handle(std::span<const uint8_t> datagram,
                IpAddress sourceAddress,
                uint16_t sourcePort,
                Reply& reply);

    /// Does this responder claim `serviceType`? Lets a transport route control
    /// datagrams without parsing them twice.
    static bool handlesServiceType(NetIpServiceType serviceType) noexcept;

private:
    struct SearchFilter {
        bool respond{true};
        std::span<const uint8_t> requestedDibs{};
    };

    SearchFilter evaluateSearchParameters(std::span<const uint8_t> datagram,
                                          size_t position,
                                          const CoreDeviceDescription& device) const;

    bool supportsService(uint8_t family, uint8_t minimumVersion) const noexcept;

    /// Families minus Security, for the responses Table 3 restricts.
    size_t publicFamilies(std::span<ServiceFamilySupport> out) const noexcept;

    [[nodiscard]] control_packet::HpaiEndpoint resolveReturnEndpoint(const control_packet::HpaiEndpoint& requested,
                                                                     IpAddress sourceAddress,
                                                                     uint16_t sourcePort) const noexcept;

    DescriptionProvider provider_{};
    std::array<ServiceFamilySupport, kMaxServiceFamilies> families_{};
    size_t familyCount_{0};

    IpAddress controlAddress_{IpAddress(0)};
    NetIpPort controlPort_{NetIpPort(config::kDefaultPort)};

    DatagramBuffer<kResponseCapacity> responseBuffer_{};
};

} // namespace netip
} // namespace knx
