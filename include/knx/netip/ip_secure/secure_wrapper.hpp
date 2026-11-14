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
namespace security {
class Aes128BlockCipher;
}
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
    static constexpr NetIpServiceType kServiceTypeTimerNotify = NetIpServiceType(0x0955);

    static constexpr size_t kSeqLen = 6;
    static constexpr size_t kSerialLen = 6;
    static constexpr size_t kTagLen = 2;
    static constexpr size_t kMacLen = 16;

    // Total overhead excluding encrypted payload.
    // header(6) + sid(2) + seq(6) + serial(6) + tag(2) + mac(16)
    static constexpr size_t kOverhead = 38;

    /// The header's total-length field is 16 bits wide.
    static constexpr size_t kMaxTotalLen = 0xFFFF;

    /// TIMER_NOTIFY is fixed-length: header(6) + timer(6) + serial(6) + tag(2)
    /// + MAC(16) (03_08_09 §2.2.2.4.2).
    static constexpr size_t kTimerNotifyLen = 36;

    /// The security information of a TIMER_NOTIFY, all of which is sent in
    /// the clear and covered by the MAC.
    struct TimerNotify {
        std::array<uint8_t, kSeqLen> timer{};
        std::array<uint8_t, kSerialLen> serial{};
        std::array<uint8_t, kTagLen> tag{};
    };

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

    /// As above, with a cipher the caller has already keyed. A security
    /// context that lives as long as its key uses this so the key schedule is
    /// built once per key rather than two or three times per datagram.
    static util::Result<size_t> wrap(knx::security::Aes128BlockCipher& keyedCipher,
                                     SessionId sessionId,
                                     const std::array<uint8_t, kSeqLen>& seq,
                                     const std::array<uint8_t, kSerialLen>& serial,
                                     const std::array<uint8_t, kTagLen>& tag,
                                     const std::array<uint8_t, 4>& counterSuffix,
                                     std::span<const uint8_t> inner,
                                     std::span<uint8_t> outWrapper);

    /// The 48-bit sequence information of a SECURE_WRAPPER frame (octets 8…13:
    /// after the 6-octet header and the 2-octet Secure Session Identifier).
    /// 0 for a frame too short to hold it.
    static constexpr uint64_t sequenceOf(std::span<const uint8_t> wrapper) noexcept
    {
        if (wrapper.size() < kHeaderLen + 2 + kSeqLen) return 0;
        uint64_t sequence = 0;
        for (size_t i = 0; i < kSeqLen; ++i) {
            sequence = (sequence << 8) | wrapper[kHeaderLen + 2 + i];
        }
        return sequence;
    }

    // Unwrap and authenticate a SecureWrapper. Returns false on parse, decrypt, or MAC failure.
    //
    // For routing, set expectedSessionId=0.
    // For tunnelling, set expectedSessionId to the negotiated session id.
    static util::Result<size_t> unwrapAndVerify(const Key& key,
                                                SessionId expectedSessionId,
                                                std::span<const uint8_t> wrapper,
                                                std::span<uint8_t> outPlaintext);

    /// As above, with a pre-keyed cipher, and handing back the security
    /// information of a frame that verified. `parsedOut` is written only on
    /// success. On any failure after decryption started, the plaintext span is
    /// zeroed: it holds unauthenticated data at that point.
    static util::Result<size_t> unwrapAndVerify(knx::security::Aes128BlockCipher& keyedCipher,
                                                SessionId expectedSessionId,
                                                std::span<const uint8_t> wrapper,
                                                std::span<uint8_t> outPlaintext,
                                                Parsed* parsedOut);

    /// Build a TIMER_NOTIFY (0955h) into `out`; returns kTimerNotifyLen.
    static util::Result<size_t> encodeTimerNotify(knx::security::Aes128BlockCipher& keyedCipher,
                                                  const TimerNotify& notify,
                                                  std::span<uint8_t> out);

    /// Parse and authenticate a TIMER_NOTIFY. Anything but exactly 36 octets,
    /// or a MAC that does not verify, is an error (03_08_09 §2.2.2.4.4).
    static util::Result<TimerNotify> decodeTimerNotify(knx::security::Aes128BlockCipher& keyedCipher,
                                                       std::span<const uint8_t> datagram);

private:
    static util::Result<void> decodeHeader_(std::span<const uint8_t> in, NetIpServiceType& serviceType, uint16_t& totalLen);
};

} // namespace ip_secure
} // namespace netip
} // namespace knx
