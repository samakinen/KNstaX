// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "knx/netip/header_codec.hpp"
#include "knx/types.hpp"
#include "knx/util/result.hpp"

namespace knx {
namespace netip {

/// Flow-control payload of ROUTING_BUSY. Carries the sender's DeviceState (the
/// same octet as PID_KNXNETIP_DEVICE_STATE) ahead of its own fields.
struct RoutingBusyInfo {
    uint8_t deviceState{0};
    /// How long the receiver must stop sending, in milliseconds (03/08/05 §4.3).
    uint16_t waitTimeMs{0};
    /// 0000h means "every device shall act on this"; anything else addresses a
    /// subset, and a device outside that subset ignores the frame.
    uint16_t controlField{0};
};

struct RoutingLostMessageInfo {
    uint8_t deviceState{0};
    uint16_t lostMessages{0};
};

class RoutingCodec {
public:
    static constexpr uint8_t KNXNETIP_HEADER_LEN = KnxNetIpCodec::kHeaderLen;
    static constexpr uint8_t KNXNETIP_VERSION = KnxNetIpCodec::kVersion;

    /// LostMessageInfo is four octets (03/08/05 §5.3), BusyInfo six (§5.4).
    static constexpr size_t ROUTING_LOST_MESSAGE_FRAME_LEN = KNXNETIP_HEADER_LEN + 4;
    static constexpr size_t ROUTING_BUSY_FRAME_LEN = KNXNETIP_HEADER_LEN + 6;

    static constexpr NetIpServiceType ST_ROUTING_INDICATION = NetIpServiceType(0x0530);
    static constexpr NetIpServiceType ST_ROUTING_LOST_MESSAGE = NetIpServiceType(0x0531);
    static constexpr NetIpServiceType ST_ROUTING_BUSY = NetIpServiceType(0x0532);
    /// AN183 / 03/02/06 §4.1.3. Carries the system-broadcast domain on IP, which
    /// has no control-field bit of its own once the frame has become a datagram.
    static constexpr NetIpServiceType ST_ROUTING_SYSTEM_BROADCAST = NetIpServiceType(0x0533);

    static util::Result<void> decodeHeader(std::span<const uint8_t> data, KnxNetIpHeader& header);

    static util::Result<size_t> encodeRoutingIndication(std::span<const uint8_t> cemi, std::span<uint8_t> out);

    static util::Result<std::span<const uint8_t>> decodeRoutingIndication(std::span<const uint8_t> data);

    /// Same body as a routing indication; only the service type differs. Which
    /// domain a frame belongs to is the caller's decision, not this codec's.
    static util::Result<size_t> encodeRoutingSystemBroadcast(std::span<const uint8_t> cemi, std::span<uint8_t> out);

    static util::Result<std::span<const uint8_t>> decodeRoutingSystemBroadcast(std::span<const uint8_t> data);

    static util::Result<void> encodeRoutingLostMessage(const RoutingLostMessageInfo& info,
                                                       std::span<uint8_t, ROUTING_LOST_MESSAGE_FRAME_LEN> out);

    static util::Result<void> decodeRoutingLostMessage(std::span<const uint8_t> data, RoutingLostMessageInfo& info);

    static util::Result<void> encodeRoutingBusy(const RoutingBusyInfo& info,
                                                std::span<uint8_t, ROUTING_BUSY_FRAME_LEN> out);

    static util::Result<void> decodeRoutingBusy(std::span<const uint8_t> data, RoutingBusyInfo& info);

private:
    static util::Result<size_t> encodeCemiCarrier(NetIpServiceType serviceType,
                                                  std::span<const uint8_t> cemi,
                                                  std::span<uint8_t> out);
};

} // namespace netip
} // namespace knx
