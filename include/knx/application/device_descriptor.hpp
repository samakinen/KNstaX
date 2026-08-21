// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file device_descriptor.hpp
 * @brief KNX Device Descriptor definitions
 * 
 * Device descriptors provide information about device capabilities and features.
 * They are read by ETS and other configuration tools to identify device type.
 * 
 * Per KNX spec 03/05/01 (Resources):
 * - Descriptor Type 0: Medium type and firmware version
 * - Descriptor Type 2: Extended device information (System 7)
 */

#pragma once

#include <cstdint>
#include <array>
#include "knx/types.hpp"

namespace knx {
namespace application {

/**
 * @brief Device Descriptor Type
 */
enum class DescriptorType : uint8_t {
    Type0 = 0,  ///< Basic descriptor (legacy, rarely used)
    Type2 = 2   ///< Extended descriptor (System 7 standard)
};

/**
 * @brief The medium's code, as KNX numbers media everywhere it names one.
 *
 * It is the high nibble of mask version byte 0 (03/05/01 §4.1.2, Table 7) and
 * it is MediumType/@Number in the ETS master data — the same four values in
 * both places. Keeping one table means the mask version a device reports and
 * the medium its catalogue entry declares cannot drift apart, which matters
 * because ETS compares them before it will download anything.
 *
 * Not to be confused with `MediumType`'s own values: those are the PID_MEDIUM_TYPE
 * bit mask (TP1 = 02h, IP = 20h), a different encoding of the same fact.
 */
constexpr uint8_t mediumCode(knx::MediumType medium) noexcept
{
    switch (medium) {
        case knx::MediumType::TP1:   return 0x0;
        case knx::MediumType::PL110: return 0x1;
        case knx::MediumType::RF:    return 0x2;
        case knx::MediumType::IP:    return 0x5;
    }
    return 0x0;
}

/// The medium's name in the ETS master data ("TP", "PL", "RF", "IP").
constexpr const char* mediumName(knx::MediumType medium) noexcept
{
    switch (medium) {
        case knx::MediumType::TP1:   return "TP";
        case knx::MediumType::PL110: return "PL";
        case knx::MediumType::RF:    return "RF";
        case knx::MediumType::IP:    return "IP";
    }
    return "TP";
}

/// Mask version 07B0h with the medium nibble cleared: firmware type 7,
/// version B, subcode 0 — the System B realisation this stack implements.
inline constexpr uint16_t kSystemBMaskBase = 0x07B0;

/**
 * @brief This stack's mask version on a given medium: 07B0h on TP1, 57B0h on KNX IP.
 *
 * Only the medium nibble moves. The firmware type and version identify the
 * realisation, not the wire it sits on, and this stack is System B on every
 * medium it supports.
 */
constexpr uint16_t systemBMaskVersion(knx::MediumType medium) noexcept
{
    return static_cast<uint16_t>((static_cast<uint16_t>(mediumCode(medium)) << 12)
                                 | (kSystemBMaskBase & 0x0FFFu));
}

static_assert(systemBMaskVersion(knx::MediumType::TP1) == 0x07B0,
              "System B on TP1 is 07B0h (03/05/01 Table 7)");
static_assert(systemBMaskVersion(knx::MediumType::IP) == 0x57B0,
              "System B on KNX IP is 57B0h (03/05/01 Table 7)");

/**
 * @brief Device Descriptor Type 0 (KNX Mask Version)
 *
 * 2-byte mask version as defined in KNX spec 03_05_01 section 4.1.2:
 *   Byte 0: {MediumType[3:0], FirmwareType[3:0]}
 *   Byte 1: {Version[3:0], Subcode[3:0]}
 *
 * Well-known values (Table 7):
 *   0x07B0 = System B, TP1
 *   0x0010 = BCU 1, TP1
 *   0x0020 = BCU 2, TP1
 */
struct DeviceDescriptorType0 {
    uint8_t maskHigh;  ///< Upper byte of mask version
    uint8_t maskLow;   ///< Lower byte of mask version

    std::array<uint8_t, 2> encode() const {
        return {maskHigh, maskLow};
    }

    static DeviceDescriptorType0 decode(const std::array<uint8_t, 2>& data) {
        return {data[0], data[1]};
    }
};

/**
 * @brief Device Descriptor Type 2
 * 
 * Extended descriptor format - 14 bytes:
 * - Bytes 0-1: Medium type and firmware version
 * - Bytes 2-3: Device type (manufacturer-specific)
 * - Bytes 4-5: Version (application ID)
 * - Bytes 6-7: Link management procedures
 * - Bytes 8-13: Reserved/manufacturer-specific
 */
struct DeviceDescriptorType2 {
    knx::MediumType mediumType;           ///< Physical medium
    uint8_t firmwareVersion;         ///< Firmware version
    DeviceType deviceType;            ///< Manufacturer device type
    uint16_t applicationVersion;     ///< Application version/ID
    uint16_t linkMgmtProcedures;     ///< Supported link procedures
    std::array<uint8_t, 6> reserved; ///< Reserved bytes
    
    /**
     * @brief Encode to 14-byte format
     * @return Encoded descriptor
     */
    std::array<uint8_t, 14> encode() const {
        std::array<uint8_t, 14> data;
        data[0] = static_cast<uint8_t>(mediumType);
        data[1] = firmwareVersion;
        data[2] = static_cast<uint8_t>((deviceType.value() >> 8) & 0xFFu);
        data[3] = static_cast<uint8_t>(deviceType.value() & 0xFFu);
        data[4] = static_cast<uint8_t>((applicationVersion >> 8) & 0xFFu);
        data[5] = static_cast<uint8_t>(applicationVersion & 0xFFu);
        data[6] = static_cast<uint8_t>((linkMgmtProcedures >> 8) & 0xFFu);
        data[7] = static_cast<uint8_t>(linkMgmtProcedures & 0xFFu);
        std::copy(reserved.begin(), reserved.end(), data.begin() + 8);
        return data;
    }
    
    /**
     * @brief Decode from 14-byte format
     * @param data Encoded descriptor (must be 14 bytes)
     * @return Decoded descriptor
     */
    static DeviceDescriptorType2 decode(const std::array<uint8_t, 14>& data) {
        DeviceDescriptorType2 desc;
        desc.mediumType = static_cast<knx::MediumType>(data[0]);
        desc.firmwareVersion = data[1];
        desc.deviceType = DeviceType(static_cast<uint16_t>((data[2] << 8) | data[3]));
        desc.applicationVersion = (static_cast<uint16_t>(data[4]) << 8) | data[5];
        desc.linkMgmtProcedures = (static_cast<uint16_t>(data[6]) << 8) | data[7];
        std::copy(data.begin() + 8, data.end(), desc.reserved.begin());
        return desc;
    }
};

/**
 * @brief Device Descriptor container
 * 
 * Holds both Type 0 and Type 2 descriptors for a device.
 */
struct DeviceDescriptor {
    DeviceDescriptorType0 type0;
    DeviceDescriptorType2 type2;
    
    /**
     * @brief Create default descriptor for TP1 device
     */
    static DeviceDescriptor createDefault() {
        DeviceDescriptor desc;
        // Mask version 0x07B0 = System B, TP1 (KNX spec 03_05_01 Table 7)
        desc.type0.maskHigh = 0x07;
        desc.type0.maskLow  = 0xB0;
        
        desc.type2.mediumType = knx::MediumType::TP1;
        desc.type2.firmwareVersion = 1;
        desc.type2.deviceType = DeviceType(0x0000);  // Generic device
        desc.type2.applicationVersion = 0x0001;
        desc.type2.linkMgmtProcedures = 0x0000;
        desc.type2.reserved.fill(0);
        
        return desc;
    }
};

} // namespace application
} // namespace knx
