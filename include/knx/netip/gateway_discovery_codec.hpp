// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/control_packet_codec.hpp"
#include "knx/netip/gateway_info.hpp"
#include "knx/netip/header_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace knx {
namespace netip {

class GatewayDiscoveryCodec {
public:
    static constexpr uint8_t kDeviceInfoDibType = 0x01;
    static constexpr uint8_t kSupportedServiceFamiliesDibType = 0x02;
    static constexpr uint8_t kSecuredServiceFamiliesDibType = 0x06; // DIB_SECURED_SERVICE_FAMILIES (ISO 22510 §5.7.2.5)

    static inline util::Result<void> parseSearchResponse(std::span<const uint8_t> data, GatewayInfo& info)
    {
        return parseGatewayResponse(data, control_packet::kServiceSearchResponse, info);
    }

    static inline util::Result<void> parseDescriptionResponse(std::span<const uint8_t> data, GatewayInfo& info)
    {
        return parseGatewayResponse(data, control_packet::kServiceDescriptionResponse, info);
    }

private:
    static inline util::Result<void> parseGatewayResponse(std::span<const uint8_t> data,
                                                          NetIpServiceType expectedServiceType,
                                                          GatewayInfo& info)
    {
        KnxNetIpHeader header;
        auto headerResult = KnxNetIpCodec::decodeHeader(data, header);
        if (headerResult.isError()) return headerResult.error();
        if (header.serviceType != expectedServiceType) return util::ErrorCode::DecodeFailed;

        data = data.first(header.totalLength);

        size_t position = KnxNetIpCodec::kHeaderLen;
        auto endpointResult = parseControlEndpoint(data, position, info);
        if (endpointResult.isError()) return endpointResult.error();

        while (position < data.size()) {
            auto dibResult = parseDib(data, position, info);
            if (dibResult.isError()) break;
        }

        return util::Result<void>::ok();
    }

    static inline util::Result<void> parseControlEndpoint(std::span<const uint8_t> data,
                                                          size_t& position,
                                                          GatewayInfo& info)
    {
        if ((position + 2) > data.size()) return util::ErrorCode::InvalidFrameSize;

        const uint8_t hpaiLen = data[position];
        if (hpaiLen != control_packet::Codec::kHpaiLength) return util::ErrorCode::DecodeFailed;
        if ((position + hpaiLen) > data.size()) return util::ErrorCode::InvalidFrameSize;
        if (data[position + 1] != static_cast<uint8_t>(control_packet::HpaiProtocol::Udp)) {
            return util::ErrorCode::DecodeFailed;
        }

        info.ipAddress = IpAddress::fromOctets(data[position + 2],
                                               data[position + 3],
                                               data[position + 4],
                                               data[position + 5]);
        info.port = NetIpPort(static_cast<uint16_t>((static_cast<uint16_t>(data[position + 6]) << 8) |
                                                    data[position + 7]));
        position += hpaiLen;
        return util::Result<void>::ok();
    }

    static inline util::Result<void> parseDib(std::span<const uint8_t> data,
                                              size_t& position,
                                              GatewayInfo& info)
    {
        if ((position + 2) > data.size()) return util::ErrorCode::InvalidFrameSize;

        const uint8_t dibLen = data[position];
        const uint8_t dibType = data[position + 1];
        if (dibLen < 2 || (position + dibLen) > data.size()) return util::ErrorCode::InvalidFrameSize;

        const auto dib = data.subspan(position, dibLen);
        if (dibType == kDeviceInfoDibType) {
            parseDeviceInfoDib(dib, info);
        } else if (dibType == kSupportedServiceFamiliesDibType) {
            info.supportedServices.assign(dib.begin(), dib.end());
        } else if (dibType == kSecuredServiceFamiliesDibType) {
            info.securedServiceFamilies.assign(dib.begin(), dib.end());
        }

        position += dibLen;
        return util::Result<void>::ok();
    }

    static inline void parseDeviceInfoDib(std::span<const uint8_t> dib, GatewayInfo& info)
    {
        if (dib.size() >= 24) {
            std::memcpy(info.macAddress, dib.data() + 18, 6);
        }

        if (dib.size() >= 54) {
            char friendlyName[31] = {0};
            std::memcpy(friendlyName, dib.data() + 24, 30);
            info.friendlyName = friendlyName;
        }

        info.deviceDIB.assign(dib.begin(), dib.end());
    }
};

} // namespace netip
} // namespace knx