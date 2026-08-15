// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file network_parameter_service.cpp
 * @brief Serial-number-addressed commissioning and network parameter services.
 */

#include "knx/application/network_parameter_service.hpp"

#include "knx/protocol/tpdu_codec.hpp"
#include "knx/util/log.hpp"

#include <algorithm>

namespace knx {
namespace application {

namespace {

constexpr const char* TAG = "KNX.App.NetParam";

/// A_IndividualAddressSerialNumber_Write carries 4 reserved octets after the
/// new address (03/03/07 Figure 14).  They are transmitted as zero and ignored
/// on receipt, but their presence is what makes the PDU 12 octets rather than 10.
constexpr size_t kSerialNumberWriteReservedBytes = 4u;

/// A_IndividualAddressSerialNumber_Response carries a 2-octet domain address
/// followed by 2 reserved octets (Figure 13).
constexpr size_t kSerialNumberResponseReservedBytes = 2u;

constexpr size_t kNetworkParameterHeaderBytes = 3u;  // objectType(2) + propertyId(1)

util::Result<size_t> buildBroadcast(APCIService service,
                                    std::span<const uint8_t> payload,
                                    std::span<uint8_t> out)
{
    return knx::protocol::buildTpdu(
        knx::protocol::TPCIField::create(knx::protocol::TPCI::UnnumberedData),
        APCIField::create(service),
        payload,
        out);
}

}  // namespace

// ---------------------------------------------------------------------------
// Decoders
// ---------------------------------------------------------------------------

util::Result<KnxSerialNumber> NetworkParameterService::decodeSerialNumberRead(
    std::span<const uint8_t> apdu)
{
    if (apdu.size() < 2u + kKnxSerialNumberBytes) {
        return util::ErrorCode::DecodeFailed;
    }
    const auto header = knx::protocol::unpackTpduHeader(apdu[0], apdu[1]);
    if (header.apci.service() != APCIService::IndividualAddressSerialNumberRead) {
        return util::ErrorCode::DecodeFailed;
    }

    KnxSerialNumber serial{};
    std::copy_n(apdu.begin() + 2, kKnxSerialNumberBytes, serial.begin());
    return serial;
}

util::Result<IndividualAddressSerialNumberWrite> NetworkParameterService::decodeSerialNumberWrite(
    std::span<const uint8_t> apdu)
{
    // Header + serial + new address.  The 4 reserved octets are tolerated but
    // not required: some tools omit them and the payload is unambiguous either
    // way, so rejecting a short-but-complete PDU would only break interop.
    constexpr size_t kMinimum = 2u + kKnxSerialNumberBytes + 2u;
    if (apdu.size() < kMinimum) {
        return util::ErrorCode::DecodeFailed;
    }
    const auto header = knx::protocol::unpackTpduHeader(apdu[0], apdu[1]);
    if (header.apci.service() != APCIService::IndividualAddressSerialNumberWrite) {
        return util::ErrorCode::DecodeFailed;
    }

    IndividualAddressSerialNumberWrite request{};
    std::copy_n(apdu.begin() + 2, kKnxSerialNumberBytes, request.serialNumber.begin());

    const size_t addressOffset = 2u + kKnxSerialNumberBytes;
    request.newAddress = IndividualAddress(static_cast<uint16_t>(
        (static_cast<uint16_t>(apdu[addressOffset]) << 8) | apdu[addressOffset + 1]));

    return request;
}

util::Result<NetworkParameterRequest> NetworkParameterService::decodeNetworkParameter(
    std::span<const uint8_t> apdu)
{
    if (apdu.size() < 2u + kNetworkParameterHeaderBytes) {
        return util::ErrorCode::DecodeFailed;
    }
    const auto header = knx::protocol::unpackTpduHeader(apdu[0], apdu[1]);
    const auto service = header.apci.service();
    if (service != APCIService::NetworkParameterRead &&
        service != APCIService::NetworkParameterWrite &&
        service != APCIService::NetworkParameterResponse) {
        return util::ErrorCode::DecodeFailed;
    }

    NetworkParameterRequest request{};
    request.objectType = InterfaceObjectType(static_cast<uint16_t>(
        (static_cast<uint16_t>(apdu[2]) << 8) | apdu[3]));
    request.propertyId = static_cast<PropertyID>(apdu[4]);

    const auto value = apdu.subspan(2u + kNetworkParameterHeaderBytes);
    if (value.size() > kMaxNetworkParameterValueBytes) {
        return util::ErrorCode::BufferTooSmall;
    }
    if (!request.value.assign(value)) {
        return util::ErrorCode::BufferTooSmall;
    }

    return request;
}

// ---------------------------------------------------------------------------
// Encoders
// ---------------------------------------------------------------------------

util::Result<size_t> NetworkParameterService::encodeSerialNumberRead(
    const KnxSerialNumber& serialNumber, std::span<uint8_t> out)
{
    return buildBroadcast(APCIService::IndividualAddressSerialNumberRead, serialNumber, out);
}

util::Result<size_t> NetworkParameterService::encodeSerialNumberResponse(
    const KnxSerialNumber& serialNumber, uint16_t domainAddress, std::span<uint8_t> out)
{
    std::array<uint8_t, kKnxSerialNumberBytes + 2u + kSerialNumberResponseReservedBytes> payload{};
    std::copy(serialNumber.begin(), serialNumber.end(), payload.begin());
    payload[kKnxSerialNumberBytes] = static_cast<uint8_t>((domainAddress >> 8) & 0xFFu);
    payload[kKnxSerialNumberBytes + 1u] = static_cast<uint8_t>(domainAddress & 0xFFu);
    // Reserved octets stay zero.
    return buildBroadcast(APCIService::IndividualAddressSerialNumberResponse, payload, out);
}

util::Result<size_t> NetworkParameterService::encodeSerialNumberWrite(
    const KnxSerialNumber& serialNumber, const IndividualAddress& newAddress, std::span<uint8_t> out)
{
    std::array<uint8_t, kKnxSerialNumberBytes + 2u + kSerialNumberWriteReservedBytes> payload{};
    std::copy(serialNumber.begin(), serialNumber.end(), payload.begin());
    payload[kKnxSerialNumberBytes] = static_cast<uint8_t>((newAddress.raw >> 8) & 0xFFu);
    payload[kKnxSerialNumberBytes + 1u] = static_cast<uint8_t>(newAddress.raw & 0xFFu);
    return buildBroadcast(APCIService::IndividualAddressSerialNumberWrite, payload, out);
}

util::Result<size_t> NetworkParameterService::encodeNetworkParameterResponse(
    InterfaceObjectType objectType,
    PropertyID propertyId,
    std::span<const uint8_t> value,
    std::span<uint8_t> out)
{
    if (value.size() > kMaxNetworkParameterValueBytes) {
        return util::ErrorCode::InvalidParameter;
    }
    std::array<uint8_t, kNetworkParameterHeaderBytes + kMaxNetworkParameterValueBytes> payload{};
    payload[0] = static_cast<uint8_t>((objectType.value() >> 8) & 0xFFu);
    payload[1] = static_cast<uint8_t>(objectType.value() & 0xFFu);
    payload[2] = static_cast<uint8_t>(propertyId);
    std::copy(value.begin(), value.end(), payload.begin() + kNetworkParameterHeaderBytes);

    return buildBroadcast(APCIService::NetworkParameterResponse,
                          std::span<const uint8_t>(payload.data(),
                                                   kNetworkParameterHeaderBytes + value.size()),
                          out);
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

util::Result<void> NetworkParameterService::handleSerialNumberRead(const KnxSerialNumber& requested)
{
    // Broadcast: every device on the line receives this.  Answering when the
    // serial number is not ours would collide with the real target's response.
    if (requested != _serialNumber) {
        return util::Result<void>::ok();
    }

    if (!_serialResponseCallback) {
        KNX_LOGW(TAG, "Serial number read matched but no response callback registered");
        return util::ErrorCode::OperationNotReady;
    }

    KNX_LOGI(TAG, "Serial number read matched; answering with domain address 0x%04X",
             static_cast<unsigned>(_domainAddress));
    _serialResponseCallback(_serialNumber, _domainAddress);
    return util::Result<void>::ok();
}

util::Result<void> NetworkParameterService::handleSerialNumberWrite(
    const IndividualAddressSerialNumberWrite& request)
{
    if (request.serialNumber != _serialNumber) {
        return util::Result<void>::ok();
    }

    if (!request.newAddress.isValid()) {
        KNX_LOGW(TAG, "Refusing serial-number write of invalid address 0x%04X",
                 static_cast<unsigned>(request.newAddress.raw));
        return util::ErrorCode::InvalidAddress;
    }

    if (!_addressWriteCallback) {
        KNX_LOGW(TAG, "Serial number write matched but no address-write callback registered");
        return util::ErrorCode::OperationNotReady;
    }

    KNX_LOGI(TAG, "Serial-number-addressed individual address write: %u.%u.%u",
             request.newAddress.area(), request.newAddress.line(), request.newAddress.device());
    return _addressWriteCallback(request.newAddress);
}

util::Result<void> NetworkParameterService::handleNetworkParameterRead(
    const NetworkParameterRequest& request)
{
    if (!_networkParameterRead || !_networkParameterResponse) {
        return util::Result<void>::ok();  // Not exposed: stay silent.
    }

    auto value = _networkParameterRead(request);
    if (value.isError()) {
        // "We do not have that parameter" is not an error on a broadcast; it
        // simply means this device is not part of the answer set.
        return util::Result<void>::ok();
    }

    _networkParameterResponse(request.objectType, request.propertyId, value.value().span());
    return util::Result<void>::ok();
}

util::Result<void> NetworkParameterService::handleNetworkParameterWrite(
    const NetworkParameterRequest& request)
{
    if (!_networkParameterWrite) {
        return util::Result<void>::ok();
    }
    return _networkParameterWrite(request);
}

} // namespace application
} // namespace knx
