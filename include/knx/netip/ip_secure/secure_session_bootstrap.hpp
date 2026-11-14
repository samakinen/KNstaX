// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#if KNX_SECURE_ENABLED

#include "knx/netip/ip_secure/secure_session.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/security/x25519.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {
namespace ip_secure {

class SecureSessionBootstrap {
public:
    struct NegotiatedSession {
        SecureWrapper::Key wrapperKey{};
        SessionId sessionId{SessionId::invalid()};
        uint64_t nextSequence{0};
    };

    struct AuthenticateRequest {
        NegotiatedSession session{};
        size_t wrappedFrameLength{0};
    };

    static util::Result<void> deriveClientPublicKey(const knx::security::X25519::Scalar& clientPrivateKey,
                                                    SecureSession::PublicKey& outClientPublicKey);

    /**
     * @brief Answer a SESSION_RESPONSE with a wrapped SESSION_AUTHENTICATE.
     *
     * With @p deviceAuthenticationCode the SESSION_RESPONSE MAC is checked
     * first and a response that fails it is refused (AccessDenied):
     * that MAC is what authenticates the server to the client
     * (03/08/09 §2.2.3.7.3). Without one the check is skipped, which the
     * specification allows ("optionally", §2.2.3.7.5) but which leaves
     * the client open to a man in the middle.
     */
    static util::Result<AuthenticateRequest> buildAuthenticateRequest(
        const knx::security::X25519::Scalar& clientPrivateKey,
        std::span<const uint8_t> passwordLatin1,
        UserId userId,
        const std::array<uint8_t, SecureWrapper::kSerialLen>& clientSerial,
        uint64_t initialSequence,
        const SecureSession::PublicKey& clientPublicKey,
        std::span<const uint8_t> sessionResponseFrame,
        std::span<uint8_t> outWrappedFrame,
        const SecureSession::DeviceAuthenticationCode* deviceAuthenticationCode = nullptr);

    /**
     * @brief Decode the server's SESSION_STATUS during the handshake.
     *
     * "this frame shall only be sent wrapped in a SECURE_WRAPPER frame over
     * the intended secure session" (03/08/09 §2.2.3.9): a plain one is
     * refused, since anyone could send it. @p outSequence receives the
     * wrapper's sequence number, the first one received on the session.
     */
    static util::Result<uint8_t> decodeSessionStatus(const SecureWrapper::Key& wrapperKey,
                                                     SessionId sessionId,
                                                     std::span<const uint8_t> frame,
                                                     std::span<uint8_t> scratchPlaintext,
                                                     uint64_t* outSequence = nullptr);
};

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
