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

#include "knx/config.hpp"
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
    void(const KnxSerialNumber& serialNumber, uint16_t domainAddress), ::knx::config::callbackBytes(64)>;

/**
 * @brief Applies a new individual address addressed by serial number.
 */
using SerialNumberAddressWriteCallback = util::InplaceFunction<
    util::Result<void>(const IndividualAddress& newAddress), ::knx::config::callbackBytes(64)>;

/**
 * @brief The Domain Address in the encoding its medium uses.
 *
 * 03/03/07 §3.3.7: two octets on KNX-PL110, six on KNX-RF, and four on plain
 * KNX IP where it is the routing multicast address. One value, three widths —
 * so it is carried as bytes and the length is what says which medium this is.
 */
using DomainAddressBuffer = util::FixedVector<uint8_t, 6>;

/**
 * @brief Emits an A_DomainAddressSerialNumber_Response system broadcast.
 */
using DomainAddressResponseCallback = util::InplaceFunction<
    void(const KnxSerialNumber& serialNumber, std::span<const uint8_t> domainAddress), ::knx::config::callbackBytes(64)>;

/**
 * @brief Applies a Domain Address addressed by serial number.
 *
 * Returning an error refuses the change and leaves the stored value alone.
 * On KNX IP the BusAccessUnit installs one that stores the address as
 * PID_ROUTING_MULTICAST_ADDRESS (03/03/07 §3.3.7), active after the next
 * restart like any other write of that property.
 */
using DomainAddressWriteCallback = util::InplaceFunction<
    util::Result<void>(std::span<const uint8_t> domainAddress), ::knx::config::callbackBytes(64)>;

/**
 * @brief Answers A_NetworkParameter_Read.  Returning an error means "this
 * device does not expose that parameter", which per spec means stay silent.
 */
using NetworkParameterReadHandler = util::InplaceFunction<
    util::Result<NetworkParameterValueBuffer>(const NetworkParameterRequest& request), ::knx::config::callbackBytes(64)>;

using NetworkParameterWriteHandler = util::InplaceFunction<
    util::Result<void>(const NetworkParameterRequest& request), ::knx::config::callbackBytes(64)>;

using NetworkParameterResponseCallback = util::InplaceFunction<
    void(InterfaceObjectType objectType,
         PropertyID propertyId,
         std::span<const uint8_t> value), ::knx::config::callbackBytes(64)>;

/**
 * @brief Handler for the serial-number and network-parameter broadcast services.
 *
 * @thread_safety Owner-context only.
 */
class NetworkParameterService {
public:
    void setSerialNumber(const KnxSerialNumber& serialNumber) { _serialNumber = serialNumber; }
    const KnxSerialNumber& serialNumber() const { return _serialNumber; }

    /**
     * @brief The device's Domain Address, in its medium's encoding.
     *
     * Empty on TP1, where the concept only exists for open media. The length
     * set here is the length this device will accept in a write and emit in a
     * response, so it is also how the service learns which medium it is on.
     */
    void setDomainAddress(std::span<const uint8_t> domainAddress) {
        _domainAddress.clear();
        (void)_domainAddress.assign(domainAddress);
    }
    [[nodiscard]] std::span<const uint8_t> domainAddress() const { return _domainAddress.span(); }

    void setDomainAddressResponseCallback(DomainAddressResponseCallback callback) {
        _domainAddressResponseCallback = std::move(callback);
    }
    void setDomainAddressWriteCallback(DomainAddressWriteCallback callback) {
        _domainAddressWriteCallback = std::move(callback);
    }

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

    /// A_DomainAddressSerialNumber_Read.ind: answer only when the serial is ours.
    util::Result<void> handleDomainAddressSerialNumberRead(const KnxSerialNumber& requested);

    /// A_DomainAddressSerialNumber_Write.ind: apply only when the serial is ours
    /// and the address is the width this medium uses.
    util::Result<void> handleDomainAddressSerialNumberWrite(const KnxSerialNumber& requested,
                                                            std::span<const uint8_t> domainAddress);

    util::Result<void> handleNetworkParameterRead(const NetworkParameterRequest& request);
    util::Result<void> handleNetworkParameterWrite(const NetworkParameterRequest& request);

private:
    KnxSerialNumber _serialNumber{};
    DomainAddressBuffer _domainAddress{};
    DomainAddressResponseCallback _domainAddressResponseCallback{};
    DomainAddressWriteCallback _domainAddressWriteCallback{};
    SerialNumberResponseCallback _serialResponseCallback{};
    SerialNumberAddressWriteCallback _addressWriteCallback{};
    NetworkParameterReadHandler _networkParameterRead{};
    NetworkParameterWriteHandler _networkParameterWrite{};
    NetworkParameterResponseCallback _networkParameterResponse{};
};

} // namespace application
} // namespace knx
