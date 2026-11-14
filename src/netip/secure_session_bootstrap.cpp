// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_session_bootstrap.hpp"

#include "knx/netip/header_codec.hpp"
#include "knx/security/data_secure.hpp"

#if KNX_SECURE_ENABLED

namespace knx {
namespace netip {
namespace ip_secure {

namespace {

constexpr uint64_t kSeq48Mask = 0xFFFFFFFFFFFFULL;

} // namespace

util::Result<void> SecureSessionBootstrap::deriveClientPublicKey(
    const knx::security::X25519::Scalar& clientPrivateKey,
    SecureSession::PublicKey& outClientPublicKey)
{
    knx::security::X25519::PublicKey clientPubRaw{};
    auto pubResult = knx::security::X25519::publicFromPrivate(clientPrivateKey, clientPubRaw);
    if (pubResult.isError()) return pubResult.error();

    for (size_t i = 0; i < outClientPublicKey.size(); ++i) outClientPublicKey[i] = clientPubRaw[i];
    return util::Result<void>::ok();
}

util::Result<SecureSessionBootstrap::AuthenticateRequest> SecureSessionBootstrap::buildAuthenticateRequest(
    const knx::security::X25519::Scalar& clientPrivateKey,
    std::span<const uint8_t> passwordLatin1,
    UserId userId,
    const std::array<uint8_t, SecureWrapper::kSerialLen>& clientSerial,
    uint64_t initialSequence,
    const SecureSession::PublicKey& clientPublicKey,
    std::span<const uint8_t> sessionResponseFrame,
    std::span<uint8_t> outWrappedFrame,
    const SecureSession::DeviceAuthenticationCode* deviceAuthenticationCode)
{
    SecureSession::SessionResponse parsed{};
    if (SecureSession::decodeSessionResponse(sessionResponseFrame, parsed).isError()) {
        return util::ErrorCode::DecodeFailed;
    }

    // 03/08/09 §2.2.3.7.3/§2.2.3.7.5: the MAC over the server's and the
    // client's public values, keyed with the Device Authentication Code, is
    // what proves the response came from the device and not a man in the
    // middle. Checked before any key material is derived from it.
    if (deviceAuthenticationCode != nullptr
        && !SecureSession::verifySessionResponseMac(*deviceAuthenticationCode, parsed, clientPublicKey)) {
        return util::ErrorCode::AccessDenied;
    }

    SecureSession::SessionKey sessionKey{};
    if (SecureSession::deriveSessionKey(clientPrivateKey, parsed.serverPublicKey, sessionKey).isError()) {
        return util::ErrorCode::OperationFailed;
    }

    SecureSession::UserPasswordKey userPasswordKey{};
    if (SecureSession::deriveUserPasswordKeyLatin1(passwordLatin1, userPasswordKey).isError()) {
        return util::ErrorCode::OperationFailed;
    }

    std::array<uint8_t, SecureSession::kSessionAuthenticateFrameLen> authPlain{};
    auto authPlainResult = SecureSession::encodeSessionAuthenticate(userPasswordKey,
                                                                    userId,
                                                                    clientPublicKey,
                                                                    parsed.serverPublicKey,
                                                                    authPlain);
    if (authPlainResult.isError()) return util::ErrorCode::EncodeFailed;

    SecureWrapper::Key wrapperKey{};
    for (size_t i = 0; i < wrapperKey.size(); ++i) wrapperKey[i] = sessionKey[i];

    const uint64_t seq48 = (initialSequence & kSeq48Mask);
    std::array<uint8_t, SecureWrapper::kSeqLen> seqBytes{};
    knx::security::writeSecureU48BE(seqBytes, seq48);

    const std::array<uint8_t, SecureWrapper::kTagLen> tag{0x00, 0x00};
    const std::array<uint8_t, 4> counterSuffix{0x00, 0x00, 0xFF, 0x00};

    auto wrappedResult = SecureWrapper::wrap(wrapperKey,
                                             parsed.sessionId,
                                             seqBytes,
                                             clientSerial,
                                             tag,
                                             counterSuffix,
                                             authPlain,
                                             outWrappedFrame);
    if (wrappedResult.isError()) return util::ErrorCode::OperationFailed;

    AuthenticateRequest request{};
    request.session.wrapperKey = wrapperKey;
    request.session.sessionId = parsed.sessionId;
    request.session.nextSequence = ((seq48 + 1u) & kSeq48Mask);
    request.wrappedFrameLength = wrappedResult.value();
    return request;
}

util::Result<uint8_t> SecureSessionBootstrap::decodeSessionStatus(const SecureWrapper::Key& wrapperKey,
                                                                  SessionId sessionId,
                                                                  std::span<const uint8_t> frame,
                                                                  std::span<uint8_t> scratchPlaintext,
                                                                  uint64_t* outSequence)
{
    KnxNetIpHeader header;
    auto headerResult = KnxNetIpCodec::decodeHeader(frame, header);
    if (headerResult.isError()) return util::ErrorCode::DecodeFailed;

    // A plain SESSION_STATUS is not authenticated: a success code in one
    // would let anyone on the path declare the session open.
    if (header.serviceType != SecureWrapper::kServiceTypeSecureWrapper) {
        return util::ErrorCode::AccessDenied;
    }

    auto unwrapResult = SecureWrapper::unwrapAndVerify(wrapperKey, sessionId, frame, scratchPlaintext);
    if (unwrapResult.isError()) return util::ErrorCode::DecodeFailed;

    uint8_t status = 0xFF;
    if (SecureSession::decodeSessionStatus(scratchPlaintext.first(unwrapResult.value()), status).isError()) {
        return util::ErrorCode::DecodeFailed;
    }
    if (outSequence != nullptr) {
        *outSequence = SecureWrapper::sequenceOf(frame);
    }
    return status;
}

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
