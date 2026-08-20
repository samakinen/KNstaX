// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/core_server_responder.hpp"

#include "knx/util/log.hpp"

#include <algorithm>

namespace knx {
namespace netip {

[[maybe_unused]] static const char* TAG = "KNX.CoreServer";

namespace {

// Search Request Parameter type codes (03/08/02 §7.6.3.3.2).
constexpr uint8_t kSrpMandatoryBit = 0x80;
constexpr uint8_t kSrpTypeMask = 0x7F;
constexpr uint8_t kSrpSelectByProgrammingMode = 0x01;
constexpr uint8_t kSrpSelectByMacAddress = 0x02;
constexpr uint8_t kSrpSelectByService = 0x03;
constexpr uint8_t kSrpRequestDibs = 0x04;

} // namespace

bool CoreServerResponder::handlesServiceType(NetIpServiceType serviceType) noexcept
{
    return serviceType == control_packet::kServiceSearchRequest ||
           serviceType == control_packet::kServiceDescriptionRequest ||
           serviceType == CoreDiscoveryCodec::kServiceSearchRequestExtended;
}

util::Result<void> CoreServerResponder::setServiceFamilies(std::span<const ServiceFamilySupport> families)
{
    if (families.size() > kMaxServiceFamilies) return util::ErrorCode::BufferTooSmall;
    std::copy(families.begin(), families.end(), families_.begin());
    familyCount_ = families.size();
    return util::Result<void>::ok();
}

bool CoreServerResponder::supportsService(uint8_t family, uint8_t minimumVersion) const noexcept
{
    for (size_t i = 0; i < familyCount_; ++i) {
        if (families_[i].family == family) {
            return families_[i].version >= minimumVersion;
        }
    }
    return false;
}

size_t CoreServerResponder::publicFamilies(std::span<ServiceFamilySupport> out) const noexcept
{
    size_t count = 0;
    for (size_t i = 0; i < familyCount_ && count < out.size(); ++i) {
        if (families_[i].family == service_family::kSecurity) continue;
        out[count++] = families_[i];
    }
    return count;
}

control_packet::HpaiEndpoint CoreServerResponder::resolveReturnEndpoint(
    const control_packet::HpaiEndpoint& requested,
    IpAddress sourceAddress,
    uint16_t sourcePort) const noexcept
{
    // A client behind NAT sends 0.0.0.0:0 and means "wherever this datagram
    // came from". Honouring that is what lets ETS discover a device across a
    // router; taking the HPAI literally would answer into a private address.
    control_packet::HpaiEndpoint endpoint = requested;
    if (endpoint.address.isZero()) endpoint.address = sourceAddress;
    if (endpoint.port == 0) endpoint.port = sourcePort;
    return endpoint;
}

CoreServerResponder::SearchFilter CoreServerResponder::evaluateSearchParameters(
    std::span<const uint8_t> datagram,
    size_t position,
    const CoreDeviceDescription& device) const
{
    SearchFilter filter{};

    while (position < datagram.size()) {
        const uint8_t structureLength = datagram[position];
        // A zero-length block would never terminate the walk, and a block that
        // runs past the datagram is malformed either way.
        if (structureLength < 2 || (position + structureLength) > datagram.size()) {
            filter.respond = false;
            return filter;
        }

        const uint8_t typeOctet = datagram[position + 1];
        const bool mandatory = (typeOctet & kSrpMandatoryBit) != 0;
        const uint8_t type = typeOctet & kSrpTypeMask;
        const auto body = datagram.subspan(position + 2, structureLength - 2u);

        switch (type) {
            case kSrpSelectByProgrammingMode:
                if (!device.programmingMode) {
                    filter.respond = false;
                    return filter;
                }
                break;

            case kSrpSelectByMacAddress:
                if (body.size() != device.macAddress.size() ||
                    !std::equal(body.begin(), body.end(), device.macAddress.begin())) {
                    filter.respond = false;
                    return filter;
                }
                break;

            case kSrpSelectByService:
                if (body.size() != 2 || !supportsService(body[0], body[1])) {
                    filter.respond = false;
                    return filter;
                }
                break;

            case kSrpRequestDibs:
                // Explicitly never a reason to stay silent (§7.6.3.3.6).
                filter.requestedDibs = body;
                break;

            default:
                // Type 00h is invalid and deliberately behaves like any other
                // unknown code, which is how a client probes this rule.
                if (mandatory) {
                    KNX_LOGD(TAG, "Unsupported mandatory SRP 0x%02X — not responding",
                             static_cast<unsigned>(type));
                    filter.respond = false;
                    return filter;
                }
                break;
        }

        position += structureLength;
    }

    return filter;
}

bool CoreServerResponder::handle(std::span<const uint8_t> datagram,
                                 IpAddress sourceAddress,
                                 uint16_t sourcePort,
                                 Reply& reply)
{
    KnxNetIpHeader header;
    if (KnxNetIpCodec::decodeHeader(datagram, header).isError()) return false;
    if (!handlesServiceType(header.serviceType)) return false;

    if (!provider_) {
        KNX_LOGW(TAG, "Discovery request with no device description bound");
        return true;
    }

    const auto frame = datagram.first(header.totalLength);
    size_t position = KnxNetIpCodec::kHeaderLen;

    auto endpointResult = control_packet::Codec::decodeHpai(frame, position);
    if (endpointResult.isError()) return true;

    // "If this information contains a TCP address, the frame shall be
    // discarded" — discovery is defined as connectionless (§7.6.1).
    if (endpointResult.value().protocol != control_packet::HpaiProtocol::Udp) {
        return true;
    }

    const CoreDeviceDescription device = provider_();

    std::array<ServiceFamilySupport, kMaxServiceFamilies> restricted{};
    const size_t restrictedCount = publicFamilies(restricted);

    PacketWriter writer(responseBuffer_.span());
    util::Result<void> encodeResult = util::ErrorCode::OperationNotSupported;

    if (header.serviceType == control_packet::kServiceDescriptionRequest) {
        encodeResult = CoreDiscoveryCodec::encodeDescriptionResponse(
            writer, device, std::span<const ServiceFamilySupport>(restricted.data(), restrictedCount));
    } else {
        const bool extended = (header.serviceType == CoreDiscoveryCodec::kServiceSearchRequestExtended);
        const SearchFilter filter = extended
            ? evaluateSearchParameters(frame, position, device)
            : SearchFilter{};
        if (!filter.respond) return true;

        const control_packet::HpaiEndpoint controlEndpoint{
            .protocol = control_packet::HpaiProtocol::Udp,
            .address = controlAddress_,
            .port = controlPort_.value(),
        };

        encodeResult = extended
            ? CoreDiscoveryCodec::encodeSearchResponseExtended(
                  writer,
                  controlEndpoint,
                  device,
                  std::span<const ServiceFamilySupport>(families_.data(), familyCount_),
                  filter.requestedDibs)
            : CoreDiscoveryCodec::encodeSearchResponse(
                  writer,
                  controlEndpoint,
                  device,
                  std::span<const ServiceFamilySupport>(restricted.data(), restrictedCount));
    }

    if (encodeResult.isError()) {
        KNX_LOGW(TAG, "Failed to build Core response: %d", static_cast<int>(encodeResult.error()));
        return true;
    }

    const auto destination = resolveReturnEndpoint(endpointResult.value(), sourceAddress, sourcePort);
    reply.address = destination.address;
    reply.port = NetIpPort(destination.port);
    reply.datagram = writer.span();
    return true;
}

} // namespace netip
} // namespace knx
