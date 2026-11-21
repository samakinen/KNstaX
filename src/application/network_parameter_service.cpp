// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file network_parameter_service.cpp
 * @brief Serial-number-addressed commissioning and network parameter services.
 */

#include "knx/application/network_parameter_service.hpp"

#include "knx/util/log.hpp"

#include <algorithm>

namespace knx {
namespace application {

namespace {

constexpr const char* TAG = "KNX.App.NetParam";

}  // namespace

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

    // A_IndividualAddressSerialNumber_Response carries a fixed two-octet domain
    // address field whatever the medium, so a wider one — the four-octet
    // multicast address of a KNX IP device, say — has no room here and goes out
    // as zero. The full value is what A_DomainAddressSerialNumber_Read answers.
    const uint16_t narrowDomainAddress =
        (_domainAddress.size() == 2u)
            ? static_cast<uint16_t>((static_cast<uint16_t>(_domainAddress[0]) << 8)
                                    | _domainAddress[1])
            : 0u;

    KNX_LOGI(TAG, "Serial number read matched; answering with domain address 0x%04X",
             static_cast<unsigned>(narrowDomainAddress));
    _serialResponseCallback(_serialNumber, narrowDomainAddress);
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

util::Result<void> NetworkParameterService::handleDomainAddressSerialNumberRead(
    const KnxSerialNumber& requested)
{
    // 03/03/07 §3.3.6: answer only when the serial number is ours. Every other
    // device on the system broadcast hears the same frame.
    if (requested != _serialNumber) {
        return util::Result<void>::ok();
    }
    if (!_domainAddressResponseCallback) {
        return util::Result<void>::ok();
    }

    KNX_LOGI(TAG, "Domain address read matched; answering with %zu-octet address",
             _domainAddress.size());
    _domainAddressResponseCallback(_serialNumber, _domainAddress.span());
    return util::Result<void>::ok();
}

util::Result<void> NetworkParameterService::handleDomainAddressSerialNumberWrite(
    const KnxSerialNumber& requested, std::span<const uint8_t> domainAddress)
{
    if (requested != _serialNumber) {
        return util::Result<void>::ok();
    }

    // §3.3.7 gives the address one width per medium — 2 on PL110, 6 on RF, 4 on
    // plain KNX IP. A write of a different width is addressed at a device on
    // another medium that happens to share our serial number, which cannot
    // happen, or it is malformed. Either way it is not ours to apply.
    if (domainAddress.size() != _domainAddress.size()) {
        KNX_LOGW(TAG,
                 "Refusing %zu-octet domain address write: this medium uses %zu octets",
                 domainAddress.size(), _domainAddress.size());
        return util::ErrorCode::InvalidParameter;
    }

    if (!_domainAddressWriteCallback) {
        // No owner to apply it. Recording it anyway would report an address
        // back that the device is not actually reachable on.
        KNX_LOGW(TAG, "Domain address write matched but no write callback registered");
        return util::ErrorCode::OperationNotReady;
    }

    auto applied = _domainAddressWriteCallback(domainAddress);
    if (applied.isError()) {
        KNX_LOGW(TAG, "Domain address write refused by the owner: %s",
                 util::errorCodeToString(applied.error()));
        return applied;
    }

    _domainAddress.clear();
    (void)_domainAddress.assign(domainAddress);
    KNX_LOGI(TAG, "Domain address set by serial-number write (%zu octets)",
             _domainAddress.size());
    return util::Result<void>::ok();
}

#if KNX_FEATURE_NETWORK_PARAMETER
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
#endif

} // namespace application
} // namespace knx
