// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/datagram_scratch.hpp"
#include "knx/netip/header_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {
namespace control_packet {

constexpr NetIpServiceType kServiceSearchRequest = NetIpServiceType(0x0201);
constexpr NetIpServiceType kServiceSearchResponse = NetIpServiceType(0x0202);
constexpr NetIpServiceType kServiceDescriptionRequest = NetIpServiceType(0x0203);
constexpr NetIpServiceType kServiceDescriptionResponse = NetIpServiceType(0x0204);
constexpr NetIpServiceType kServiceConnectionRequest = NetIpServiceType(0x0205);
constexpr NetIpServiceType kServiceConnectionResponse = NetIpServiceType(0x0206);
constexpr NetIpServiceType kServiceConnectionStateRequest = NetIpServiceType(0x0207);
constexpr NetIpServiceType kServiceConnectionStateResponse = NetIpServiceType(0x0208);
constexpr NetIpServiceType kServiceDisconnectRequest = NetIpServiceType(0x0209);
constexpr NetIpServiceType kServiceDisconnectResponse = NetIpServiceType(0x020A);
constexpr NetIpServiceType kServiceTunnelingRequest = NetIpServiceType(0x0420);
constexpr NetIpServiceType kServiceTunnelingAck = NetIpServiceType(0x0421);

enum class HpaiProtocol : uint8_t {
    Udp = 0x01,
    Tcp = 0x02,
};

struct HpaiEndpoint {
    HpaiProtocol protocol{HpaiProtocol::Udp};
    IpAddress address{};
    uint16_t port{0};
};

struct ChannelStatus {
    uint8_t channelId{0};
    uint8_t status{0};
};

struct TunnelingAck {
    uint8_t channelId{0};
    uint8_t sequence{0};
    uint8_t status{0};
};

struct TunnelingRequest {
    uint8_t channelId{0};
    uint8_t sequence{0};
    std::span<const uint8_t> cemi{};
};

class Codec {
public:
    static constexpr uint8_t kHpaiLength = 0x08;
    static constexpr uint8_t kCriLength = 0x04;
    static constexpr uint8_t kConnectionTypeTunnel = 0x04;
    static constexpr uint8_t kTunnelLayerTp1 = 0x02;
    static constexpr uint8_t kTunnelingHeaderLength = 0x04;

    static inline util::Result<void> encodeSearchRequest(PacketWriter& writer, const HpaiEndpoint& controlEndpoint)
    {
        auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
        if (resetResult.isError()) return resetResult.error();
        auto hpaiResult = appendHpai(writer, controlEndpoint);
        if (hpaiResult.isError()) return hpaiResult.error();
        return encodePacketHeader(writer.span(), kServiceSearchRequest);
    }

    static inline util::Result<void> encodeDescriptionRequest(PacketWriter& writer, const HpaiEndpoint& controlEndpoint)
    {
        auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
        if (resetResult.isError()) return resetResult.error();
        auto hpaiResult = appendHpai(writer, controlEndpoint);
        if (hpaiResult.isError()) return hpaiResult.error();
        return encodePacketHeader(writer.span(), kServiceDescriptionRequest);
    }

    static inline util::Result<void> encodeConnectionRequest(PacketWriter& writer,
                                                             const HpaiEndpoint& controlEndpoint,
                                                             const HpaiEndpoint& dataEndpoint,
                                                             uint8_t knxLayer = kTunnelLayerTp1)
    {
        auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
        if (resetResult.isError()) return resetResult.error();

        auto controlResult = appendHpai(writer, controlEndpoint);
        if (controlResult.isError()) return controlResult.error();

        auto dataResult = appendHpai(writer, dataEndpoint);
        if (dataResult.isError()) return dataResult.error();

        const std::array<uint8_t, 4> cri{
            kCriLength,
            kConnectionTypeTunnel,
            knxLayer,
            0x00,
        };
        auto criResult = writer.write(cri);
        if (criResult.isError()) return criResult.error();

        return encodePacketHeader(writer.span(), kServiceConnectionRequest);
    }

    static inline util::Result<void> encodeConnectionStateRequest(PacketWriter& writer,
                                                                  uint8_t channelId,
                                                                  const HpaiEndpoint& controlEndpoint)
    {
        return encodeChannelEndpointRequest(writer, kServiceConnectionStateRequest, channelId, controlEndpoint);
    }

    static inline util::Result<void> encodeDisconnectRequest(PacketWriter& writer,
                                                             uint8_t channelId,
                                                             const HpaiEndpoint& controlEndpoint)
    {
        return encodeChannelEndpointRequest(writer, kServiceDisconnectRequest, channelId, controlEndpoint);
    }

    static inline util::Result<void> encodeTunnelingRequest(PacketWriter& writer,
                                                            uint8_t channelId,
                                                            uint8_t sequence,
                                                            std::span<const uint8_t> cemi)
    {
        auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
        if (resetResult.isError()) return resetResult.error();

        const std::array<uint8_t, 4> tunnelingHeader{
            kTunnelingHeaderLength,
            channelId,
            sequence,
            0x00,
        };
        auto headerResult = writer.write(tunnelingHeader);
        if (headerResult.isError()) return headerResult.error();

        auto cemiResult = writer.write(cemi);
        if (cemiResult.isError()) return cemiResult.error();

        return encodePacketHeader(writer.span(), kServiceTunnelingRequest);
    }

    static inline util::Result<void> encodeTunnelingAck(PacketWriter& writer,
                                                        uint8_t channelId,
                                                        uint8_t sequence,
                                                        uint8_t status = 0x00)
    {
        auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
        if (resetResult.isError()) return resetResult.error();

        const std::array<uint8_t, 4> tunnelingAck{
            kTunnelingHeaderLength,
            channelId,
            sequence,
            status,
        };
        auto bodyResult = writer.write(tunnelingAck);
        if (bodyResult.isError()) return bodyResult.error();

        return encodePacketHeader(writer.span(), kServiceTunnelingAck);
    }

    static inline util::Result<ChannelStatus> decodeChannelStatusResponse(std::span<const uint8_t> data,
                                                                          NetIpServiceType expectedServiceType)
    {
        if (data.size() < 8) return util::ErrorCode::InvalidFrameSize;

        KnxNetIpHeader header;
        auto headerResult = KnxNetIpCodec::decodeHeader(data, header);
        if (headerResult.isError()) return headerResult.error();
        if (header.serviceType != expectedServiceType) return util::ErrorCode::DecodeFailed;
        if (header.totalLength < 8 || header.totalLength > data.size()) return util::ErrorCode::InvalidFrameSize;

        return ChannelStatus{data[6], data[7]};
    }

    static inline util::Result<TunnelingAck> decodeTunnelingAck(std::span<const uint8_t> data)
    {
        return decodeTunnelingControl<TunnelingAck>(data, kServiceTunnelingAck,
                                                    [](std::span<const uint8_t> frame) {
                                                        return TunnelingAck{frame[7], frame[8], frame[9]};
                                                    });
    }

    static inline util::Result<TunnelingRequest> decodeTunnelingRequest(std::span<const uint8_t> data)
    {
        return decodeTunnelingControl<TunnelingRequest>(data, kServiceTunnelingRequest,
                                                        [](std::span<const uint8_t> frame) {
                                                            return TunnelingRequest{frame[7], frame[8], frame.subspan(10)};
                                                        });
    }

private:
    template <typename T, typename Builder>
    static inline util::Result<T> decodeTunnelingControl(std::span<const uint8_t> data,
                                                         NetIpServiceType expectedServiceType,
                                                         Builder&& builder)
    {
        if (data.size() < 10) return util::ErrorCode::InvalidFrameSize;

        KnxNetIpHeader header;
        auto headerResult = KnxNetIpCodec::decodeHeader(data, header);
        if (headerResult.isError()) return headerResult.error();
        if (header.serviceType != expectedServiceType) return util::ErrorCode::DecodeFailed;
        if (header.totalLength < 10 || header.totalLength > data.size()) return util::ErrorCode::InvalidFrameSize;

        const auto frame = data.first(header.totalLength);
        if (frame[6] != kTunnelingHeaderLength) return util::ErrorCode::DecodeFailed;
        return builder(frame);
    }

    static inline util::Result<void> encodeChannelEndpointRequest(PacketWriter& writer,
                                                                  NetIpServiceType serviceType,
                                                                  uint8_t channelId,
                                                                  const HpaiEndpoint& controlEndpoint)
    {
        auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
        if (resetResult.isError()) return resetResult.error();

        const std::array<uint8_t, 2> prefix{channelId, 0x00};
        auto prefixResult = writer.write(prefix);
        if (prefixResult.isError()) return prefixResult.error();

        auto hpaiResult = appendHpai(writer, controlEndpoint);
        if (hpaiResult.isError()) return hpaiResult.error();

        return encodePacketHeader(writer.span(), serviceType);
    }

    static inline util::Result<void> appendHpai(PacketWriter& writer, const HpaiEndpoint& endpoint)
    {
        uint8_t a = 0;
        uint8_t b = 0;
        uint8_t c = 0;
        uint8_t d = 0;
        endpoint.address.toOctets(a, b, c, d);

        const std::array<uint8_t, 8> hpai{
            kHpaiLength,
            static_cast<uint8_t>(endpoint.protocol),
            a,
            b,
            c,
            d,
            static_cast<uint8_t>((endpoint.port >> 8) & 0xFF),
            static_cast<uint8_t>(endpoint.port & 0xFF),
        };
        return writer.write(hpai);
    }

    static inline util::Result<void> encodePacketHeader(std::span<uint8_t> packet, NetIpServiceType serviceType)
    {
        if (packet.size() < KnxNetIpCodec::kHeaderLen) return util::ErrorCode::InvalidFrameSize;

        auto headerResult = KnxNetIpCodec::encodeHeader(
            serviceType,
            packet.size() - KnxNetIpCodec::kHeaderLen,
            std::span<uint8_t, KnxNetIpCodec::kHeaderLen>(packet.data(), KnxNetIpCodec::kHeaderLen));
        if (headerResult.isError()) return headerResult.error();
        return util::Result<void>::ok();
    }
};

} // namespace control_packet
} // namespace netip
} // namespace knx