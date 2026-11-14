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

/// Status codes of CONNECT_RESPONSE / TUNNELLING_ACK used by the tunnelling
/// client and server (03/08/02 §7.3, §7.8.2; 03/08/04 §5.4.3).
inline constexpr uint8_t kStatusNoError = 0x00;
inline constexpr uint8_t kStatusSequenceNumber = 0x04;
inline constexpr uint8_t kStatusConnectionId = 0x21;
inline constexpr uint8_t kStatusConnectionType = 0x22;
inline constexpr uint8_t kStatusConnectionOption = 0x23;
inline constexpr uint8_t kStatusNoMoreConnections = 0x24;
inline constexpr uint8_t kStatusTunnellingLayer = 0x29;
inline constexpr uint8_t kStatusNoTunnellingAddress = 0x2D;
inline constexpr uint8_t kStatusConnectionInUse = 0x2E;

/// CONNECT_REQUEST (03/08/02 §7.8.1, CRI per 03/08/04 §5.4.3).
struct ConnectRequest {
    HpaiEndpoint controlEndpoint{};
    HpaiEndpoint dataEndpoint{};
    uint8_t connectionType{0};
    uint8_t knxLayer{0};
    /// Extended CRI (length 6): the Individual Address the client asks for.
    bool hasRequestedAddress{false};
    uint16_t requestedAddress{0};
};

/// CONNECT_RESPONSE (03/08/02 §7.8.2, Figure 49): channel ID and status, and
/// on success the server's data endpoint HPAI and the CRD. For a tunnelling
/// connection the CRD carries the Individual Address assigned to it
/// (03/08/04 §5.4.4).
struct ConnectResponse {
    uint8_t channelId{0};
    uint8_t status{0};
    HpaiEndpoint dataEndpoint{};
    uint8_t connectionType{0};
    uint16_t tunnelAddress{0};
};

/// CONNECTIONSTATE_REQUEST / DISCONNECT_REQUEST: channel ID and the sender's
/// control endpoint.
struct ChannelEndpointRequest {
    uint8_t channelId{0};
    HpaiEndpoint controlEndpoint{};
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

    /// CONNECT_RESPONSE. On success the body is channel ID, status, the data
    /// endpoint HPAI and the CRD (03/08/02 §7.8.2, Figure 49); on failure the
    /// server may send just channel ID and status.
    static inline util::Result<void> encodeConnectResponse(PacketWriter& writer,
                                                           uint8_t channelId,
                                                           uint8_t status,
                                                           const HpaiEndpoint& dataEndpoint,
                                                           uint16_t tunnelAddress)
    {
        auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
        if (resetResult.isError()) return resetResult.error();

        const std::array<uint8_t, 2> prefix{channelId, status};
        auto prefixResult = writer.write(prefix);
        if (prefixResult.isError()) return prefixResult.error();

        if (status == kStatusNoError) {
            auto hpaiResult = appendHpai(writer, dataEndpoint);
            if (hpaiResult.isError()) return hpaiResult.error();
            const std::array<uint8_t, 4> crd{
                kCriLength,
                kConnectionTypeTunnel,
                static_cast<uint8_t>(tunnelAddress >> 8),
                static_cast<uint8_t>(tunnelAddress & 0xFF),
            };
            auto crdResult = writer.write(crd);
            if (crdResult.isError()) return crdResult.error();
        }
        return encodePacketHeader(writer.span(), kServiceConnectionResponse);
    }

    /// CONNECT_RESPONSE. A successful response must carry the data endpoint
    /// HPAI and a tunnelling CRD; one that does not is malformed, since the
    /// client could not know where to send data or which address it has.
    static inline util::Result<ConnectResponse> decodeConnectResponse(std::span<const uint8_t> data)
    {
        auto channelStatus = decodeChannelStatusResponse(data, kServiceConnectionResponse);
        if (channelStatus.isError()) return channelStatus.error();

        ConnectResponse response{};
        response.channelId = channelStatus.value().channelId;
        response.status = channelStatus.value().status;
        if (response.status != kStatusNoError) {
            return response;
        }

        KnxNetIpHeader header;
        (void)KnxNetIpCodec::decodeHeader(data, header);
        const auto frame = data.first(header.totalLength);
        size_t position = 8;
        auto hpai = decodeHpai(frame, position);
        if (hpai.isError()) return hpai.error();
        response.dataEndpoint = hpai.value();

        // CRD: length, connection type, then type-specific data. For a
        // tunnelling connection that is the 2-octet Individual Address.
        if (position + 2 > frame.size()) return util::ErrorCode::InvalidFrameSize;
        const uint8_t crdLength = frame[position];
        if (crdLength < 2 || position + crdLength > frame.size()) return util::ErrorCode::InvalidFrameSize;
        response.connectionType = frame[position + 1];
        if (response.connectionType == kConnectionTypeTunnel) {
            if (crdLength != kCriLength) return util::ErrorCode::DecodeFailed;
            response.tunnelAddress = static_cast<uint16_t>((static_cast<uint16_t>(frame[position + 2]) << 8)
                                                           | frame[position + 3]);
        }
        return response;
    }

    /// CONNECT_REQUEST: control HPAI, data HPAI, CRI (basic, length 4, or
    /// extended, length 6, with a requested Individual Address).
    static inline util::Result<ConnectRequest> decodeConnectRequest(std::span<const uint8_t> data)
    {
        KnxNetIpHeader header;
        auto headerResult = KnxNetIpCodec::decodeHeader(data, header);
        if (headerResult.isError()) return headerResult.error();
        if (header.serviceType != kServiceConnectionRequest) return util::ErrorCode::DecodeFailed;
        if (header.totalLength > data.size()) return util::ErrorCode::InvalidFrameSize;
        const auto frame = data.first(header.totalLength);

        ConnectRequest request{};
        size_t position = KnxNetIpCodec::kHeaderLen;
        auto control = decodeHpai(frame, position);
        if (control.isError()) return control.error();
        auto dataHpai = decodeHpai(frame, position);
        if (dataHpai.isError()) return dataHpai.error();
        request.controlEndpoint = control.value();
        request.dataEndpoint = dataHpai.value();

        if (position + 2 > frame.size()) return util::ErrorCode::InvalidFrameSize;
        const uint8_t criLength = frame[position];
        if (criLength < 2 || position + criLength > frame.size()) return util::ErrorCode::InvalidFrameSize;
        request.connectionType = frame[position + 1];
        if (criLength >= 3) request.knxLayer = frame[position + 2];
        if (criLength == 6) {
            request.hasRequestedAddress = true;
            request.requestedAddress = static_cast<uint16_t>((static_cast<uint16_t>(frame[position + 4]) << 8)
                                                             | frame[position + 5]);
        }
        return request;
    }

    /// CONNECTIONSTATE_REQUEST / DISCONNECT_REQUEST: channel ID, reserved,
    /// control endpoint HPAI.
    static inline util::Result<ChannelEndpointRequest> decodeChannelEndpointRequest(std::span<const uint8_t> data,
                                                                                    NetIpServiceType expectedServiceType)
    {
        KnxNetIpHeader header;
        auto headerResult = KnxNetIpCodec::decodeHeader(data, header);
        if (headerResult.isError()) return headerResult.error();
        if (header.serviceType != expectedServiceType) return util::ErrorCode::DecodeFailed;
        if (header.totalLength > data.size() || header.totalLength < 16) return util::ErrorCode::InvalidFrameSize;
        const auto frame = data.first(header.totalLength);

        ChannelEndpointRequest request{};
        request.channelId = frame[6];
        size_t position = 8;
        auto hpai = decodeHpai(frame, position);
        if (hpai.isError()) return hpai.error();
        request.controlEndpoint = hpai.value();
        return request;
    }

    /// The channel-status responses (CONNECTIONSTATE_RESPONSE,
    /// DISCONNECT_RESPONSE, a failed CONNECT_RESPONSE): channel ID + status.
    static inline util::Result<void> encodeChannelStatusResponse(PacketWriter& writer,
                                                                 NetIpServiceType serviceType,
                                                                 uint8_t channelId,
                                                                 uint8_t status)
    {
        auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
        if (resetResult.isError()) return resetResult.error();
        const std::array<uint8_t, 2> body{channelId, status};
        auto bodyResult = writer.write(body);
        if (bodyResult.isError()) return bodyResult.error();
        return encodePacketHeader(writer.span(), serviceType);
    }

    /// True for the "Route Back" HPAI 0.0.0.0:0 (03/08/02 §8.6.2.2): the
    /// reply goes to the source address and port of the received packet.
    static inline bool isRouteBack(const HpaiEndpoint& endpoint)
    {
        return endpoint.address.isZero() && endpoint.port == 0;
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

public:
    /// HPAI is the one structure every control service shares, so the discovery
    /// and self-description responders build on these rather than repeating the
    /// eight-octet layout each time.
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

    /// Read an HPAI out of `data` at `position`, advancing it past the block.
    ///
    /// A TCP HPAI is decoded, not rejected: whether a TCP return address is
    /// acceptable depends on the service that carries it (discovery must
    /// discard it, connection management must not), so that judgement belongs
    /// to the caller rather than here.
    static inline util::Result<HpaiEndpoint> decodeHpai(std::span<const uint8_t> data, size_t& position)
    {
        if ((position + kHpaiLength) > data.size()) return util::ErrorCode::InvalidFrameSize;
        if (data[position] != kHpaiLength) return util::ErrorCode::DecodeFailed;

        const uint8_t protocol = data[position + 1];
        if (protocol != static_cast<uint8_t>(HpaiProtocol::Udp) &&
            protocol != static_cast<uint8_t>(HpaiProtocol::Tcp)) {
            return util::ErrorCode::DecodeFailed;
        }

        HpaiEndpoint endpoint{};
        endpoint.protocol = static_cast<HpaiProtocol>(protocol);
        endpoint.address = IpAddress::fromOctets(data[position + 2],
                                                 data[position + 3],
                                                 data[position + 4],
                                                 data[position + 5]);
        endpoint.port = static_cast<uint16_t>((static_cast<uint16_t>(data[position + 6]) << 8) |
                                              static_cast<uint16_t>(data[position + 7]));
        position += kHpaiLength;
        return endpoint;
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