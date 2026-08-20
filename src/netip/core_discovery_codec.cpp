// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/core_discovery_codec.hpp"

#include "knx/util/bit_ops.hpp"

#include <algorithm>

namespace knx {
namespace netip {

namespace bits = knx::util;

namespace {

/// Is `type` in the basic set every response carries regardless of the SRP?
constexpr bool isBasicDib(uint8_t type) noexcept
{
    return type == CoreDiscoveryCodec::kDibDeviceInfo ||
           type == CoreDiscoveryCodec::kDibSupportedServiceFamilies ||
           type == CoreDiscoveryCodec::kDibExtendedDeviceInfo;
}

} // namespace

util::Result<void> CoreDiscoveryCodec::appendDeviceInfoDib(PacketWriter& writer,
                                                           const CoreDeviceDescription& device)
{
    std::array<uint8_t, kDeviceInfoDibLength> dib{};
    size_t idx = 0;

    dib[idx++] = kDeviceInfoDibLength;
    dib[idx++] = kDibDeviceInfo;
    dib[idx++] = device.medium;
    // Only bit 0 is defined; the rest are reserved and stay clear.
    dib[idx++] = device.programmingMode ? 0x01 : 0x00;

    dib[idx++] = bits::getHighByte(device.individualAddress.raw);
    dib[idx++] = bits::getLowByte(device.individualAddress.raw);

    dib[idx++] = bits::getHighByte(device.projectInstallationId);
    dib[idx++] = bits::getLowByte(device.projectInstallationId);

    std::copy(device.serialNumber.begin(), device.serialNumber.end(), dib.begin() + idx);
    idx += device.serialNumber.size();

    uint8_t a = 0, b = 0, c = 0, d = 0;
    device.routingMulticastAddress.toOctets(a, b, c, d);
    dib[idx++] = a;
    dib[idx++] = b;
    dib[idx++] = c;
    dib[idx++] = d;

    std::copy(device.macAddress.begin(), device.macAddress.end(), dib.begin() + idx);
    idx += device.macAddress.size();

    // The name is NUL-padded rather than truncated at the first NUL: unused
    // octets "shall be filled with the NULL character" (§7.5.4.2), and a name
    // shorter than the field would otherwise leak whatever the buffer held.
    for (size_t i = 0; i < kFriendlyNameLength; ++i) {
        const char ch = (i < device.friendlyName.size()) ? device.friendlyName[i] : '\0';
        dib[idx++] = static_cast<uint8_t>(ch);
    }

    return writer.write(dib);
}

util::Result<void> CoreDiscoveryCodec::appendServiceFamiliesDib(PacketWriter& writer,
                                                                uint8_t dibType,
                                                                std::span<const ServiceFamilySupport> families)
{
    const size_t bodyLength = families.size() * 2;
    const size_t totalLength = 2 + bodyLength;
    if (totalLength > 0xFF) return util::ErrorCode::InvalidFrameSize;

    auto lengthResult = writer.push(static_cast<uint8_t>(totalLength));
    if (lengthResult.isError()) return lengthResult.error();
    auto typeResult = writer.push(dibType);
    if (typeResult.isError()) return typeResult.error();

    for (const auto& family : families) {
        auto idResult = writer.push(family.family);
        if (idResult.isError()) return idResult.error();
        auto versionResult = writer.push(family.version);
        if (versionResult.isError()) return versionResult.error();
    }

    // Each entry is a pair, so the structure is already even. Nothing to pad.
    return util::Result<void>::ok();
}

util::Result<void> CoreDiscoveryCodec::appendExtendedDeviceInfoDib(PacketWriter& writer,
                                                                   const CoreDeviceDescription& device)
{
    const std::array<uint8_t, kExtendedDeviceInfoDibLength> dib{
        kExtendedDeviceInfoDibLength,
        kDibExtendedDeviceInfo,
        device.mediumStatus,
        0x00,  // reserved
        bits::getHighByte(device.maxLocalApduLength),
        bits::getLowByte(device.maxLocalApduLength),
        bits::getHighByte(device.deviceDescriptorType0),
        bits::getLowByte(device.deviceDescriptorType0),
    };
    return writer.write(dib);
}

util::Result<void> CoreDiscoveryCodec::encodeSearchResponse(PacketWriter& writer,
                                                            const control_packet::HpaiEndpoint& controlEndpoint,
                                                            const CoreDeviceDescription& device,
                                                            std::span<const ServiceFamilySupport> families)
{
    auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
    if (resetResult.isError()) return resetResult.error();

    auto hpaiResult = control_packet::Codec::appendHpai(writer, controlEndpoint);
    if (hpaiResult.isError()) return hpaiResult.error();

    auto deviceResult = appendDeviceInfoDib(writer, device);
    if (deviceResult.isError()) return deviceResult.error();

    auto familiesResult = appendServiceFamiliesDib(writer, kDibSupportedServiceFamilies, families);
    if (familiesResult.isError()) return familiesResult.error();

    return control_packet::Codec::encodePacketHeader(writer.span(), control_packet::kServiceSearchResponse);
}

util::Result<void> CoreDiscoveryCodec::encodeSearchResponseExtended(
    PacketWriter& writer,
    const control_packet::HpaiEndpoint& controlEndpoint,
    const CoreDeviceDescription& device,
    std::span<const ServiceFamilySupport> families,
    std::span<const uint8_t> requestedDibs)
{
    auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
    if (resetResult.isError()) return resetResult.error();

    auto hpaiResult = control_packet::Codec::appendHpai(writer, controlEndpoint);
    if (hpaiResult.isError()) return hpaiResult.error();

    auto deviceResult = appendDeviceInfoDib(writer, device);
    if (deviceResult.isError()) return deviceResult.error();

    auto familiesResult = appendServiceFamiliesDib(writer, kDibSupportedServiceFamilies, families);
    if (familiesResult.isError()) return familiesResult.error();

    auto extendedResult = appendExtendedDeviceInfoDib(writer, device);
    if (extendedResult.isError()) return extendedResult.error();

    // Anything else the client listed and this device can answer. Types it does
    // not recognise are ignored rather than refused (§7.6.3.3.6); the basic set
    // is already present, so asking for it again changes nothing.
    for (const uint8_t type : requestedDibs) {
        if (type == 0 || isBasicDib(type)) continue;
        // No other DIB is supported yet. When one is, it belongs here — and
        // until then silence is the specified behaviour, not a gap.
    }

    return control_packet::Codec::encodePacketHeader(writer.span(), kServiceSearchResponseExtended);
}

util::Result<void> CoreDiscoveryCodec::encodeDescriptionResponse(PacketWriter& writer,
                                                                 const CoreDeviceDescription& device,
                                                                 std::span<const ServiceFamilySupport> families)
{
    auto resetResult = writer.reset(KnxNetIpCodec::kHeaderLen);
    if (resetResult.isError()) return resetResult.error();

    auto deviceResult = appendDeviceInfoDib(writer, device);
    if (deviceResult.isError()) return deviceResult.error();

    auto familiesResult = appendServiceFamiliesDib(writer, kDibSupportedServiceFamilies, families);
    if (familiesResult.isError()) return familiesResult.error();

    return control_packet::Codec::encodePacketHeader(writer.span(), control_packet::kServiceDescriptionResponse);
}

util::Result<control_packet::HpaiEndpoint> CoreDiscoveryCodec::parseReturnEndpoint(
    std::span<const uint8_t> datagram,
    NetIpServiceType expectedServiceType)
{
    KnxNetIpHeader header;
    auto headerResult = KnxNetIpCodec::decodeHeader(datagram, header);
    if (headerResult.isError()) return headerResult.error();
    if (header.serviceType != expectedServiceType) return util::ErrorCode::DecodeFailed;
    if (header.totalLength > datagram.size()) return util::ErrorCode::InvalidFrameSize;

    size_t position = KnxNetIpCodec::kHeaderLen;
    return control_packet::Codec::decodeHpai(datagram.first(header.totalLength), position);
}

} // namespace netip
} // namespace knx
