// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file apci_services.hpp
 * @brief KNX Application Layer Protocol Control Information (APCI) services
 * 
 * Defines APCI service codes and structures for application layer services.
 * Per KNX spec 03/03/07 (Application Layer).
 */

#pragma once

#include "knx/config.hpp"
#include "knx/application/property.hpp"
#include "knx/types.hpp"
#include "knx/util/fixed_vector.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace knx {
namespace application {

inline constexpr size_t kMaxMemoryServiceDataBytes = 63u;
using PropertyServiceDataBuffer = util::FixedVector<uint8_t, config::MAX_APDU_LENGTH>;
using MemoryServiceDataBuffer = util::FixedVector<uint8_t, kMaxMemoryServiceDataBytes>;

/**
 * @brief APCI Service Codes
 * 
 * Application Protocol Control Information codes define the service type.
 * Encoded in bits 0-9 of the APCI field (10 bits total).
 */
enum class APCIService : uint16_t {
    // Group value services
    GroupValueRead = 0x000,        ///< A_GroupValue_Read
    GroupValueResponse = 0x040,    ///< A_GroupValue_Response  
    GroupValueWrite = 0x080,       ///< A_GroupValue_Write
    
    // Individual address services
    IndividualAddressWrite = 0x0C0,      ///< A_IndividualAddress_Write
    IndividualAddressRead = 0x100,       ///< A_IndividualAddress_Read
    IndividualAddressResponse = 0x140,   ///< A_IndividualAddress_Response
    
    // ADC services
    ADCRead = 0x180,               ///< A_ADC_Read
    ADCResponse = 0x1C0,           ///< A_ADC_Response
    
    // Memory services
    MemoryRead = 0x200,            ///< A_Memory_Read
    MemoryResponse = 0x240,        ///< A_Memory_Response
    MemoryWrite = 0x280,           ///< A_Memory_Write

    // Extended property services (03/03/07 §3.4.3.2, §3.4.5, §3.4.8).  Like the
    // extended memory block below they carry the full 10-bit APCI and share the
    // 0x1C0 service group with A_ADC_Response, so they must be matched on all
    // ten bits.  Profiles v02.01.01 §9.1.2.3 makes them mandatory for the KNX
    // Data Security profile.
    PropertyExtValueRead = 0x1CC,          ///< A_PropertyExtValue_Read
    PropertyExtValueResponse = 0x1CD,      ///< A_PropertyExtValue_Response
    PropertyExtValueWriteCon = 0x1CE,      ///< A_PropertyExtValue_WriteCon
    PropertyExtValueWriteConRes = 0x1CF,   ///< A_PropertyExtValue_WriteConRes
    PropertyExtValueWriteUnCon = 0x1D0,    ///< A_PropertyExtValue_WriteUnCon
    PropertyExtValueInfoReport = 0x1D1,    ///< A_PropertyExtValue_InfoReport
    PropertyExtDescriptionRead = 0x1D2,    ///< A_PropertyExtDescription_Read
    PropertyExtDescriptionResponse = 0x1D3, ///< A_PropertyExtDescription_Response
    FunctionPropertyExtCommand = 0x1D4,     ///< A_FunctionPropertyExtCommand
    FunctionPropertyExtStateRead = 0x1D5,   ///< A_FunctionPropertyExtState_Read
    FunctionPropertyExtStateResponse = 0x1D6, ///< A_FunctionPropertyExtState_Response

    // Extended memory services.  Unlike A_Memory_*, these carry the full 10-bit
    // APCI (no data6 payload), use a 24-bit address, and answer with an
    // explicit return code.  KNX Data Secure devices are required to implement
    // them: ETS loads the Security interface object key tables through this
    // path, and the 16-bit A_Memory_* address space cannot express it.
    MemoryExtendedWrite = 0x1FB,         ///< A_MemoryExtended_Write
    MemoryExtendedWriteResponse = 0x1FC, ///< A_MemoryExtended_Write_Response
    MemoryExtendedRead = 0x1FD,          ///< A_MemoryExtended_Read
    MemoryExtendedReadResponse = 0x1FE,  ///< A_MemoryExtended_Read_Response


    // Device descriptor services
    DeviceDescriptorRead = 0x300,        ///< A_DeviceDescriptor_Read
    DeviceDescriptorResponse = 0x340,    ///< A_DeviceDescriptor_Response
    
    // Restart service.  The request's restart_type lives in bit 0 of the APCI's
    // data field (03/03/07 §3.4.2.2): 0x380 = basic restart with no payload,
    // 0x381 = master reset carrying erase code + channel number.  Both mask to
    // the same service group, so the type travels in ADataFrame::apciData.
    Restart = 0x380,               ///< A_Restart
    RestartResponse = 0x3A1,       ///< A_Restart_Response (error code + process time)
    
    // Function Property services (03/03/07 §3.4.5).  Note these live below the
    // Escape block, in the 0x2Cx range, and carry the full 10-bit APCI.
    FunctionPropertyCommand = 0x2C7,       ///< A_FunctionPropertyCommand
    FunctionPropertyStateRead = 0x2C8,     ///< A_FunctionPropertyState_Read
    FunctionPropertyStateResponse = 0x2C9, ///< A_FunctionPropertyState_Response

    // Escape code for extended services
    Escape = 0x3C0,                ///< Extended APCI

    // Property services (via Escape)
    PropertyValueRead = 0x3D5,           ///< A_PropertyValue_Read
    PropertyValueResponse = 0x3D6,       ///< A_PropertyValue_Response
    PropertyValueWrite = 0x3D7,          ///< A_PropertyValue_Write
    PropertyDescriptionRead = 0x3D8,     ///< A_PropertyDescription_Read
    PropertyDescriptionResponse = 0x3D9, ///< A_PropertyDescription_Response

    // Authorization services
    AuthorizeRequest = 0x3D1,      ///< A_Authorize_Request
    AuthorizeResponse = 0x3D2,     ///< A_Authorize_Response

    // Key services
    KeyWrite = 0x3D3,              ///< A_Key_Write
    KeyResponse = 0x3D4,           ///< A_Key_Response

    // Network parameter services (03/03/07 Table 1)
    NetworkParameterRead = 0x3DA,      ///< A_NetworkParameter_Read
    NetworkParameterResponse = 0x3DB,  ///< A_NetworkParameter_Response
    NetworkParameterWrite = 0x3E4,     ///< A_NetworkParameter_Write

    // Serial-number-addressed commissioning (03/03/07 Table 1)
    IndividualAddressSerialNumberRead = 0x3DC,     ///< A_IndividualAddressSerialNumber_Read
    IndividualAddressSerialNumberResponse = 0x3DD, ///< A_IndividualAddressSerialNumber_Response
    IndividualAddressSerialNumberWrite = 0x3DE,    ///< A_IndividualAddressSerialNumber_Write

    // System network parameter services (03/03/07 §3.3.8, §3.3.9, Table 1).
    // A third block inside the nominal A_ADC_Response span that carries the
    // full 10-bit APCI.  ETS uses the Read for NM_Read_SerialNumber_By_-
    // ProgrammingMode (03/05/02 §2.17.1.4) — its scan for devices in
    // programming mode — so masking it to the service group makes the device
    // invisible to "Add device" / "Program individual address".
    SystemNetworkParameterRead = 0x1C8,      ///< A_SystemNetworkParameter_Read
    SystemNetworkParameterResponse = 0x1C9,  ///< A_SystemNetworkParameter_Response
    SystemNetworkParameterWrite = 0x1CA,     ///< A_SystemNetworkParameter_Write
};

/**
 * @brief Raw APCI field wrapper
 */
struct APCIField {
    uint16_t raw{0};
    constexpr APCIField() = default;
    constexpr explicit APCIField(uint16_t value) : raw(value) {}
    static constexpr uint16_t Data6Mask = 0x3F;

    /**
     * @brief Create APCI field for service
     *
     * @param service Service code
     * @param data Optional 6-bit data value (for services that encode data in APCI)
     * @return APCI field
     */
    static constexpr APCIField create(APCIService service, uint8_t data = 0) {
        return APCIField(static_cast<uint16_t>(service) | (data & Data6Mask));
    }

    /**
     * @brief Extract APCI service code from APCI field
     *
     * APCI field is 2 bytes with 10-bit service code in upper bits.
     *
     * @return Service code
     */
    constexpr APCIService service() const {
        // Most services are identified by bits 6-9 alone, with bits 0-5 free to
        // carry a short payload.  Two ranges instead use the full 10 bits and
        // therefore must be matched exactly:
        //   - the Escape block (0x3Cx), which is how the property, authorize,
        //     key, network-parameter and serial-number services are encoded;
        //   - the Function Property block (0x2C7..0x2C9), which sits inside the
        //     0x2Cx group and would otherwise be mistaken for it;
        //   - the extended memory block (0x1FB..0x1FE), which shares the 0x1Cx
        //     group with A_ADC_Response and would otherwise be mistaken for it;
        //   - the system network parameter block (0x1C8..0x1CA), in that same
        //     0x1Cx group.
        const uint16_t serviceGroup = raw & 0x03C0;
        if (serviceGroup == static_cast<uint16_t>(APCIService::Escape)) {
            return static_cast<APCIService>(raw & 0x03FF);
        }
        if (isFunctionProperty() || isMemoryExtended() || isPropertyExtended()
            || isSystemNetworkParameter()) {
            return static_cast<APCIService>(raw & 0x03FF);
        }
        return static_cast<APCIService>(serviceGroup);
    }

    /**
     * @brief True for the four extended memory services (0x1FB..0x1FE).
     *
     * These must be matched on all 10 bits: masking to the service group would
     * yield 0x1C0 (A_ADC_Response), so without this the device would answer
     * ETS's key-table download as if it were an ADC reply.
     */
    constexpr bool isMemoryExtended() const {
        const uint16_t full = raw & 0x03FF;
        return full >= static_cast<uint16_t>(APCIService::MemoryExtendedWrite)
            && full <= static_cast<uint16_t>(APCIService::MemoryExtendedReadResponse);
    }

    /**
     * @brief True for the eleven extended property services (0x1CC..0x1D6).
     *
     * Same reasoning as isMemoryExtended(): the block sits inside the nominal
     * A_ADC_Response span and uses all ten APCI bits.
     */
    /**
     * @brief True for the three system network parameter services (0x1C8..0x1CA).
     *
     * Same reasoning as isMemoryExtended(): masking to the service group would
     * yield 0x1C0 (A_ADC_Response), so ETS's programming-mode scan would be
     * dispatched as an ADC reply and silently ignored.
     */
    constexpr bool isSystemNetworkParameter() const {
        const uint16_t full = raw & 0x03FF;
        return full >= static_cast<uint16_t>(APCIService::SystemNetworkParameterRead)
            && full <= static_cast<uint16_t>(APCIService::SystemNetworkParameterWrite);
    }

    constexpr bool isPropertyExtended() const {
        const uint16_t full = raw & 0x03FF;
        return full >= static_cast<uint16_t>(APCIService::PropertyExtValueRead)
            && full <= static_cast<uint16_t>(APCIService::FunctionPropertyExtStateResponse);
    }

    /**
     * @brief True for the three Function Property services (0x2C7..0x2C9).
     */
    constexpr bool isFunctionProperty() const {
        const uint16_t full = raw & 0x03FF;
        return full == static_cast<uint16_t>(APCIService::FunctionPropertyCommand)
            || full == static_cast<uint16_t>(APCIService::FunctionPropertyStateRead)
            || full == static_cast<uint16_t>(APCIService::FunctionPropertyStateResponse);
    }

    /**
     * @brief Check if APCI uses extended format (Escape)
     *
     * @return true if extended service
     */
    constexpr bool isExtended() const {
        return (raw & 0x03C0) == static_cast<uint16_t>(APCIService::Escape);
    }

    /**
     * @brief Extract 6-bit data value from APCI field (short APDU)
     * @return 6-bit data value (0..63)
     */
    constexpr uint8_t data6() const {
        return static_cast<uint8_t>(raw & Data6Mask);
    }
};

/**
 * @brief Device Descriptor Read Request
 */
struct DeviceDescriptorReadRequest {
    uint8_t descriptorType;  ///< Descriptor type (0 or 2)
};

/**
 * @brief Device Descriptor Response
 */
struct DeviceDescriptorResponse {
    uint8_t descriptorType;                              ///< Descriptor type
    util::FixedVector<uint8_t, 16> descriptorData;       ///< Encoded descriptor (max 14 bytes for type 2)
};

/**
 * @brief Memory Read Request
 */
struct MemoryReadRequest {
    uint8_t count;      ///< Number of bytes to read (1-63)
    MemoryAddress address;   ///< Memory address
};

/**
 * @brief Memory Response
 */
struct MemoryResponse {
    using DataBuffer = MemoryServiceDataBuffer;

    uint8_t count{};                   ///< Number of bytes
    MemoryAddress address{};           ///< Memory address
    DataBuffer data;                   ///< Memory data
};

/**
 * @brief Memory Write Request
 */
struct MemoryWriteRequest {
    using DataBuffer = MemoryServiceDataBuffer;

    uint8_t count{};                   ///< Number of bytes
    MemoryAddress address{};           ///< Memory address
    DataBuffer data;                   ///< Data to write
};

/**
 * @brief 24-bit memory address used by the extended memory services.
 *
 * Deliberately separate from MemoryAddress: widening that 16-bit type would
 * change every classic A_Memory_* encode/decode path, and the two address
 * widths are wire-visible.  narrow() bridges to the 16-bit address space the
 * device's memory regions actually live in.
 */
struct ExtendedMemoryAddress {
    static constexpr uint32_t kMaxValue = 0x00FFFFFFu;

    uint32_t raw{0};

    constexpr ExtendedMemoryAddress() = default;
    constexpr explicit ExtendedMemoryAddress(uint32_t value) : raw(value & kMaxValue) {}
    constexpr explicit ExtendedMemoryAddress(MemoryAddress address) : raw(address.raw) {}

    /// True when the address is reachable through the 16-bit region map.
    constexpr bool fitsInMemoryAddress() const { return raw <= 0xFFFFu; }
    constexpr MemoryAddress narrow() const { return MemoryAddress(static_cast<uint16_t>(raw)); }

    constexpr bool operator==(const ExtendedMemoryAddress& other) const { return raw == other.raw; }
    constexpr bool operator!=(const ExtendedMemoryAddress& other) const { return raw != other.raw; }
};

/**
 * @brief The KNX Error Code Set shared by every extended service.
 *
 * One enum, because the specification defines one set: 03/03/07 v02.01.01
 * §3.4.5.5 "Return Codes" for the extended property services, and Tables 3 and
 * 4 of §3.4.9 for extended memory, which are subsets of it with identical
 * values.
 *
 * The negative codes live in the generic range E0h..FEh — they are *not* small
 * integers, which is the easy mistake.  Profiles v02.01.01 §9.1.2.3.3 NOTE 5
 * imposes no minimal subset, so a device may use as few as it can distinguish,
 * but the ones it does emit have to be these values.
 *
 * Note the three access codes are distinct: AccessWriteOnly and AccessReadOnly
 * report which direction the resource refuses, while AccessDenied is
 * specifically authorisation (A_Authorize / KNX Security).
 */
enum class KnxReturnCode : uint8_t {
    Success = 0x00,                     ///< E_SUCCESS
    MemoryError = 0xF1,                 ///< E_MEMORY_ERROR
    ExceedsMaxApduLength = 0xF4,        ///< E_LENGTH_EXCEEDS_MAX_APDU_LENGTH
    DataOverflow = 0xF5,                ///< E_DATA_OVERFLOW (write beyond the resource)
    DataMin = 0xF6,                     ///< E_DATA_MIN (write value too low)
    DataMax = 0xF7,                     ///< E_DATA_MAX (write value too high)
    DataVoid = 0xF8,                    ///< E_DATA_VOID (supported service, invalid data)
    TemporarilyNotAvailable = 0xF9,     ///< E_TEMPORARILY_NOT_AVAILABLE
    AccessWriteOnly = 0xFA,             ///< E_ACCESS_WRITE_ONLY (read of a write-only resource)
    AccessReadOnly = 0xFB,              ///< E_ACCESS_READ_ONLY (write to a read-only resource)
    AccessDenied = 0xFC,                ///< E_ACCESS_DENIED (authorisation)
    AddressVoid = 0xFD,                 ///< E_ADDRESS_VOID (object/property absent, index out of range)
    DataTypeConflict = 0xFE,            ///< E_DATA_TYPE_CONFLICT (wrong datatype / not PDT_FUNCTION)
    GenericError = 0xFF,                ///< E_ERROR
};

/// The extended memory services draw from the same set (§3.4.9 Tables 3 and 4).
using MemoryExtendedReturnCode = KnxReturnCode;

/**
 * @brief A_MemoryExtended_Read / A_MemoryExtended_Write request.
 */
struct MemoryExtendedRequest {
    using DataBuffer = PropertyServiceDataBuffer;

    uint8_t count{};                    ///< Number of octets addressed
    ExtendedMemoryAddress address{};    ///< 24-bit start address
    DataBuffer data;                    ///< Payload (writes only)
};

/// Which extended memory request an answer belongs to.  Carried explicitly
/// because a *failed* read also has no data, so payload shape cannot be used to
/// tell a read response from a write response.
enum class MemoryExtendedResponseKind : uint8_t {
    Read,
    Write,
};

/**
 * @brief A_MemoryExtended_Read_Response / A_MemoryExtended_Write_Response.
 *
 * The address is echoed back so the tool can correlate the answer, and the
 * return code precedes it — the opposite layout to the classic services.
 */
struct MemoryExtendedResponse {
    using DataBuffer = PropertyServiceDataBuffer;

    MemoryExtendedResponseKind kind{MemoryExtendedResponseKind::Read};
    MemoryExtendedReturnCode returnCode{MemoryExtendedReturnCode::Success};
    ExtendedMemoryAddress address{};
    DataBuffer data;                    ///< Read data; empty on writes and on failures
};

/**
 * @brief The 5-octet addressing header every extended property service carries.
 *
 * 03/03/07 v02.01.01 Figures 44/45 and 49-54 and 62-64, all identical:
 *
 *   octet 8..9   interface_object_type (16 bit)
 *   octet 10     object_instance high 8 bits
 *   octet 11     object_instance low 4 bits | property_id high 4 bits
 *   octet 12     property_id low 8 bits
 *
 * So object_instance and property_id are *12 bits each*, straddling octet 11.
 * The classic services use an 8-bit object index and 8-bit property ID, which
 * is why these cannot simply reuse the same decode path.
 */
struct PropertyExtHeader {
    static constexpr size_t kEncodedLength = 5;

    uint16_t objectType{};
    uint16_t objectInstance{};  ///< 12 bit, 1-based per KNX
    uint16_t propertyId{};      ///< 12 bit

    constexpr bool fieldsInRange() const {
        return objectInstance <= 0x0FFFu && propertyId <= 0x0FFFu;
    }

    /// Decode from the service payload (octet 8 onwards).
    static constexpr PropertyExtHeader decode(std::span<const uint8_t> data) {
        PropertyExtHeader header{};
        header.objectType = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
        header.objectInstance =
            static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 4) | ((data[3] >> 4) & 0x0Fu));
        header.propertyId =
            static_cast<uint16_t>((static_cast<uint16_t>(data[3] & 0x0Fu) << 8) | data[4]);
        return header;
    }

    /// Encode into the service payload (octet 8 onwards); needs kEncodedLength octets.
    constexpr void encode(std::span<uint8_t> out) const {
        out[0] = static_cast<uint8_t>((objectType >> 8) & 0xFF);
        out[1] = static_cast<uint8_t>(objectType & 0xFF);
        out[2] = static_cast<uint8_t>((objectInstance >> 4) & 0xFF);
        out[3] = static_cast<uint8_t>(((objectInstance & 0x0Fu) << 4)
                                      | ((propertyId >> 8) & 0x0Fu));
        out[4] = static_cast<uint8_t>(propertyId & 0xFF);
    }
};

/**
 * @brief A_PropertyExtValue_Read / _WriteCon / _WriteUnCon / _InfoReport request.
 *
 * Follows the header with nr_of_elem (1 octet) and start_index (2 octets); the
 * write flavours then carry the data.  Note start_index is 16 bits here, where
 * the classic service packs 4-bit count and 12-bit index into two octets.
 */
struct PropertyExtValueRequest {
    using DataBuffer = PropertyServiceDataBuffer;

    PropertyExtHeader header{};
    uint8_t elementCount{};
    uint16_t startIndex{};
    DataBuffer data;
};

/**
 * @brief A_PropertyExtValue_Response / _WriteConRes.
 *
 * On failure §3.4.5.1 requires elementCount = 0 and the request's startIndex
 * echoed back, with the return code in the trailing field.
 */
struct PropertyExtValueResponse {
    using DataBuffer = PropertyServiceDataBuffer;

    PropertyExtHeader header{};
    uint8_t elementCount{};
    uint16_t startIndex{};
    KnxReturnCode returnCode{KnxReturnCode::Success};
    DataBuffer data;
};

/**
 * @brief A_PropertyExtDescription_Read request (Figure 44).
 *
 * descriptionType is 4 bits and propertyIndex 12 bits, packed into two octets
 * after the header.  Only description type zero is currently defined.
 */
struct PropertyExtDescriptionRequest {
    PropertyExtHeader header{};
    uint8_t descriptionType{};
    uint16_t propertyIndex{};
};

/**
 * @brief A_PropertyExtDescription_Response, description type zero (Figure 45).
 */
struct PropertyExtDescriptionResponse {
    PropertyExtHeader header{};
    uint8_t descriptionType{};
    uint16_t propertyIndex{};
    uint16_t dptMain{};
    uint16_t dptSub{};
    bool writeEnabled{false};
    uint8_t propertyDataType{};  ///< 6 bit PDT
    uint16_t maxElements{};
    uint8_t readLevel{};         ///< 4 bit
    uint8_t writeLevel{};        ///< 4 bit
};

/**
 * @brief A_FunctionPropertyExtCommand / _State_Read request (Figures 62, 63).
 */
struct FunctionPropertyExtRequest {
    using DataBuffer = PropertyServiceDataBuffer;

    PropertyExtHeader header{};
    DataBuffer data;
    /// How the request was secured. The Security Interface Object's function
    /// properties are reached through this service too, and their Access
    /// Policies do not care which of the two encodings ETS chose.
    RequestSecurity security{};
};

/**
 * @brief A_FunctionPropertyExtState_Response (Figure 64).
 *
 * Return code sits between the header and the data, unlike the value services
 * where it trails.
 */
struct FunctionPropertyExtResponse {
    using DataBuffer = PropertyServiceDataBuffer;

    PropertyExtHeader header{};
    KnxReturnCode returnCode{KnxReturnCode::Success};
    DataBuffer data;
};

/**
 * @brief Property Value Read Request
 */
struct PropertyValueReadRequest {
    InterfaceObjectIndex objectIndex{};  ///< Interface object index
    PropertyID propertyId{};             ///< Property ID
    uint8_t elementCount{};              ///< Number of elements
    uint16_t startIndex{};               ///< Start index
};

/**
 * @brief Property Value Response
 */
struct PropertyValueResponse {
    using DataBuffer = PropertyServiceDataBuffer;

    InterfaceObjectIndex objectIndex{};  ///< Interface object index
    PropertyID propertyId{};             ///< Property ID
    uint8_t elementCount{};              ///< Number of elements
    uint16_t startIndex{};               ///< Start index
    DataBuffer data;                     ///< Property value data
};

/**
 * @brief Property Value Write Request
 */
struct PropertyValueWriteRequest {
    using DataBuffer = PropertyServiceDataBuffer;

    InterfaceObjectIndex objectIndex{};  ///< Interface object index
    PropertyID propertyId{};             ///< Property ID
    uint8_t elementCount{};              ///< Number of elements
    uint16_t startIndex{};               ///< Start index
    DataBuffer data;                     ///< Property value data
};

} // namespace application
} // namespace knx
