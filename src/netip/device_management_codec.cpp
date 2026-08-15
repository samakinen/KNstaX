// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/device_management_codec.hpp"

namespace knx {
namespace netip {
namespace device_management {
namespace {

util::Result<void> validateTarget(const PropertyAccessTarget& target)
{
    if (target.elementCount == 0 || target.elementCount > 0x0F) return util::ErrorCode::InvalidParameter;
    if (target.startIndex > 0x0FFF) return util::ErrorCode::InvalidParameter;
    return util::Result<void>::ok();
}

util::Result<void> encodeHeader(std::span<uint8_t> out,
                                const ConnectionHeader& connection,
                                uint8_t messageCode,
                                const PropertyAccessTarget& target)
{
    if (out.size() < kPropertyHeaderSize) return util::ErrorCode::BufferTooSmall;
    auto validateResult = validateTarget(target);
    if (validateResult.isError()) return validateResult.error();

    out[0] = kConnectionHeaderLength;
    out[1] = connection.channelId.value();
    out[2] = connection.sequenceCounter;
    out[3] = 0x00;
    out[4] = messageCode;
    out[5] = static_cast<uint8_t>((target.objectType.value() >> 8) & 0xFF);
    out[6] = static_cast<uint8_t>(target.objectType.value() & 0xFF);
    out[7] = target.objectInstance.value();
    return util::Result<void>::ok();
}

util::Result<void> encodePropertyFields(std::span<uint8_t> out, const PropertyAccessTarget& target)
{
    if (out.size() < 3) return util::ErrorCode::BufferTooSmall;

    out[0] = static_cast<uint8_t>(target.propertyId);
    out[1] = static_cast<uint8_t>((target.elementCount << 4) | ((target.startIndex >> 8) & 0x0F));
    out[2] = static_cast<uint8_t>(target.startIndex & 0xFF);
    return util::Result<void>::ok();
}

template <typename T>
util::Result<T> decodeCommon(std::span<const uint8_t> payload, uint8_t expectedMessageCode)
{
    if (payload.size() < 11) return util::ErrorCode::InvalidFrameSize;
    if (payload[0] != kConnectionHeaderLength) return util::ErrorCode::DecodeFailed;
    if (payload[3] != 0x00) return util::ErrorCode::DecodeFailed;
    if (payload[4] != expectedMessageCode) return util::ErrorCode::DecodeFailed;

    T result{};
    result.connection.channelId = ChannelId(payload[1]);
    result.connection.sequenceCounter = payload[2];
    result.target.objectType = InterfaceObjectType((static_cast<uint16_t>(payload[5]) << 8) | payload[6]);
    result.target.objectInstance = InterfaceObjectInstance(payload[7]);
    result.target.propertyId = static_cast<application::PropertyID>(payload[8]);
    result.target.elementCount = static_cast<uint8_t>((payload[9] >> 4) & 0x0F);
    result.target.startIndex = static_cast<uint16_t>(((payload[9] & 0x0F) << 8) | payload[10]);
    return result;
}

} // namespace

util::Result<size_t> encodePropertyReadRequest(const PropertyReadRequest& request, std::span<uint8_t> out)
{
    if (out.size() < 11) return util::ErrorCode::BufferTooSmall;

    auto headerResult = encodeHeader(out, request.connection, kPropertyReadRequestCode, request.target);
    if (headerResult.isError()) return headerResult.error();
    auto propertyResult = encodePropertyFields(out.subspan(8), request.target);
    if (propertyResult.isError()) return propertyResult.error();
    return 11;
}

util::Result<size_t> encodePropertyWriteRequest(const PropertyWriteRequest& request, std::span<uint8_t> out)
{
    if (out.size() < 11 + request.data.size()) return util::ErrorCode::BufferTooSmall;

    auto headerResult = encodeHeader(out, request.connection, kPropertyWriteRequestCode, request.target);
    if (headerResult.isError()) return headerResult.error();
    auto propertyResult = encodePropertyFields(out.subspan(8), request.target);
    if (propertyResult.isError()) return propertyResult.error();
    for (size_t i = 0; i < request.data.size(); ++i) {
        out[11 + i] = request.data[i];
    }
    return 11 + request.data.size();
}

util::Result<PropertyReadConfirmationView> decodePropertyReadConfirmation(std::span<const uint8_t> payload)
{
    auto commonResult = decodeCommon<PropertyReadConfirmationView>(payload, kPropertyReadConfirmationCode);
    if (commonResult.isError()) return commonResult.error();
    auto result = commonResult.value();
    result.data = payload.subspan(11);
    return result;
}

util::Result<PropertyWriteConfirmation> decodePropertyWriteConfirmation(std::span<const uint8_t> payload)
{
    return decodeCommon<PropertyWriteConfirmation>(payload, kPropertyWriteConfirmationCode);
}

} // namespace device_management
} // namespace netip
} // namespace knx