// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/security/aes128_cbc_mac.hpp"
#include "knx/security/aes128_ctr.hpp"
#include "knx/util/result.hpp"
#include "knx/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {
namespace ip_secure {

// Implements the KNXnet/IP Secure Wrapper (service type 0x0950) primitive as used by
// - KNX/IP Secure tunnelling over TCP (session id != 0, tag = 0x0000)
// - KNX/IP Secure routing over multicast (session id == 0, tag = random)
//
// Algorithm matches KNXUltimate and the KNX IP Secure spec: AES-CBC-MAC + AES-CTR.
class SecureWrapper {
public:
    using Key = knx::security::Aes128CbcMac::Key;

    static constexpr uint8_t kHeaderLen = 0x06;
    static constexpr uint8_t kVersion = 0x10;
    static constexpr NetIpServiceType kServiceTypeSecureWrapper = NetIpServiceType(0x0950);

    static constexpr size_t kSeqLen = 6;
    static constexpr size_t kSerialLen = 6;
    static constexpr size_t kTagLen = 2;
    static constexpr size_t kMacLen = 16;

    // Total overhead excluding encrypted payload.
    // header(6) + sid(2) + seq(6) + serial(6) + tag(2) + mac(16)
    static constexpr size_t kOverhead = 38;

    struct Parsed {
        SessionId sessionId{SessionId::invalid()};
        std::array<uint8_t, kSeqLen> seq{};
        std::array<uint8_t, kSerialLen> serial{};
        std::array<uint8_t, kTagLen> tag{};
    };

    // Wrap an inner KNXnet/IP frame into a SecureWrapper.
    //
    // For tunnelling:
    // - sessionId must be the negotiated session id
    // - tag should be {0x00, 0x00}
    // - counterSuffix must be {0x00,0x00,0xFF,0x00}
    //
    // For routing:
    // - sessionId must be 0
    // - counterSuffix must be {tag[0],tag[1],0xFF,0x00}
    static util::Result<size_t> wrap(const Key& key,
                                     SessionId sessionId,
                                     const std::array<uint8_t, kSeqLen>& seq,
                                     const std::array<uint8_t, kSerialLen>& serial,
                                     const std::array<uint8_t, kTagLen>& tag,
                                     const std::array<uint8_t, 4>& counterSuffix,
                                     std::span<const uint8_t> inner,
                                     std::span<uint8_t> outWrapper);

    // Unwrap and authenticate a SecureWrapper. Returns false on parse, decrypt, or MAC failure.
    //
    // For routing, set expectedSessionId=0.
    // For tunnelling, set expectedSessionId to the negotiated session id.
    static util::Result<size_t> unwrapAndVerify(const Key& key,
                                                SessionId expectedSessionId,
                                                std::span<const uint8_t> wrapper,
                                                std::span<uint8_t> outPlaintext);

private:
    static util::Result<void> decodeHeader_(std::span<const uint8_t> in, NetIpServiceType& serviceType, uint16_t& totalLen);
};

} // namespace ip_secure
} // namespace netip
} // namespace knx
