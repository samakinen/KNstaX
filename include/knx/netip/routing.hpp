// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "knx/netip/header_codec.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

namespace knx {
namespace netip {

class RoutingCodec {
public:
    static constexpr uint8_t KNXNETIP_HEADER_LEN = KnxNetIpCodec::kHeaderLen;
    static constexpr uint8_t KNXNETIP_VERSION = KnxNetIpCodec::kVersion;
    static constexpr size_t ROUTING_LOST_MESSAGE_FRAME_LEN = KNXNETIP_HEADER_LEN + 2;

    static constexpr NetIpServiceType ST_ROUTING_INDICATION = NetIpServiceType(0x0530);
    static constexpr NetIpServiceType ST_ROUTING_LOST_MESSAGE = NetIpServiceType(0x0531);

    static util::Result<void> decodeHeader(std::span<const uint8_t> data, KnxNetIpHeader& header);

    static util::Result<size_t> encodeRoutingIndication(std::span<const uint8_t> cemi, std::span<uint8_t> out);

    static util::Result<std::span<const uint8_t>> decodeRoutingIndication(std::span<const uint8_t> data);

    static util::Result<void> encodeRoutingLostMessage(uint16_t lostCount,
                                                       std::span<uint8_t, ROUTING_LOST_MESSAGE_FRAME_LEN> out);

    // Parses ROUTING_LOST_MESSAGE and returns the lost-message counter.
    static util::Result<void> decodeRoutingLostMessage(std::span<const uint8_t> data, uint16_t& lostCount);
};

} // namespace netip
} // namespace knx
