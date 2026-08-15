// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/header_codec.hpp"

#include <limits>

namespace knx {
namespace netip {

util::Result<void> KnxNetIpCodec::decodeHeader(std::span<const uint8_t> data, KnxNetIpHeader& header)
{
    if (data.size() < kHeaderLen) return util::ErrorCode::InvalidFrameSize;
    if (data[0] != kHeaderLen || data[1] != kVersion) return util::ErrorCode::DecodeFailed;

    header.serviceType = NetIpServiceType(static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) | data[3]));
    header.totalLength = static_cast<uint16_t>((static_cast<uint16_t>(data[4]) << 8) | data[5]);

    if (header.totalLength < kHeaderLen) return util::ErrorCode::DecodeFailed;
    if (static_cast<size_t>(header.totalLength) > data.size()) return util::ErrorCode::InvalidFrameSize;
    return util::Result<void>::ok();
}

util::Result<size_t> KnxNetIpCodec::encodeHeader(NetIpServiceType serviceType,
                                                 size_t payloadLength,
                                                 std::span<uint8_t, kHeaderLen> outHeader)
{
    if (payloadLength > (std::numeric_limits<uint16_t>::max() - kHeaderLen)) return util::ErrorCode::InvalidFrameSize;

    const uint16_t totalLength = static_cast<uint16_t>(kHeaderLen + payloadLength);
    outHeader[0] = kHeaderLen;
    outHeader[1] = kVersion;
    outHeader[2] = static_cast<uint8_t>((serviceType.value() >> 8) & 0xFF);
    outHeader[3] = static_cast<uint8_t>(serviceType.value() & 0xFF);
    outHeader[4] = static_cast<uint8_t>((totalLength >> 8) & 0xFF);
    outHeader[5] = static_cast<uint8_t>(totalLength & 0xFF);
    return totalLength;
}

util::Result<std::span<const uint8_t>> KnxNetIpCodec::payloadSpan(std::span<const uint8_t> data,
                                                                  NetIpServiceType expectedServiceType)
{
    KnxNetIpHeader header;
    auto headerResult = decodeHeader(data, header);
    if (headerResult.isError()) return headerResult.error();
    if (header.serviceType != expectedServiceType) return util::ErrorCode::DecodeFailed;

    return data.subspan(kHeaderLen, header.totalLength - kHeaderLen);
}

} // namespace netip
} // namespace knx