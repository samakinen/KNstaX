// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file device_certificate.hpp
 * @brief The KNX device certificate: what an installer types into ETS
 *
 * ETS reads a Data Secure device's Factory Tool Key (FDSK) only through its
 * device certificate, printed on the device or its packaging as text (and a QR
 * code of the same text). It is 36 Base32 characters in six groups of six:
 *
 *     serial number (6 octets) | FDSK (16 octets) | CRC-4 (4 bits)
 *
 * 180 bits, most significant bit first, in the RFC 4648 alphabet (A-Z, 2-7:
 * no 0 or 1, which ETS does not accept). The CRC-4 uses the generator
 * x^4 + x + 1 over the 22 octets, high nibble first, from 0. The layout is the
 * one ETS accepts and the open-source KNX stacks generate.
 *
 * The serial number is the device's KNX serial number, PID_SERIAL_NUMBER as
 * ETS reads it (manufacturer code first).
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace security {

/// "XXXXXX-XXXXXX-XXXXXX-XXXXXX-XXXXXX-XXXXXX" and a terminating NUL.
using DeviceCertificateText = std::array<char, 36 + 5 + 1>;

/// CRC-4, generator x^4 + x + 1, over @p data high nibble first, from 0.
constexpr uint8_t deviceCertificateCrc4(std::span<const uint8_t> data)
{
    uint8_t crc = 0;
    const auto step = [&crc](uint8_t nibble) {
        crc = static_cast<uint8_t>(crc ^ nibble);
        for (int bit = 0; bit < 4; ++bit) {
            crc = static_cast<uint8_t>((crc & 0x8u) != 0u ? ((crc << 1) ^ 0x13u) : (crc << 1));
        }
        crc = static_cast<uint8_t>(crc & 0xFu);
    };
    for (const uint8_t octet : data) {
        step(static_cast<uint8_t>(octet >> 4));
        step(static_cast<uint8_t>(octet & 0xFu));
    }
    return crc;
}

/// The device certificate of a device with @p serialNumber and @p fdsk.
constexpr DeviceCertificateText formatDeviceCertificate(std::span<const uint8_t, 6> serialNumber,
                                                        std::span<const uint8_t, 16> fdsk)
{
    std::array<uint8_t, 23> bits{};
    for (size_t i = 0; i < 6; ++i) bits[i] = serialNumber[i];
    for (size_t i = 0; i < 16; ++i) bits[6 + i] = fdsk[i];
    bits[22] = static_cast<uint8_t>(deviceCertificateCrc4(std::span<const uint8_t>(bits.data(), 22)) << 4);

    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    DeviceCertificateText text{};
    size_t out = 0;
    for (size_t symbol = 0; symbol < 36; ++symbol) {
        if (symbol != 0 && symbol % 6 == 0) {
            text[out++] = '-';
        }
        // Five bits from bit position symbol * 5, most significant first.
        uint8_t value = 0;
        for (size_t b = 0; b < 5; ++b) {
            const size_t position = symbol * 5 + b;
            const uint8_t bit = static_cast<uint8_t>((bits[position / 8] >> (7 - position % 8)) & 1u);
            value = static_cast<uint8_t>((value << 1) | bit);
        }
        text[out++] = kAlphabet[value];
    }
    text[out] = '\0';
    return text;
}

}  // namespace security
}  // namespace knx
