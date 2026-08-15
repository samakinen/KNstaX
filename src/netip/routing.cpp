// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/routing.hpp"

#include "knx/netip/header_codec.hpp"
#include "knx/util/bit_ops.hpp"
#include "knx/util/result.hpp"

#include <limits>

namespace knx {
namespace netip {

namespace bits = knx::util;

static constexpr size_t KNXNETIP_HEADER_SIZE = KnxNetIpCodec::kHeaderLen;

util::Result<void> RoutingCodec::decodeHeader(std::span<const uint8_t> data, KnxNetIpHeader& header)
{
    return KnxNetIpCodec::decodeHeader(data, header);
}

util::Result<size_t> RoutingCodec::encodeRoutingIndication(std::span<const uint8_t> cemi, std::span<uint8_t> out)
{
    const size_t cemiLen = cemi.size();

    if (cemiLen > (std::numeric_limits<uint16_t>::max() - KNXNETIP_HEADER_SIZE)) return util::ErrorCode::InvalidFrameSize;

    const uint16_t totalLength = static_cast<uint16_t>(KNXNETIP_HEADER_SIZE + cemiLen);
    if (out.size() < totalLength) return util::ErrorCode::BufferTooSmall;

    auto headerResult = KnxNetIpCodec::encodeHeader(ST_ROUTING_INDICATION,
                                                    cemiLen,
                                                    std::span<uint8_t, KNXNETIP_HEADER_SIZE>(out.data(), KNXNETIP_HEADER_SIZE));
    if (headerResult.isError()) return headerResult.error();
    std::copy(cemi.begin(), cemi.end(), out.begin() + KNXNETIP_HEADER_SIZE);

    return static_cast<size_t>(totalLength);
}

util::Result<std::span<const uint8_t>> RoutingCodec::decodeRoutingIndication(std::span<const uint8_t> data)
{
    return KnxNetIpCodec::payloadSpan(data, ST_ROUTING_INDICATION);
}

util::Result<void> RoutingCodec::encodeRoutingLostMessage(uint16_t lostCount,
                                                          std::span<uint8_t, ROUTING_LOST_MESSAGE_FRAME_LEN> out)
{
    auto headerResult = KnxNetIpCodec::encodeHeader(ST_ROUTING_LOST_MESSAGE,
                                                    2,
                                                    std::span<uint8_t, KNXNETIP_HEADER_SIZE>(out.data(), KNXNETIP_HEADER_SIZE));
    if (headerResult.isError()) return headerResult.error();
    out[KNXNETIP_HEADER_SIZE] = bits::getHighByte(lostCount);
    out[KNXNETIP_HEADER_SIZE + 1] = bits::getLowByte(lostCount);
    return util::Result<void>::ok();
}

util::Result<void> RoutingCodec::decodeRoutingLostMessage(std::span<const uint8_t> data, uint16_t& lostCount)
{
    KnxNetIpHeader header;
    auto headerResult = decodeHeader(data, header);
    if (headerResult.isError()) return headerResult;
    if (header.serviceType != ST_ROUTING_LOST_MESSAGE) return util::ErrorCode::DecodeFailed;

    // ROUTING_LOST_MESSAGE payload is 2 bytes (lost-message counter).
    if (header.totalLength != KNXNETIP_HEADER_SIZE + 2) return util::ErrorCode::DecodeFailed;

    const size_t pos = KNXNETIP_HEADER_SIZE;
    if (pos + 2 > data.size()) return util::ErrorCode::InvalidFrameSize;

    lostCount = bits::makeWord(data[pos], data[pos + 1]);
    return util::Result<void>::ok();
}

} // namespace netip
} // namespace knx
