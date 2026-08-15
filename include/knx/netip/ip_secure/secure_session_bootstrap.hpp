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

    static util::Result<AuthenticateRequest> buildAuthenticateRequest(
        const knx::security::X25519::Scalar& clientPrivateKey,
        std::span<const uint8_t> passwordLatin1,
        UserId userId,
        const std::array<uint8_t, SecureWrapper::kSerialLen>& clientSerial,
        uint64_t initialSequence,
        const SecureSession::PublicKey& clientPublicKey,
        std::span<const uint8_t> sessionResponseFrame,
        std::span<uint8_t> outWrappedFrame);

    static util::Result<uint8_t> decodeSessionStatus(const SecureWrapper::Key& wrapperKey,
                                                     SessionId sessionId,
                                                     std::span<const uint8_t> frame,
                                                     std::span<uint8_t> scratchPlaintext);
};

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED