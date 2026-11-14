// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/security/aes128_cbc_mac.hpp"
#include "knx/util/result.hpp"
#include "knx/types.hpp"

#if KNX_SECURE_ENABLED

#include "knx/security/aes128_ctr.hpp"
#include "knx/security/sha256.hpp"
#include "knx/security/x25519.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {
namespace ip_secure {

class SecureSession {
public:
    static constexpr uint8_t kHeaderLen = 0x06;
    static constexpr uint8_t kVersion = 0x10;

    static constexpr NetIpServiceType kServiceTypeSessionRequest = NetIpServiceType(0x0951);
    static constexpr NetIpServiceType kServiceTypeSessionResponse = NetIpServiceType(0x0952);
    static constexpr NetIpServiceType kServiceTypeSessionAuthenticate = NetIpServiceType(0x0953);
    static constexpr NetIpServiceType kServiceTypeSessionStatus = NetIpServiceType(0x0954);

    static constexpr size_t kPublicKeyLen = 32;
    static constexpr size_t kSessionRequestFrameLen = kHeaderLen + 8 + kPublicKeyLen;
    static constexpr size_t kSessionResponseFrameLen = kHeaderLen + 2 + kPublicKeyLen + 16;
    static constexpr size_t kSessionAuthenticateFrameLen = 0x18;
    static constexpr size_t kSessionStatusFrameLen = 8;

    using PublicKey = std::array<uint8_t, kPublicKeyLen>;
    using SessionKey = std::array<uint8_t, 16>;
    using UserPasswordKey = std::array<uint8_t, 16>;
    using DeviceAuthenticationCode = std::array<uint8_t, 16>;
    using MessageAuthenticationCode = std::array<uint8_t, 16>;

    struct SessionResponse {
        SessionId sessionId{SessionId::invalid()};
        PublicKey serverPublicKey{};
        std::array<uint8_t, 16> messageAuthenticationCode{};
    };

    // Derives the user password key (PBKDF2-SHA256, salt = "user-password.1.secure.ip.knx.org", 65536 iterations, 16 bytes).
    static util::Result<void> deriveUserPasswordKeyLatin1(std::span<const uint8_t> passwordLatin1, UserPasswordKey& outKey);

    // Builds 0x0951 SECURE_SESSION_REQUEST.
    static util::Result<void> encodeSessionRequest(const PublicKey& clientPublicKey,
                                                   std::span<uint8_t, kSessionRequestFrameLen> out);

    // Builds 0x0952 SECURE_SESSION_RESPONSE.
    // The messageAuthenticationCode must be 16 bytes and is required by the KNX specification.
    static util::Result<void> encodeSessionResponse(SessionId sessionId,
                                                    const PublicKey& serverPublicKey,
                                                    const std::array<uint8_t, 16>& messageAuthenticationCode,
                                                    std::span<uint8_t, kSessionResponseFrameLen> out);

    // Parses 0x0952 SECURE_SESSION_RESPONSE.
    static util::Result<void> decodeSessionResponse(std::span<const uint8_t> frame, SessionResponse& out);

    // Builds plaintext 0x0953 SECURE_SESSION_AUTHENTICATE.
    // Note: on-wire this is typically wrapped in 0x0950 using the negotiated session key.
    static util::Result<void> encodeSessionAuthenticate(const UserPasswordKey& userPasswordKey,
                                                        UserId userId,
                                                        const PublicKey& clientPublicKey,
                                                        const PublicKey& serverPublicKey,
                                                        std::span<uint8_t, kSessionAuthenticateFrameLen> out);

    // Builds plaintext 0x0954 SECURE_SESSION_STATUS.
    // (Used in tests/mocks; real gateways send this wrapped.)
    static util::Result<void> encodeSessionStatus(uint8_t status,
                                                  std::span<uint8_t, kSessionStatusFrameLen> out);

    static util::Result<void> decodeSessionStatus(std::span<const uint8_t> frame, uint8_t& statusOut);

    // Derive session key = SHA-256(X25519(priv, peerPub))[0..15].
    /// Device Authentication Code from its shared secret (03/08/09 §2.3.1.3.1):
    /// PBKDF2(HMAC-SHA256, secret, "device-authentication-code.1.secure.ip.knx.org",
    /// 65 536, 128).
    static util::Result<void> deriveDeviceAuthenticationCodeLatin1(std::span<const uint8_t> secretLatin1,
                                                                    DeviceAuthenticationCode& outCode);

    /**
     * @brief The SESSION_RESPONSE MAC (03/08/09 §2.2.3.7.4): AES128-CCM with
     * the Device Authentication Code as key, A = Secure Header | Secure
     * Session Identifier | (X ^ Y), empty payload, B0 = 0 with Q = 0 and
     * Ctr0 = 0…0 FFh 00h. The returned MAC is the encrypted one, as it
     * appears in the frame.
     */
    static util::Result<void> computeSessionResponseMac(const DeviceAuthenticationCode& deviceAuthenticationCode,
                                                       SessionId sessionId,
                                                       const PublicKey& clientPublicKey,
                                                       const PublicKey& serverPublicKey,
                                                       MessageAuthenticationCode& outMac);

    /// Check the MAC of a decoded SESSION_RESPONSE against the Device
    /// Authentication Code (03/08/09 §2.2.3.7.5). Constant-time compare.
    static bool verifySessionResponseMac(const DeviceAuthenticationCode& deviceAuthenticationCode,
                                         const SessionResponse& response,
                                         const PublicKey& clientPublicKey);

    static util::Result<void> deriveSessionKey(const knx::security::X25519::Scalar& clientPrivateKey,
                                 const PublicKey& serverPublicKey,
                                 SessionKey& out);

private:
    static util::Result<void> decodeHeader_(std::span<const uint8_t> in, NetIpServiceType& serviceType, uint16_t& totalLen);
};

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
