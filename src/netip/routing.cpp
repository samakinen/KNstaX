// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/routing.hpp"

#include "knx/netip/header_codec.hpp"
#include "knx/util/bit_ops.hpp"
#include "knx/util/result.hpp"

#include <algorithm>
#include <limits>

namespace knx {
namespace netip {

namespace bits = knx::util;

static constexpr size_t KNXNETIP_HEADER_SIZE = KnxNetIpCodec::kHeaderLen;

// Both flow-control bodies are self-delimiting: the first octet repeats their
// own length, so a receiver can skip one it does not understand.
static constexpr uint8_t kLostMessageInfoLength = 4;
static constexpr uint8_t kBusyInfoLength = 6;

util::Result<void> RoutingCodec::decodeHeader(std::span<const uint8_t> data, KnxNetIpHeader& header)
{
    return KnxNetIpCodec::decodeHeader(data, header);
}

util::Result<size_t> RoutingCodec::encodeCemiCarrier(NetIpServiceType serviceType,
                                                     std::span<const uint8_t> cemi,
                                                     std::span<uint8_t> out)
{
    const size_t cemiLen = cemi.size();

    if (cemiLen > (std::numeric_limits<uint16_t>::max() - KNXNETIP_HEADER_SIZE)) return util::ErrorCode::InvalidFrameSize;

    const uint16_t totalLength = static_cast<uint16_t>(KNXNETIP_HEADER_SIZE + cemiLen);
    if (out.size() < totalLength) return util::ErrorCode::BufferTooSmall;

    auto headerResult = KnxNetIpCodec::encodeHeader(serviceType,
                                                    cemiLen,
                                                    std::span<uint8_t, KNXNETIP_HEADER_SIZE>(out.data(), KNXNETIP_HEADER_SIZE));
    if (headerResult.isError()) return headerResult.error();
    std::copy(cemi.begin(), cemi.end(), out.begin() + KNXNETIP_HEADER_SIZE);

    return static_cast<size_t>(totalLength);
}

util::Result<size_t> RoutingCodec::encodeRoutingIndication(std::span<const uint8_t> cemi, std::span<uint8_t> out)
{
    return encodeCemiCarrier(ST_ROUTING_INDICATION, cemi, out);
}

util::Result<std::span<const uint8_t>> RoutingCodec::decodeRoutingIndication(std::span<const uint8_t> data)
{
    return KnxNetIpCodec::payloadSpan(data, ST_ROUTING_INDICATION);
}

util::Result<size_t> RoutingCodec::encodeRoutingSystemBroadcast(std::span<const uint8_t> cemi, std::span<uint8_t> out)
{
    return encodeCemiCarrier(ST_ROUTING_SYSTEM_BROADCAST, cemi, out);
}

util::Result<std::span<const uint8_t>> RoutingCodec::decodeRoutingSystemBroadcast(std::span<const uint8_t> data)
{
    return KnxNetIpCodec::payloadSpan(data, ST_ROUTING_SYSTEM_BROADCAST);
}

util::Result<void> RoutingCodec::encodeRoutingLostMessage(const RoutingLostMessageInfo& info,
                                                          std::span<uint8_t, ROUTING_LOST_MESSAGE_FRAME_LEN> out)
{
    auto headerResult = KnxNetIpCodec::encodeHeader(ST_ROUTING_LOST_MESSAGE,
                                                    kLostMessageInfoLength,
                                                    std::span<uint8_t, KNXNETIP_HEADER_SIZE>(out.data(), KNXNETIP_HEADER_SIZE));
    if (headerResult.isError()) return headerResult.error();

    out[KNXNETIP_HEADER_SIZE + 0] = kLostMessageInfoLength;
    out[KNXNETIP_HEADER_SIZE + 1] = info.deviceState;
    out[KNXNETIP_HEADER_SIZE + 2] = bits::getHighByte(info.lostMessages);
    out[KNXNETIP_HEADER_SIZE + 3] = bits::getLowByte(info.lostMessages);
    return util::Result<void>::ok();
}

util::Result<void> RoutingCodec::decodeRoutingLostMessage(std::span<const uint8_t> data, RoutingLostMessageInfo& info)
{
    KnxNetIpHeader header;
    auto headerResult = decodeHeader(data, header);
    if (headerResult.isError()) return headerResult;
    if (header.serviceType != ST_ROUTING_LOST_MESSAGE) return util::ErrorCode::DecodeFailed;
    if (header.totalLength != ROUTING_LOST_MESSAGE_FRAME_LEN) return util::ErrorCode::DecodeFailed;
    if (data.size() < ROUTING_LOST_MESSAGE_FRAME_LEN) return util::ErrorCode::InvalidFrameSize;
    if (data[KNXNETIP_HEADER_SIZE] != kLostMessageInfoLength) return util::ErrorCode::DecodeFailed;

    info.deviceState = data[KNXNETIP_HEADER_SIZE + 1];
    info.lostMessages = bits::makeWord(data[KNXNETIP_HEADER_SIZE + 2], data[KNXNETIP_HEADER_SIZE + 3]);
    return util::Result<void>::ok();
}

util::Result<void> RoutingCodec::encodeRoutingBusy(const RoutingBusyInfo& info,
                                                   std::span<uint8_t, ROUTING_BUSY_FRAME_LEN> out)
{
    auto headerResult = KnxNetIpCodec::encodeHeader(ST_ROUTING_BUSY,
                                                    kBusyInfoLength,
                                                    std::span<uint8_t, KNXNETIP_HEADER_SIZE>(out.data(), KNXNETIP_HEADER_SIZE));
    if (headerResult.isError()) return headerResult.error();

    out[KNXNETIP_HEADER_SIZE + 0] = kBusyInfoLength;
    out[KNXNETIP_HEADER_SIZE + 1] = info.deviceState;
    out[KNXNETIP_HEADER_SIZE + 2] = bits::getHighByte(info.waitTimeMs);
    out[KNXNETIP_HEADER_SIZE + 3] = bits::getLowByte(info.waitTimeMs);
    out[KNXNETIP_HEADER_SIZE + 4] = bits::getHighByte(info.controlField);
    out[KNXNETIP_HEADER_SIZE + 5] = bits::getLowByte(info.controlField);
    return util::Result<void>::ok();
}

util::Result<void> RoutingCodec::decodeRoutingBusy(std::span<const uint8_t> data, RoutingBusyInfo& info)
{
    KnxNetIpHeader header;
    auto headerResult = decodeHeader(data, header);
    if (headerResult.isError()) return headerResult;
    if (header.serviceType != ST_ROUTING_BUSY) return util::ErrorCode::DecodeFailed;
    if (header.totalLength != ROUTING_BUSY_FRAME_LEN) return util::ErrorCode::DecodeFailed;
    if (data.size() < ROUTING_BUSY_FRAME_LEN) return util::ErrorCode::InvalidFrameSize;
    if (data[KNXNETIP_HEADER_SIZE] != kBusyInfoLength) return util::ErrorCode::DecodeFailed;

    info.deviceState = data[KNXNETIP_HEADER_SIZE + 1];
    info.waitTimeMs = bits::makeWord(data[KNXNETIP_HEADER_SIZE + 2], data[KNXNETIP_HEADER_SIZE + 3]);
    info.controlField = bits::makeWord(data[KNXNETIP_HEADER_SIZE + 4], data[KNXNETIP_HEADER_SIZE + 5]);
    return util::Result<void>::ok();
}

} // namespace netip
} // namespace knx
