// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file network_parameter_service.hpp
 * @brief Serial-number-addressed commissioning and network parameter services.
 *
 * Covers three KNX broadcast services (03/03/07 §3.2.4, §3.2.5 and Table 1):
 *
 *  - A_IndividualAddressSerialNumber_Read / _Response
 *  - A_IndividualAddressSerialNumber_Write
 *  - A_NetworkParameter_Read / _Write / _Response
 *
 * The serial-number pair is what lets a management client address one specific
 * device on a shared bus without anybody walking up and pressing a programming
 * button.  With only A_IndividualAddress_Write (which addresses "whichever
 * device is in programming mode") a device that is physically hard to reach
 * cannot be commissioned at all.
 */

#pragma once

#include "knx/application/apci_services.hpp"
#include "knx/application/property.hpp"
#include "knx/types.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace application {

/// KNX serial numbers are 6 octets, administered by the KNX Association.
inline constexpr size_t kKnxSerialNumberBytes = 6u;
using KnxSerialNumber = std::array<uint8_t, kKnxSerialNumberBytes>;

inline constexpr size_t kMaxNetworkParameterValueBytes = 16u;
using NetworkParameterValueBuffer =
    util::FixedVector<uint8_t, kMaxNetworkParameterValueBytes>;

/**
 * @brief Decoded A_IndividualAddressSerialNumber_Write request.
 */
struct IndividualAddressSerialNumberWrite {
    KnxSerialNumber serialNumber{};
    IndividualAddress newAddress{};
};

/**
 * @brief Decoded A_NetworkParameter_Read/_Write request.
 *
 * The parameter is addressed by interface object type plus property id, which
 * is why this is not simply a property service: it is answered by *any* device
 * on the line matching the test value, without an individual address.
 */
struct NetworkParameterRequest {
    InterfaceObjectType objectType{0};
    PropertyID propertyId{};
    /// For Read this is the test value to compare against; for Write it is the
    /// value to store.
    NetworkParameterValueBuffer value{};
};

/**
 * @brief Emits an A_IndividualAddressSerialNumber_Response broadcast.
 */
using SerialNumberResponseCallback = util::InplaceFunction<
    void(const KnxSerialNumber& serialNumber, uint16_t domainAddress), 64>;

/**
 * @brief Applies a new individual address addressed by serial number.
 */
using SerialNumberAddressWriteCallback = util::InplaceFunction<
    util::Result<void>(const IndividualAddress& newAddress), 64>;

/**
 * @brief Answers A_NetworkParameter_Read.  Returning an error means "this
 * device does not expose that parameter", which per spec means stay silent.
 */
using NetworkParameterReadHandler = util::InplaceFunction<
    util::Result<NetworkParameterValueBuffer>(const NetworkParameterRequest& request), 64>;

using NetworkParameterWriteHandler = util::InplaceFunction<
    util::Result<void>(const NetworkParameterRequest& request), 64>;

using NetworkParameterResponseCallback = util::InplaceFunction<
    void(InterfaceObjectType objectType,
         PropertyID propertyId,
         std::span<const uint8_t> value), 64>;

/**
 * @brief Handler for the serial-number and network-parameter broadcast services.
 *
 * @thread_safety Owner-context only.
 */
class NetworkParameterService {
public:
    void setSerialNumber(const KnxSerialNumber& serialNumber) { _serialNumber = serialNumber; }
    const KnxSerialNumber& serialNumber() const { return _serialNumber; }

    /// Domain address reported in the serial-number response.  Zero on TP1,
    /// where the concept only exists for open media (PL/RF).
    void setDomainAddress(uint16_t domainAddress) { _domainAddress = domainAddress; }

    void setSerialNumberResponseCallback(SerialNumberResponseCallback callback) {
        _serialResponseCallback = std::move(callback);
    }
    void setAddressWriteCallback(SerialNumberAddressWriteCallback callback) {
        _addressWriteCallback = std::move(callback);
    }
    void setNetworkParameterReadHandler(NetworkParameterReadHandler handler) {
        _networkParameterRead = std::move(handler);
    }
    void setNetworkParameterWriteHandler(NetworkParameterWriteHandler handler) {
        _networkParameterWrite = std::move(handler);
    }
    void setNetworkParameterResponseCallback(NetworkParameterResponseCallback callback) {
        _networkParameterResponse = std::move(callback);
    }

    /**
     * @brief Handle A_IndividualAddressSerialNumber_Read.
     *
     * Silently ignores requests carrying somebody else's serial number — this
     * is a broadcast, so every device on the line sees every request and only
     * the addressed one may answer.
     */
    util::Result<void> handleSerialNumberRead(const KnxSerialNumber& requested);

    /**
     * @brief Handle A_IndividualAddressSerialNumber_Write.
     */
    util::Result<void> handleSerialNumberWrite(const IndividualAddressSerialNumberWrite& request);

    util::Result<void> handleNetworkParameterRead(const NetworkParameterRequest& request);
    util::Result<void> handleNetworkParameterWrite(const NetworkParameterRequest& request);

    // --- Codecs -----------------------------------------------------------
    // `apdu` always includes the 2-octet TPCI/APCI header.

    static util::Result<KnxSerialNumber> decodeSerialNumberRead(std::span<const uint8_t> apdu);
    static util::Result<IndividualAddressSerialNumberWrite> decodeSerialNumberWrite(
        std::span<const uint8_t> apdu);
    static util::Result<NetworkParameterRequest> decodeNetworkParameter(std::span<const uint8_t> apdu);

    static util::Result<size_t> encodeSerialNumberResponse(const KnxSerialNumber& serialNumber,
                                                           uint16_t domainAddress,
                                                           std::span<uint8_t> out);
    static util::Result<size_t> encodeSerialNumberRead(const KnxSerialNumber& serialNumber,
                                                       std::span<uint8_t> out);
    static util::Result<size_t> encodeSerialNumberWrite(const KnxSerialNumber& serialNumber,
                                                        const IndividualAddress& newAddress,
                                                        std::span<uint8_t> out);
    static util::Result<size_t> encodeNetworkParameterResponse(InterfaceObjectType objectType,
                                                               PropertyID propertyId,
                                                               std::span<const uint8_t> value,
                                                               std::span<uint8_t> out);

private:
    KnxSerialNumber _serialNumber{};
    uint16_t _domainAddress{0};
    SerialNumberResponseCallback _serialResponseCallback{};
    SerialNumberAddressWriteCallback _addressWriteCallback{};
    NetworkParameterReadHandler _networkParameterRead{};
    NetworkParameterWriteHandler _networkParameterWrite{};
    NetworkParameterResponseCallback _networkParameterResponse{};
};

} // namespace application
} // namespace knx
