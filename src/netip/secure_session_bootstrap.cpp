// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_session_bootstrap.hpp"

#include "knx/netip/header_codec.hpp"

#if KNX_SECURE_ENABLED

namespace knx {
namespace netip {
namespace ip_secure {

namespace {

constexpr uint64_t kSeq48Mask = 0xFFFFFFFFFFFFULL;

void encodeSeq48BE(uint64_t seq48, std::span<uint8_t, SecureWrapper::kSeqLen> out) noexcept
{
    seq48 &= kSeq48Mask;
    out[0] = static_cast<uint8_t>((seq48 >> 40) & 0xFF);
    out[1] = static_cast<uint8_t>((seq48 >> 32) & 0xFF);
    out[2] = static_cast<uint8_t>((seq48 >> 24) & 0xFF);
    out[3] = static_cast<uint8_t>((seq48 >> 16) & 0xFF);
    out[4] = static_cast<uint8_t>((seq48 >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>(seq48 & 0xFF);
}

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
    std::span<uint8_t> outWrappedFrame)
{
    SecureSession::SessionResponse parsed{};
    if (SecureSession::decodeSessionResponse(sessionResponseFrame, parsed).isError()) {
        return util::ErrorCode::DecodeFailed;
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
    encodeSeq48BE(seq48, seqBytes);

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
                                                                  std::span<uint8_t> scratchPlaintext)
{
    KnxNetIpHeader header;
    auto headerResult = KnxNetIpCodec::decodeHeader(frame, header);
    if (headerResult.isError()) return util::ErrorCode::DecodeFailed;

    uint8_t status = 0xFF;
    if (header.serviceType == SecureWrapper::kServiceTypeSecureWrapper) {
        auto unwrapResult = SecureWrapper::unwrapAndVerify(wrapperKey, sessionId, frame, scratchPlaintext);
        if (unwrapResult.isError()) return util::ErrorCode::DecodeFailed;
        if (SecureSession::decodeSessionStatus(scratchPlaintext.first(unwrapResult.value()), status).isError()) {
            return util::ErrorCode::DecodeFailed;
        }
        return status;
    }

    if (SecureSession::decodeSessionStatus(frame, status).isError()) return util::ErrorCode::DecodeFailed;
    return status;
}

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED