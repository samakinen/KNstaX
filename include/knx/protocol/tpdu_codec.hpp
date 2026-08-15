// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tpdu_codec.hpp
 * @brief Shared TPDU (TPCI/APCI) header packing/unpacking helpers
 */

#pragma once

#include "knx/application/apci_services.hpp"
#include "knx/protocol/tpci.hpp"
#include "knx/util/fixed_vector.hpp"
#include "knx/util/result.hpp"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace knx {
namespace protocol {

struct TpduHeader {
    TPCIField tpci;                // upper 6 bits used
    application::APCIField apci;   // lower 10 bits used
};

inline constexpr size_t tpduLengthFromPayload(size_t payloadLength) {
    return 2 + payloadLength;
}

// Transport control frames (T_Connect, T_Disconnect, T_ACK, T_NAK) have no APCI
// and require exactly 1 TPDU byte. Data frames require 2 bytes (TPCI+APCI) + payload.
inline constexpr size_t tpduLengthFromTpci(TPCIField tpci, size_t payloadLength) {
    const uint8_t tpciType = static_cast<uint8_t>(tpci.raw & 0xC0);
    if (tpciType == 0x80 || tpciType == 0xC0) {
        return 1;
    }
    return 2 + payloadLength;
}

inline constexpr size_t payloadLengthFromTpdu(size_t tpduLength) {
    return (tpduLength >= 2) ? (tpduLength - 2) : 0;
}

inline constexpr void packTpduHeader(TPCIField tpci, application::APCIField apci, uint8_t& tpdu0, uint8_t& tpdu1) {
    const uint8_t tpciType = static_cast<uint8_t>(tpci.raw & 0xC0);
    if (tpciType == 0x80 || tpciType == 0xC0) {
        // Transport control frames: APCI is not present; low 2 bits belong to TPCI.
        tpdu0 = tpci.raw;
        tpdu1 = 0x00;
        return;
    }

    const uint8_t tpci6 = static_cast<uint8_t>(tpci.raw & 0xFC);
    const uint16_t apci10 = static_cast<uint16_t>(apci.raw & 0x03FF);
    tpdu0 = static_cast<uint8_t>(tpci6 | ((apci10 >> 8) & 0x03));
    tpdu1 = static_cast<uint8_t>(apci10 & 0xFF);
}

inline constexpr TpduHeader unpackTpduHeader(uint8_t tpdu0, uint8_t tpdu1) {
    TpduHeader header{};
    const uint8_t tpciType = static_cast<uint8_t>(tpdu0 & 0xC0);
    if (tpciType == 0x80 || tpciType == 0xC0) {
        // Transport control frames: APCI is not present; low 2 bits belong to TPCI.
        header.tpci = TPCIField(tpdu0);
        header.apci = application::APCIField(0);
        return header;
    }

    header.tpci = TPCIField(static_cast<uint8_t>(tpdu0 & 0xFC));
    header.apci = application::APCIField(static_cast<uint16_t>(((static_cast<uint16_t>(tpdu0) & 0x03) << 8) | tpdu1));
    return header;
}

inline util::Result<size_t> buildTpdu(TPCIField tpci,
                                      application::APCIField apci,
                                      std::span<const uint8_t> payload,
                                      std::span<uint8_t> out) {
    const size_t totalLength = tpduLengthFromTpci(tpci, payload.size());
    if (out.size() < totalLength) {
        return util::ErrorCode::BufferTooSmall;
    }

    if (totalLength == 1) {
        out[0] = tpci.raw;
    } else {
        packTpduHeader(tpci, apci, out[0], out[1]);
        for (size_t i = 0; i < payload.size(); ++i) {
            out[2 + i] = payload[i];
        }
    }
    return totalLength;
}

inline util::Result<size_t> buildTpdu(TPCIField tpci,
                                      application::APCIField apci,
                                      std::initializer_list<uint8_t> payload,
                                      std::span<uint8_t> out) {
    return buildTpdu(tpci, apci, std::span<const uint8_t>(payload), out);
}

inline util::Result<void> buildTpduInPlace(TPCIField tpci,
                                           application::APCIField apci,
                                           std::span<const uint8_t> payload,
                                           std::vector<uint8_t>& out) {
    out.resize(tpduLengthFromTpci(tpci, payload.size()));
    auto result = buildTpdu(tpci, apci, payload, std::span<uint8_t>(out));
    if (result.isError()) {
        out.clear();
        return result.error();
    }
    return util::Result<void>::ok();
}

inline util::Result<void> buildTpduInPlace(TPCIField tpci,
                                           application::APCIField apci,
                                           std::initializer_list<uint8_t> payload,
                                           std::vector<uint8_t>& out) {
    return buildTpduInPlace(tpci, apci, std::span<const uint8_t>(payload), out);
}

template <size_t N>
inline util::Result<void> buildTpduInPlace(TPCIField tpci,
                                           application::APCIField apci,
                                           std::span<const uint8_t> payload,
                                           util::FixedVector<uint8_t, N>& out) {
    const size_t needed = tpduLengthFromTpci(tpci, payload.size());
    out.resize(needed);
    auto result = buildTpdu(tpci, apci, payload, std::span<uint8_t>(out.data(), needed));
    if (result.isError()) {
        out.clear();
        return result.error();
    }
    return util::Result<void>::ok();
}

template <size_t N>
inline util::Result<void> buildTpduInPlace(TPCIField tpci,
                                           application::APCIField apci,
                                           std::initializer_list<uint8_t> payload,
                                           util::FixedVector<uint8_t, N>& out) {
    return buildTpduInPlace(tpci, apci, std::span<const uint8_t>(payload), out);
}

template <size_t N>
inline util::Result<void> buildTpduInPlace(TPCIField tpci,
                                           application::APCIField apci,
                                           const util::FixedVector<uint8_t, N>& payload,
                                           util::FixedVector<uint8_t, N>& out) {
    return buildTpduInPlace(tpci, apci, payload.span(), out);
}

inline std::vector<uint8_t> buildTpdu(TPCIField tpci,
                                      application::APCIField apci,
                                      std::span<const uint8_t> payload) {
    const size_t len = tpduLengthFromTpci(tpci, payload.size());
    std::vector<uint8_t> tpdu(len);
    if (len == 1) {
        tpdu[0] = tpci.raw;
    } else {
        packTpduHeader(tpci, apci, tpdu[0], tpdu[1]);
        for (size_t i = 0; i < payload.size(); ++i) {
            tpdu[2 + i] = payload[i];
        }
    }
    return tpdu;
}

inline std::vector<uint8_t> buildTpdu(TPCIField tpci,
                                      application::APCIField apci,
                                      std::initializer_list<uint8_t> payload) {
    return buildTpdu(tpci, apci, std::span<const uint8_t>(payload));
}

inline bool tpduHasHeader(std::span<const uint8_t> tpdu) {
    
    return tpdu.size() >= 2;
}

inline size_t tpduPayloadLength(std::span<const uint8_t> tpdu) {
    return (tpdu.size() >= 2) ? (tpdu.size() - 2) : 0;
}

} // namespace protocol
} // namespace knx
