// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_session.hpp"

#if KNX_SECURE_ENABLED

#include "knx/security/key_derivation.hpp"
#include "knx/netip/header_codec.hpp"
#include "knx/util/byte_stream.hpp"

#include <algorithm>
#include <array>
#include <string_view>

namespace knx {
namespace netip {
namespace ip_secure {

constexpr std::array<uint8_t, 8> kHpaiControlEndpointEmpty{0x08, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
constexpr std::array<uint8_t, 16> kAuthCtrIvBytes{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00,
};

namespace {

constexpr size_t kAuthenticateAdditionalDataLen = SecureSession::kHeaderLen + 2 + SecureSession::kPublicKeyLen;
constexpr std::string_view kUserPasswordSalt = "user-password.1.secure.ip.knx.org";

}

util::Result<void> SecureSession::decodeHeader_(std::span<const uint8_t> in, NetIpServiceType& serviceType, uint16_t& totalLen)
{
    KnxNetIpHeader header;
    auto headerResult = KnxNetIpCodec::decodeHeader(in, header);
    if (headerResult.isError()) return headerResult.error();
    if (header.totalLength != in.size()) return util::ErrorCode::InvalidFrameSize;
    serviceType = header.serviceType;
    totalLen = header.totalLength;
    return util::Result<void>::ok();
}

util::Result<void> SecureSession::deriveUserPasswordKeyLatin1(std::span<const uint8_t> passwordLatin1, UserPasswordKey& outKey)
{
    const auto saltBytes = std::span(
        reinterpret_cast<const uint8_t*>(kUserPasswordSalt.data()),
        kUserPasswordSalt.size());

    auto result = knx::security::KeyDerivation::pbkdf2(
        passwordLatin1,
        saltBytes,
        65536,
        outKey);
    if (result.isError()) {
        return result.error();
    }
    return util::Result<void>::ok();
}

util::Result<void> SecureSession::encodeSessionRequest(const PublicKey& clientPublicKey,
                                                       std::span<uint8_t, kSessionRequestFrameLen> out)
{
    util::ByteWriter writer(out);
    auto h0 = writer.u8(kHeaderLen);
    if (h0.isError()) return h0.error();
    auto h1 = writer.u8(kVersion);
    if (h1.isError()) return h1.error();
    auto st = writer.u16be(kServiceTypeSessionRequest.value());
    if (st.isError()) return st.error();
    auto len = writer.u16be(static_cast<uint16_t>(kSessionRequestFrameLen));
    if (len.isError()) return len.error();
    auto hpai = writer.writeBytes(std::span<const uint8_t>(kHpaiControlEndpointEmpty));
    if (hpai.isError()) return hpai.error();
    auto key = writer.writeBytes(std::span<const uint8_t>(clientPublicKey));
    if (key.isError()) return key.error();
    return util::Result<void>::ok();
}

util::Result<void> SecureSession::encodeSessionResponse(SessionId sessionId,
                                                        const PublicKey& serverPublicKey,
                                                        const std::array<uint8_t, 16>& messageAuthenticationCode,
                                                        std::span<uint8_t, kSessionResponseFrameLen> out)
{
    util::ByteWriter writer(out);
    auto h0 = writer.u8(kHeaderLen);
    if (h0.isError()) return h0.error();
    auto h1 = writer.u8(kVersion);
    if (h1.isError()) return h1.error();
    auto st = writer.u16be(kServiceTypeSessionResponse.value());
    if (st.isError()) return st.error();
    auto len = writer.u16be(static_cast<uint16_t>(kSessionResponseFrameLen));
    if (len.isError()) return len.error();
    auto sid = writer.u16be(sessionId.value());
    if (sid.isError()) return sid.error();
    auto key = writer.writeBytes(std::span<const uint8_t>(serverPublicKey));
    if (key.isError()) return key.error();
    auto mac = writer.writeBytes(std::span<const uint8_t>(messageAuthenticationCode));
    if (mac.isError()) return mac.error();
    return util::Result<void>::ok();
}

util::Result<void> SecureSession::decodeSessionResponse(std::span<const uint8_t> frame, SessionResponse& out)
{
    NetIpServiceType st;
    uint16_t totalLen = 0;
    auto headerResult = decodeHeader_(frame, st, totalLen);
    if (headerResult.isError()) return headerResult;
    if (st != kServiceTypeSessionResponse) return util::ErrorCode::DecodeFailed;


    // KNX specification requires a 16-byte message authentication code (MAC) in SessionResponse (0x0952).
    // Total frame length must be 56 bytes (6 header + 2 session id + 32 server pubkey + 16 MAC).
    if (frame.size() != 6 + 2 + kPublicKeyLen + 16) return util::ErrorCode::InvalidFrameSize;

    out.sessionId = SessionId(static_cast<uint16_t>((static_cast<uint16_t>(frame[6]) << 8) | frame[7]));
    std::copy_n(frame.begin() + 8, kPublicKeyLen, out.serverPublicKey.begin());
    std::copy_n(frame.begin() + 8 + kPublicKeyLen,
                out.messageAuthenticationCode.size(),
                out.messageAuthenticationCode.begin());

    return util::Result<void>::ok();
}

util::Result<void> SecureSession::encodeSessionAuthenticate(const UserPasswordKey& userPasswordKey,
                                                            UserId userId,
                                                            const PublicKey& clientPublicKey,
                                                            const PublicKey& serverPublicKey,
                                                            std::span<uint8_t, kSessionAuthenticateFrameLen> out)
{
    // Compute XOR(clientPub, serverPub)
    std::array<uint8_t, kPublicKeyLen> x{};
    for (size_t i = 0; i < kPublicKeyLen; ++i) x[i] = static_cast<uint8_t>(clientPublicKey[i] ^ serverPublicKey[i]);

    // additionalData = hdr(06100953) || len(0018) || 00 || userId || xor(32)
    std::array<uint8_t, kAuthenticateAdditionalDataLen> additionalData{};
    util::ByteWriter additionalWriter(additionalData);
    auto ad0 = additionalWriter.u8(kHeaderLen);
    if (ad0.isError()) return ad0.error();
    auto ad1 = additionalWriter.u8(kVersion);
    if (ad1.isError()) return ad1.error();
    auto ad2 = additionalWriter.u16be(kServiceTypeSessionAuthenticate.value());
    if (ad2.isError()) return ad2.error();
    auto ad3 = additionalWriter.u16be(static_cast<uint16_t>(kSessionAuthenticateFrameLen));
    if (ad3.isError()) return ad3.error();
    auto ad4 = additionalWriter.u8(0x00);
    if (ad4.isError()) return ad4.error();
    auto ad5 = additionalWriter.u8(userId.value());
    if (ad5.isError()) return ad5.error();
    auto ad6 = additionalWriter.writeBytes(std::span<const uint8_t>(x));
    if (ad6.isError()) return ad6.error();

    knx::security::Aes128CbcMac::Block block0{};

    knx::security::Aes128CbcMac::Block macCbc{};
    auto macResult = knx::security::Aes128CbcMac::compute(userPasswordKey,
                                                          block0,
                                                          std::span<const uint8_t>(additionalData),
                                                          std::span<const uint8_t>(),
                                                          macCbc);
    if (macResult.isError()) {
        return macResult.error();
    }

    knx::security::Aes128Ctr::Counter ctr0{};
    std::copy(kAuthCtrIvBytes.begin(), kAuthCtrIvBytes.end(), ctr0.begin());

    std::array<uint8_t, 16> macEnc{};
    auto encResult = knx::security::Aes128Ctr::crypt(userPasswordKey,
                                                     ctr0,
                                                     macCbc,
                                                     macEnc);
    if (encResult.isError()) {
        return encResult.error();
    }

    util::ByteWriter writer(out);
    auto h0 = writer.u8(kHeaderLen);
    if (h0.isError()) return h0.error();
    auto h1 = writer.u8(kVersion);
    if (h1.isError()) return h1.error();
    auto st = writer.u16be(kServiceTypeSessionAuthenticate.value());
    if (st.isError()) return st.error();
    auto len = writer.u16be(static_cast<uint16_t>(kSessionAuthenticateFrameLen));
    if (len.isError()) return len.error();
    auto reserved = writer.u8(0x00);
    if (reserved.isError()) return reserved.error();
    auto uid = writer.u8(userId.value());
    if (uid.isError()) return uid.error();
    auto mac = writer.writeBytes(std::span<const uint8_t>(macEnc));
    if (mac.isError()) return mac.error();
    return util::Result<void>::ok();
}

util::Result<void> SecureSession::encodeSessionStatus(uint8_t status,
                                                      std::span<uint8_t, kSessionStatusFrameLen> out)
{
    util::ByteWriter writer(out);
    auto h0 = writer.u8(kHeaderLen);
    if (h0.isError()) return h0.error();
    auto h1 = writer.u8(kVersion);
    if (h1.isError()) return h1.error();
    auto st = writer.u16be(kServiceTypeSessionStatus.value());
    if (st.isError()) return st.error();
    auto len = writer.u16be(static_cast<uint16_t>(kSessionStatusFrameLen));
    if (len.isError()) return len.error();
    auto s = writer.u8(status);
    if (s.isError()) return s.error();
    auto reserved = writer.u8(0x00);
    if (reserved.isError()) return reserved.error();
    return util::Result<void>::ok();
}

util::Result<void> SecureSession::decodeSessionStatus(std::span<const uint8_t> frame, uint8_t& statusOut)
{
    NetIpServiceType st;
    uint16_t totalLen = 0;
    auto headerResult = decodeHeader_(frame, st, totalLen);
    if (headerResult.isError()) return headerResult;
    if (st != kServiceTypeSessionStatus) return util::ErrorCode::DecodeFailed;
    if (frame.size() != kSessionStatusFrameLen) return util::ErrorCode::InvalidFrameSize;
    statusOut = frame[6];
    return util::Result<void>::ok();
}

util::Result<void> SecureSession::deriveSessionKey(const knx::security::X25519::Scalar& clientPrivateKey,
                                    const PublicKey& serverPublicKey,
                                    SessionKey& out)
{
    knx::security::X25519::PublicKey peer{};
    std::copy(serverPublicKey.begin(), serverPublicKey.end(), peer.begin());

    knx::security::X25519::SharedSecret secret{};
    auto secretResult = knx::security::X25519::sharedSecret(clientPrivateKey, peer, secret);
    if (secretResult.isError()) {
        return secretResult.error();
    }

    knx::security::Sha256::Digest digest{};
    auto hashResult = knx::security::Sha256::hash(std::span<const uint8_t>(secret), digest);
    if (hashResult.isError()) {
        return hashResult.error();
    }

    std::copy_n(digest.begin(), out.size(), out.begin());
    return util::Result<void>::ok();
}

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
