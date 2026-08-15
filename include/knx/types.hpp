// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file types.hpp
 * @brief Common KNX type definitions
 * 
 * This file contains fundamental types used throughout the KNX stack,
 * following KNX System Specifications.
 */

#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <cstddef>
#include <array>
#include <string_view>
#include <cstring>
#include <cstdio>
#include <type_traits>
#include "knx/util/result.hpp"

namespace knx {

template <typename T>
concept StrongValueStorage = std::integral<T>;

template <typename Tag, StrongValueStorage T, T DefaultValue, bool HasInvalid = false, T InvalidValue = DefaultValue>
struct StrongValue {
    using value_type = T;

    T raw{DefaultValue};

    constexpr StrongValue() = default;
    constexpr explicit StrongValue(T value) : raw(value) {}

    constexpr T value() const { return raw; }

    constexpr bool operator==(const StrongValue& other) const = default;
    constexpr auto operator<=>(const StrongValue& other) const = default;

    constexpr bool isValid() const requires (HasInvalid) { return raw != InvalidValue; }

    static constexpr Tag invalid() requires (HasInvalid) { return Tag(InvalidValue); }

    constexpr bool isValid() const requires (!HasInvalid) = delete;
    static constexpr Tag invalid() requires (!HasInvalid) = delete;
};

// Simple enable/disable toggle for public APIs (avoid boolean flags).
enum class Toggle : uint8_t {
    Disable = 0,
    Enable = 1
};

constexpr bool isEnabled(Toggle toggle) {
    return toggle == Toggle::Enable;
}

// Entry enabled/disabled state for tables and rule sets.
enum class EntryState : uint8_t {
    Disabled = 0,
    Enabled = 1
};

constexpr bool isEnabled(EntryState state) {
    return state == EntryState::Enabled;
}

// Write permission for properties (avoid bool flags in APIs).
enum class PropertyWriteAccess : uint8_t {
    Denied = 0,
    Allowed = 1
};

constexpr bool isWriteAllowed(PropertyWriteAccess access) {
    return access == PropertyWriteAccess::Allowed;
}

// Manufacturer identifier (KNX manufacturer code).
struct ManufacturerId : StrongValue<ManufacturerId, uint16_t, 0> {
    using StrongValue::StrongValue;
};

// Manufacturer device type identifier.
struct DeviceType : StrongValue<DeviceType, uint16_t, 0> {
    using StrongValue::StrongValue;
};

// Group object index in the group object table.
struct GroupObjectIndex : StrongValue<GroupObjectIndex, uint16_t, 0xFFFF, true, 0xFFFF> {
    using StrongValue::StrongValue;
};

// Address table index (1-based; 0 means invalid/not found).
struct AddressTableIndex : StrongValue<AddressTableIndex, uint16_t, 0, true, 0> {
    using StrongValue::StrongValue;
};

// Interface object type identifier (includes custom/manufacturer types).
// Per KNX spec 03_07_03 Table 1 - System Interface Object Types
struct InterfaceObjectType : StrongValue<InterfaceObjectType, uint16_t, 0> {
    using StrongValue::StrongValue;

    // Standard KNX System Interface Object Types (0-99) per Table 1
    static constexpr InterfaceObjectType device() { return InterfaceObjectType(0); }
    static constexpr InterfaceObjectType addressTable() { return InterfaceObjectType(1); }
    static constexpr InterfaceObjectType associationTable() { return InterfaceObjectType(2); }
    static constexpr InterfaceObjectType applicationProgram() { return InterfaceObjectType(3); }
    static constexpr InterfaceObjectType interfaceProgram() { return InterfaceObjectType(4); }
    static constexpr InterfaceObjectType eibObjectAssociation() { return InterfaceObjectType(5); }
    static constexpr InterfaceObjectType router() { return InterfaceObjectType(6); }
    static constexpr InterfaceObjectType lteFsm() { return InterfaceObjectType(7); }
    static constexpr InterfaceObjectType cemiServer() { return InterfaceObjectType(8); }
    static constexpr InterfaceObjectType groupObjectTable() { return InterfaceObjectType(9); }
    static constexpr InterfaceObjectType pollingMaster() { return InterfaceObjectType(10); }
    static constexpr InterfaceObjectType knxNetIpParameter() { return InterfaceObjectType(11); }
    // 12 is reserved per KNX spec
    static constexpr InterfaceObjectType fileServer() { return InterfaceObjectType(13); }
    static constexpr InterfaceObjectType eModeChannel() { return InterfaceObjectType(0x000E); }
    static constexpr InterfaceObjectType adjustedEModeChannel() { return InterfaceObjectType(0x000F); }
    static constexpr InterfaceObjectType textCatalogue() { return InterfaceObjectType(0x0010); }
    static constexpr InterfaceObjectType eModeDevice() { return InterfaceObjectType(0x0012); }
    
    // Security/RF system interface object types per newer KNX specs
    static constexpr InterfaceObjectType security() { return InterfaceObjectType(0x0011); }
    static constexpr InterfaceObjectType rfMedium() { return InterfaceObjectType(0x0013); }
};

// Interface object index (0-255).
struct InterfaceObjectIndex : StrongValue<InterfaceObjectIndex, uint8_t, 0> {
    using StrongValue::StrongValue;
};

// Interface object instance (1-based; 0 may mean "all" in some services).
struct InterfaceObjectInstance : StrongValue<InterfaceObjectInstance, uint8_t, 0> {
    using StrongValue::StrongValue;
};

// Property index (0 means "by ID" in property description).
struct PropertyIndex : StrongValue<PropertyIndex, uint8_t, 0> {
    using StrongValue::StrongValue;
};

// Connection table index (0-15, 0xFF invalid).
struct ConnectionIndex : StrongValue<ConnectionIndex, uint8_t, 0xFF, true, 0xFF> {
    using StrongValue::StrongValue;
};

// KNXnet/IP tunnelling channel id (0 invalid).
struct ChannelId : StrongValue<ChannelId, uint8_t, 0, true, 0> {
    using StrongValue::StrongValue;
};

// KNXnet/IP UDP/TCP port (0 invalid).
struct NetIpPort : StrongValue<NetIpPort, uint16_t, 0, true, 0> {
    using StrongValue::StrongValue;
};

// KNXnet/IP service type (16-bit).
struct NetIpServiceType : StrongValue<NetIpServiceType, uint16_t, 0> {
    using StrongValue::StrongValue;
};

// KNXnet/IP tunnelling sequence number.
struct TunnelingSequence : StrongValue<TunnelingSequence, uint8_t, 0> {
    using StrongValue::StrongValue;
};

// KNX/IP Secure user id (1..255 typically).
struct UserId : StrongValue<UserId, uint8_t, 1> {
    using StrongValue::StrongValue;
};

// KNX/IP Secure session id (0 invalid).
struct SessionId : StrongValue<SessionId, uint16_t, 0, true, 0> {
    using StrongValue::StrongValue;
};

// Access type for read/write operations in public APIs.
enum class AccessType : uint8_t {
    Read = 0,
    Write = 1
};

// Address classification for APIs that need explicit group vs individual.
enum class AddressType : uint8_t {
    Individual = 0,
    Group = 1
};

constexpr bool isGroupAddress(AddressType type) {
    return type == AddressType::Group;
}

// ============================================================================
// Address Types
// ============================================================================

template <typename Derived>
struct Address16Base {
    uint16_t raw;

    constexpr Address16Base() : raw(Derived::invalidValue()) {}
    constexpr explicit Address16Base(uint16_t addr) : raw(addr) {}

    constexpr uint16_t value() const { return raw; }
    constexpr bool isValid() const { return Derived::isValidRaw(raw); }

    util::Result<void> setAddress(uint16_t addr) {
        if (!Derived::isValidRaw(addr)) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        raw = addr;
        return util::Result<void>::ok();
    }

    util::Result<void> setAddress(uint8_t part0, uint8_t part1, uint8_t part2) {
        if (!Derived::validateParts3(part0, part1, part2)) {
            return util::Result<void>::err(util::ErrorCode::InvalidParameter);
        }
        return setAddress(Derived::encode3Parts(part0, part1, part2));
    }

    util::Result<void> setAddress(uint8_t part0, uint16_t part2) {
        if constexpr (Derived::supports2Parts) {
            if (!Derived::validateParts2(part0, part2)) {
                return util::Result<void>::err(util::ErrorCode::InvalidParameter);
            }
            return setAddress(Derived::encode2Parts(part0, part2));
        }
        return util::Result<void>::err(util::ErrorCode::InvalidParameter);
    }

    bool operator==(const Derived& other) const { return raw == other.raw; }
    bool operator!=(const Derived& other) const { return raw != other.raw; }
};

/**
 * @brief KNX Individual Address (Physical Address)
 * Format: Part0.Part1.Part2 (4.4.8 bits)
 * Convenience: Part0.Part2 (8.8 bits)
 * Operational range: 0x0001 to 0xFFFE (0xFFFF is the
 * KNX initial commissioning-state marker and 0x0000 is invalid/broadcast)
 */
struct IndividualAddress : Address16Base<IndividualAddress> {
    using Address16Base::Address16Base;

    constexpr IndividualAddress()
        : Address16Base(0x0000u) {}

    // 2-part: Part0.Part2 (8.8 bits)
    constexpr IndividualAddress(uint8_t part0, uint8_t part2)
        : Address16Base(encode2Parts(part0, part2)) {}

    constexpr IndividualAddress(uint8_t part0, uint8_t part1, uint8_t part2)
        : Address16Base(encode3Parts(part0, part1, part2)) {}

    static constexpr uint16_t invalidValue() { return 0x0000; }
    static constexpr bool isValidRaw(uint16_t addr) { return addr != 0x0000 && addr != 0xFFFF; }
    static constexpr bool supports2Parts = true;
    static constexpr bool validateParts2(uint8_t /*part0*/, uint16_t part2) {
        return part2 <= 0x00FF;
    }
    static constexpr bool validateParts3(uint8_t part0, uint8_t part1, uint8_t /*part2*/) {
        return part0 <= 0x0F && part1 <= 0x0F;
    }

    uint8_t part0() const { return static_cast<uint8_t>((raw >> 12) & 0x0F); }
    uint8_t part1() const { return static_cast<uint8_t>((raw >> 8) & 0x0F); }
    uint8_t part2() const { return static_cast<uint8_t>(raw & 0xFF); }

    uint8_t area() const { return part0(); }
    uint8_t line() const { return part1(); }
    uint8_t device() const { return part2(); }
    static constexpr uint16_t encode2Parts(uint8_t part0, uint8_t part2) {
        return (static_cast<uint16_t>(part0) << 8) | part2;
    }

    static constexpr uint16_t encode3Parts(uint8_t part0, uint8_t part1, uint8_t part2) {
        return (static_cast<uint16_t>(part0 & 0x0F) << 12) |
               (static_cast<uint16_t>(part1 & 0x0F) << 8) |
               part2;
    }
};

// KNX initial device address used before a unique individual address has been
// assigned. This is a commissioning-state marker, not a normal operational
// destination address for generic individually addressed runtime traffic.
constexpr IndividualAddress initialIndividualAddress()
{
    return IndividualAddress(0xFFFFu);
}

constexpr bool isInitialIndividualAddress(const IndividualAddress& address)
{
    return address.raw == 0xFFFFu;
}

constexpr bool isIndividualBroadcastAddress(const IndividualAddress& address)
{
    return address.raw == 0x0000u;
}

enum class IndividualAddressKind : uint8_t {
    Invalid,
    Initial,
    Operational,
};

constexpr IndividualAddressKind classifyIndividualAddress(const IndividualAddress& address)
{
    if (isInitialIndividualAddress(address)) {
        return IndividualAddressKind::Initial;
    }

    // 0.0.0 is the reserved/unassigned marker, never an operational device
    // address (matches IndividualAddress::isValidRaw).
    if (address.raw == 0x0000u) {
        return IndividualAddressKind::Invalid;
    }

    return IndividualAddressKind::Operational;
}

constexpr bool isOperationalIndividualAddress(const IndividualAddress& address)
{
    return classifyIndividualAddress(address) == IndividualAddressKind::Operational;
}

/**
 * @brief KNX Group Address
 * Supports both 2-part and 3-part formats
 * Valid range: 0x0001 to 0xFFFF (0x0000 is reserved for default/none)
 */
struct GroupAddress : Address16Base<GroupAddress> {
    using Address16Base::Address16Base;

    // 2-part: Part0.Part2 (5.11 bits)
    constexpr GroupAddress(uint8_t part0, uint16_t part2)
        : Address16Base(encode2Parts(part0, part2)) {}

    // 3-part: Part0.Part1.Part2 (5.3.8 bits)
    constexpr GroupAddress(uint8_t part0, uint8_t part1, uint8_t part2)
        : Address16Base(encode3Parts(part0, part1, part2)) {}

    static constexpr uint16_t invalidValue() { return 0x0000; }
    static constexpr bool isValidRaw(uint16_t addr) { return addr != 0x0000; }
    static constexpr bool supports2Parts = true;
    static constexpr bool validateParts2(uint8_t part0, uint16_t part2) {
        return part0 <= 0x1F && part2 <= 0x07FF;
    }
    static constexpr bool validateParts3(uint8_t part0, uint8_t part1, uint8_t /*part2*/) {
        return part0 <= 0x1F && part1 <= 0x07;
    }

    uint8_t part0() const { return static_cast<uint8_t>((raw >> 11) & 0x1F); }
    uint8_t part1() const { return static_cast<uint8_t>((raw >> 8) & 0x07); }
    uint8_t part2() const { return static_cast<uint8_t>(raw & 0xFF); }

    uint8_t main() const { return part0(); }
    uint8_t middle() const { return part1(); }
    uint8_t sub() const { return part2(); }
    static constexpr uint16_t encode2Parts(uint8_t part0, uint16_t part2) {
        return (static_cast<uint16_t>(part0 & 0x1F) << 11) | (part2 & 0x07FF);
    }

    static constexpr uint16_t encode3Parts(uint8_t part0, uint8_t part1, uint8_t part2) {
        return (static_cast<uint16_t>(part0 & 0x1F) << 11) |
               (static_cast<uint16_t>(part1 & 0x07) << 8) |
               part2;
    }
};

// Group address 0/0/0 (raw 0x0000) is used for system-broadcast style
// telegrams during commissioning and management procedures.
constexpr bool isGroupBroadcastAddress(const GroupAddress& address)
{
    return address.raw == 0x0000u;
}

// Memory address (16-bit) used by KNX A_Memory_* services.
struct MemoryAddress {
    uint16_t raw{0};

    constexpr MemoryAddress() = default;
    constexpr explicit MemoryAddress(uint16_t value) : raw(value) {}

    constexpr uint16_t value() const { return raw; }

    constexpr bool isZero() const { return raw == 0; }
    constexpr bool isBeforeOrEqual(MemoryAddress other) const { return raw <= other.raw; }
    constexpr uint16_t distanceInclusive(MemoryAddress other) const {
        return raw <= other.raw ? static_cast<uint16_t>(other.raw - raw + 1) : 0;
    }

    constexpr bool operator==(const MemoryAddress& other) const { return raw == other.raw; }
    constexpr bool operator!=(const MemoryAddress& other) const { return raw != other.raw; }
};

// IPv4 address stored in network byte order.
struct IpAddress {
    uint32_t raw{0};

    constexpr IpAddress() = default;
    constexpr explicit IpAddress(uint32_t value) : raw(value) {}

    constexpr uint32_t value() const { return raw; }
    constexpr bool isZero() const { return raw == 0; }

    constexpr bool operator==(const IpAddress& other) const { return raw == other.raw; }
    constexpr bool operator!=(const IpAddress& other) const { return raw != other.raw; }

    static IpAddress fromString(const char* ip) {
        if (!ip) return IpAddress(0);

        // Strict dotted-quad parser without libc networking dependencies.
        uint32_t parts[4] = {0, 0, 0, 0};
        int part = 0;
        const char* p = ip;
        while (*p) {
            if (part >= 4) return IpAddress(0);
            if (*p < '0' || *p > '9') return IpAddress(0);
            uint32_t v = 0;
            int digits = 0;
            while (*p >= '0' && *p <= '9') {
                v = v * 10u + static_cast<uint32_t>(*p - '0');
                if (v > 255u) return IpAddress(0);
                ++p;
                ++digits;
                if (digits > 3) return IpAddress(0);
            }
            parts[part++] = v;
            if (*p == '\0') break;
            if (*p != '.') return IpAddress(0);
            ++p;
        }
        if (part != 4) return IpAddress(0);

        // Return in "network byte order" (big-endian) as expected by socket APIs.
        // We represent this as the 4 bytes copied into a uint32_t so the in-memory
        // layout matches network order regardless of host endianness.
        const uint8_t bytes[4] = {
            static_cast<uint8_t>(parts[0]),
            static_cast<uint8_t>(parts[1]),
            static_cast<uint8_t>(parts[2]),
            static_cast<uint8_t>(parts[3]),
        };
        uint32_t out = 0;
        std::memcpy(&out, bytes, sizeof(out));
        return IpAddress(out);
    }

    static IpAddress fromString(std::string_view ip) {
        if (ip.empty()) return IpAddress(0);
        return fromString(ip.data());
    }

    void toString(char* buffer) const {
        if (!buffer) return;
        uint8_t bytes[4] = {0, 0, 0, 0};
        std::memcpy(bytes, &raw, sizeof(bytes));
        const uint8_t a = bytes[0];
        const uint8_t b = bytes[1];
        const uint8_t c = bytes[2];
        const uint8_t d = bytes[3];
        // Minimal snprintf replacement (still uses snprintf, but this is fine for all targets that already use libc).
        (void)std::snprintf(buffer, 16, "%u.%u.%u.%u", a, b, c, d);
    }

    static IpAddress fromOctets(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        const uint8_t bytes[4] = {a, b, c, d};
        uint32_t out = 0;
        std::memcpy(&out, bytes, sizeof(out));
        return IpAddress(out);
    }
        void toOctets(uint8_t& a, uint8_t& b, uint8_t& c, uint8_t& d) const {
            uint8_t bytes[4] = {0, 0, 0, 0};
            std::memcpy(bytes, &raw, sizeof(bytes));
            a = bytes[0];
            b = bytes[1];
            c = bytes[2];
            d = bytes[3];
        }

        bool isLoopback() const {
            uint8_t a = 0, b = 0, c = 0, d = 0;
            toOctets(a, b, c, d);
            (void)b; (void)c; (void)d;
            return a == 127;
        }
};

// ============================================================================
// Priority
// ============================================================================

// Values are the 2-bit wire encoding shared by the TP1 control field
// (03_02_02 §2.3.2: 00=system, 01=normal, 10=urgent, 11=low) and the cEMI
// control field 1 — both codecs cast the enum value directly.
enum class Priority : uint8_t {
    System = 0,
    Normal = 1,
    Urgent = 2,
    Low = 3
};

// ============================================================================
// Message Codes
// ============================================================================

enum class MessageCode : uint8_t {
    LData = 0x11,
    LRaw = 0x12,
    // Transport Layer
    DataBroadcast = 0x00,
    DataGroup = 0x00,
    DataIndividual = 0x00,
    DataConnected = 0x40,
    
    // Application Layer  
    GroupValueRead = 0x00,
    GroupValueResponse = 0x40,
    GroupValueWrite = 0x80,
    
    IndividualAddressWrite = 0x00,
    IndividualAddressRead = 0x01,
    IndividualAddressResponse = 0x02,
    
    AdcRead = 0x03,
    AdcResponse = 0x04,
    
    MemoryRead = 0x08,
    MemoryResponse = 0x09,
    MemoryWrite = 0x0A,
    
    DeviceDescriptorRead = 0x0C,
    DeviceDescriptorResponse = 0x0D,
    
    Restart = 0x0E,
    
    PropertyValueRead = 0x14,
    PropertyValueResponse = 0x15,
    PropertyValueWrite = 0x16,
    PropertyDescriptionRead = 0x18,
    PropertyDescriptionResponse = 0x19,
    
    FunctionPropertyCommand = 0x1C,
    FunctionPropertyStateRead = 0x1D,
    FunctionPropertyStateResponse = 0x1E
};

// ============================================================================
// Hop Count Type
// ============================================================================

enum class HopCountType : uint8_t {
    NetworkLayerParameter = 0,
    HopCount1 = 1,
    HopCount2 = 2,
    HopCount3 = 3,
    HopCount4 = 4,
    HopCount5 = 5,
    HopCount6 = 6,
    HopCount7 = 7
};

// ============================================================================
// Acknowledgment Types
// ============================================================================

enum class AckType : uint8_t {
    NoAck = 0,
    Ack = 1,
    NAck = 2,
    Busy = 3
};

// ============================================================================
// Security
// ============================================================================

enum class DataSecurity : uint8_t {
    None = 0,
    Auth = 1,
    ConfAuth = 3
};

enum class ToolAccess : uint8_t {
    None = 0,
    Auth = 1,
    ConfAuth = 3
};

struct SecurityControl {
    bool systemBroadcast;
    ToolAccess toolAccess;
    DataSecurity dataSecurity;
    
    constexpr SecurityControl()
        : systemBroadcast(false)
        , toolAccess(ToolAccess::None)
        , dataSecurity(DataSecurity::None) {}
    
    constexpr SecurityControl(Toggle sysBcast, ToolAccess tool, DataSecurity data = DataSecurity::None)
        : systemBroadcast(isEnabled(sysBcast))
        , toolAccess(tool)
        , dataSecurity(data) {}
};

// ============================================================================
// Communication Flags
// ============================================================================

enum class ComFlag : uint8_t {
    Update = 0x01,          // Group object updated
    Transmit = 0x02,        // Group object shall be transmitted
    UpdatedViaGroupTelegram = 0x04,
    TransmitRequest = 0x08,
    ReadRequest = 0x10,
    WriteRequest = 0x20,
    CommunicationEnable = 0x40,
    Init = 0x80
};

inline ComFlag operator|(ComFlag a, ComFlag b) {
    return static_cast<ComFlag>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline ComFlag operator&(ComFlag a, ComFlag b) {
    return static_cast<ComFlag>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline ComFlag& operator|=(ComFlag& a, ComFlag b) {
    a = a | b;
    return a;
}

inline ComFlag& operator&=(ComFlag& a, ComFlag b) {
    a = a & b;
    return a;
}

// ============================================================================
// Restart Types
// ============================================================================

enum class EraseCode : uint8_t {
    Reserved = 0,
    ConfirmedRestart = 1,
    FactoryReset = 2,
    ResetIA = 3,
    ResetAP = 4,
    ResetParam = 5,
    ResetLinks = 6,
    ResetWithoutIA = 7
};

// ============================================================================
// Frame Types
// ============================================================================

enum class FrameFormat : uint8_t {
    Standard = 0,
    Extended = 1
};

// ============================================================================
// Medium Types
// ============================================================================

enum class MediumType : uint8_t {
    TP1 = 0x02,
    PL110 = 0x04,
    RF = 0x10,
    IP = 0x20
};

// ============================================================================
// Security Context of a Received Request
// ============================================================================

/**
 * @brief How an inbound request was protected, as decided by the S-AL.
 *
 * KNX Access Policies (03/4/1 §6.2) are expressed per communication partner
 * *and* per protection: the same service is allowed from the Role "Tool" with
 * authentication and confidentiality and denied from an unlisted plain sender.
 * Enforcing them therefore needs one fact that only the Secure Application
 * Layer knows — how the carrying frame was secured — at the point where the
 * request is finally executed, several layers up.
 *
 * A plain frame carries the default: every field false. That is the safe
 * default for the whole stack, including builds compiled without KNX Secure,
 * where nothing ever sets these.
 */
struct RequestSecurity {
    /// Arrived as an S-A_Data PDU whose MAC verified against a known key.
    bool secured{false};
    /// The SCF Tool Access flag was set: the sender used the Tool Key, which is
    /// what grants the Role "Tool" (03/03/07 §5.2.1.2, Figure 107).
    bool toolAccess{false};
    /// CCM with authentication *and* confidentiality (A+C) rather than
    /// authentication only — the "A+C" column of the Access Policy tables.
    bool confidentiality{false};

    /// The one client the strictest Access Policies (00C/00C, e.g. the key
    /// tables and PID_SECURITY_MODE) admit: Role "Tool", secured with A+C.
    [[nodiscard]] constexpr bool toolSecured() const noexcept {
        return secured && toolAccess && confidentiality;
    }
};

// ============================================================================
// Crypto Types
// ============================================================================

constexpr size_t AES_KEY_SIZE = 16;
constexpr size_t AES_BLOCK_SIZE = 16;
constexpr size_t CCM_MAC_SIZE = 4;
constexpr size_t CCM_NONCE_SIZE = 13;

using AesKey = std::array<uint8_t, AES_KEY_SIZE>;
using Nonce = std::array<uint8_t, CCM_NONCE_SIZE>;

// ============================================================================
// Callback Types
// ============================================================================

/**
 * @brief Callback invoked before a device restart.
 *
 * @note Intentionally defined as a raw function pointer (not std::function)
 *       to allow usage from C-compatible device object implementations and
 *       to avoid dynamic memory allocation on constrained embedded targets.
 */
using BeforeRestartCallback = void (*)(void);

/**
 * @brief Callback used to serialize persistent device state into a buffer.
 *
 * @param buffer Pointer to the output buffer
 * @return Pointer to the first byte after the written data
 *
 * @note Function pointer is used for C compatibility and deterministic
 *       memory behavior. Higher-level protocol callbacks use std::function.
 */
using SaveCallback = uint8_t* (*)(uint8_t* buffer);

/**
 * @brief Callback used to restore persistent device state from a buffer.
 *
 * @param buffer Pointer to the input buffer
 * @return Pointer to the first byte after the consumed data
 *
 * @note Function pointer is used for C compatibility and deterministic
 *       memory behavior. Higher-level protocol callbacks use std::function.
 */
using RestoreCallback = const uint8_t* (*)(const uint8_t* buffer);

} // namespace knx
