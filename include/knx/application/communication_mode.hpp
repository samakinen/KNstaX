// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file communication_mode.hpp
 * @brief Which Transport Layer communication modes each A_ service may use
 *
 * 03/03/07 §2.2 Table 1 marks, for every APCI, the communication modes it is
 * defined on (M or O) and the ones it "shall not be implemented for" (x). A
 * request that arrives on a mode its service is not defined for is not that
 * service: an A_Memory_Write sent to a group address, or an
 * A_PropertyValue_Write sent as a broadcast, has to be ignored, exactly like
 * an undefined APCI (§2.2, "not defined and not supported Application Layer
 * services shall be ignored").
 */

#pragma once

#include "knx/application/apci_services.hpp"

#include <cstdint>
#include <optional>

namespace knx {
namespace application {

/// One bit per column of 03/03/07 Table 1.
enum class CommunicationMode : uint8_t {
    Multicast       = 1u << 0,  ///< T_Data_Group
    TagGroup        = 1u << 1,  ///< T_Data_Tag_Group
    Broadcast       = 1u << 2,  ///< T_Data_Broadcast
    SystemBroadcast = 1u << 3,  ///< T_Data_SystemBroadcast
    Connectionless  = 1u << 4,  ///< T_Data_Individual
    Connected       = 1u << 5,  ///< T_Data_Connected
};

/// A set of CommunicationMode bits.
using CommunicationModes = uint8_t;

constexpr CommunicationModes operator|(CommunicationMode a, CommunicationMode b) noexcept
{
    return static_cast<CommunicationModes>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr CommunicationModes operator|(CommunicationModes a, CommunicationMode b) noexcept
{
    return static_cast<CommunicationModes>(a | static_cast<uint8_t>(b));
}

constexpr CommunicationModes modes(CommunicationMode mode) noexcept
{
    return static_cast<CommunicationModes>(mode);
}

/**
 * @brief The communication modes 03/03/07 Table 1 defines @p service on.
 *
 * Every column marked M or O counts: an optional mode is one a device may
 * implement, and this stack implements all of them.
 *
 * Returns nullopt for an APCI this table does not cover (manufacturer USERMSG,
 * A_Link_*, A_FileStream_*, the coupler services and the like). The caller
 * leaves those as they were — handed to the user — rather than guessing.
 *
 * The extended property and function property rows of Table 1 have empty
 * cells in this edition; their modes come from the service clauses instead,
 * which map them to T_Data_Individual "or the T_Data_Connected" (§3.4.5,
 * §3.4.8), the same as the classic property services.
 */
constexpr std::optional<CommunicationModes> definedCommunicationModes(APCIService service) noexcept
{
    using M = CommunicationMode;
    constexpr CommunicationModes kPointToPoint = M::Connectionless | M::Connected;
    constexpr CommunicationModes kAnyBroadcast = M::Broadcast | M::SystemBroadcast;

    switch (service) {
        // §3.1: multicast only.
        case APCIService::GroupValueRead:
        case APCIService::GroupValueResponse:
        case APCIService::GroupValueWrite:
            return modes(M::Multicast);

        // §3.2: broadcast only. ETS's "program individual address" and its
        // programming-mode scan both arrive this way.
        case APCIService::IndividualAddressWrite:
        case APCIService::IndividualAddressRead:
        case APCIService::IndividualAddressResponse:
        case APCIService::IndividualAddressSerialNumberRead:
        case APCIService::IndividualAddressSerialNumberResponse:
        case APCIService::IndividualAddressSerialNumberWrite:
            return modes(M::Broadcast);

        // §3.2.6/§3.2.7: point-to-all-points or point-to-point.
        case APCIService::NetworkParameterRead:
        case APCIService::NetworkParameterResponse:
        case APCIService::NetworkParameterWrite:
            return modes(M::Broadcast) | kPointToPoint;

        // §3.3: system broadcast, and plain broadcast for the domain-address
        // services on media that have no system broadcast (§3.3.1).
        case APCIService::SystemNetworkParameterRead:
        case APCIService::SystemNetworkParameterResponse:
        case APCIService::SystemNetworkParameterWrite:
            return modes(M::SystemBroadcast);
        case APCIService::DomainAddressSerialNumberRead:
        case APCIService::DomainAddressSerialNumberResponse:
        case APCIService::DomainAddressSerialNumberWrite:
            return kAnyBroadcast;

        // §3.4: point-to-point, connectionless or on a connection.
        case APCIService::DeviceDescriptorRead:
        case APCIService::DeviceDescriptorResponse:
        case APCIService::Restart:  // what A_Restart_Response decodes to as well
        case APCIService::RestartResponse:
        case APCIService::PropertyValueRead:
        case APCIService::PropertyValueResponse:
        case APCIService::PropertyValueWrite:
        case APCIService::PropertyDescriptionRead:
        case APCIService::PropertyDescriptionResponse:
        case APCIService::PropertyExtValueRead:
        case APCIService::PropertyExtValueResponse:
        case APCIService::PropertyExtValueWriteCon:
        case APCIService::PropertyExtValueWriteConRes:
        case APCIService::PropertyExtValueWriteUnCon:
        case APCIService::PropertyExtValueInfoReport:
        case APCIService::PropertyExtDescriptionRead:
        case APCIService::PropertyExtDescriptionResponse:
        case APCIService::FunctionPropertyCommand:
        case APCIService::FunctionPropertyStateRead:
        case APCIService::FunctionPropertyStateResponse:
        case APCIService::FunctionPropertyExtCommand:
        case APCIService::FunctionPropertyExtStateRead:
        case APCIService::FunctionPropertyExtStateResponse:
        case APCIService::MemoryExtendedWrite:
        case APCIService::MemoryExtendedWriteResponse:
        case APCIService::MemoryExtendedRead:
        case APCIService::MemoryExtendedReadResponse:
        // A_Memory_* are optional connectionless and mandatory connected.
        case APCIService::MemoryRead:
        case APCIService::MemoryResponse:
        case APCIService::MemoryWrite:
            return kPointToPoint;

        // §3.5: connection-oriented only.
        case APCIService::ADCRead:
        case APCIService::ADCResponse:
        case APCIService::AuthorizeRequest:
        case APCIService::AuthorizeResponse:
        case APCIService::KeyWrite:
        case APCIService::KeyResponse:
            return modes(M::Connected);

        case APCIService::Escape:
            break;
    }
    return std::nullopt;
}

/**
 * @brief True when a request for @p service may be processed after arriving
 *        on any of the modes in @p received.
 *
 * @p received is a set rather than a single mode because a frame to the
 * broadcast address 0000h can be either T_Data_Broadcast or
 * T_Data_SystemBroadcast and the frame as delivered up the stack no longer
 * says which. Services the table does not cover are always accepted.
 */
constexpr bool isCommunicationModeAllowed(APCIService service, CommunicationModes received) noexcept
{
    const auto defined = definedCommunicationModes(service);
    return !defined.has_value() || (*defined & received) != 0u;
}

} // namespace application
} // namespace knx
