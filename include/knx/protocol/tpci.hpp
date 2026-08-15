// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tpci.hpp
 * @brief TPCI (Transport Protocol Control Information) helpers
 */

#pragma once

#include <cstdint>

namespace knx {
namespace protocol {

/**
 * @brief TPCI (Transport Protocol Control Information) codes
 */
enum class TPCI : uint8_t {
    UnnumberedData = 0x00,      ///< Unnumbered data (connectionless)
    NumberedData = 0x40,        ///< Numbered data (connection-oriented)
    UnnumberedControl = 0x80,   ///< Unnumbered control
    NumberedControl = 0xC0,     ///< Numbered control
};

/**
 * @brief TPCI control commands
 */
enum class TPCIControl : uint8_t {
    Connect = 0x80,       ///< T_Connect
    Disconnect = 0x81,    ///< T_Disconnect
    Ack = 0xC2,          ///< T_ACK
    Nak = 0xC3,          ///< T_NAK
};

/**
 * @brief Raw TPCI field wrapper
 */
struct TPCIField {
    uint8_t raw{0};
    constexpr TPCIField() = default;
    constexpr explicit TPCIField(uint8_t value) : raw(value) {}

    /**
     * @brief Extract TPCI base type from a field.
     */
    constexpr TPCI type() const {
        return static_cast<TPCI>(raw & 0xC0);
    }

    /**
     * @brief Extract 4-bit sequence number from a TPCI field.
     */
    constexpr uint8_t seqNum() const {
        return static_cast<uint8_t>((raw >> 2) & 0x0F);
    }

    /**
     * @brief Check if a TPCI field is a specific unnumbered control.
     */
    constexpr bool isControl(TPCIControl control) const {
        return raw == static_cast<uint8_t>(control);
    }

    /**
     * @brief Check for numbered ACK control.
     */
    constexpr bool isAck() const {
        return type() == TPCI::NumberedControl && ((raw & 0x03) == 0x02);
    }

    /**
     * @brief Check for numbered NAK control.
     */
    constexpr bool isNak() const {
        return type() == TPCI::NumberedControl && ((raw & 0x03) == 0x03);
    }

    /**
     * @brief Build a TPCI field from a base type and optional sequence number.
     */
    static constexpr TPCIField create(TPCI tpci, uint8_t seq = 0) {
        return TPCIField(static_cast<uint8_t>(tpci) | ((seq & 0x0F) << 2));
    }

    /**
     * @brief Build a TPCI field for a control command.
     */
    static constexpr TPCIField create(TPCIControl control) {
        return TPCIField(static_cast<uint8_t>(control));
    }

    /**
     * @brief Build a T_ACK TPCI field for a given sequence number.
     */
    static constexpr TPCIField ack(uint8_t seq) {
        return TPCIField(static_cast<uint8_t>(TPCI::NumberedControl) | ((seq & 0x0F) << 2) | 0x02);
    }

    /**
     * @brief Build a T_NAK TPCI field for a given sequence number.
     */
    static constexpr TPCIField nak(uint8_t seq) {
        return TPCIField(static_cast<uint8_t>(TPCI::NumberedControl) | ((seq & 0x0F) << 2) | 0x03);
    }
};

} // namespace protocol
} // namespace knx
