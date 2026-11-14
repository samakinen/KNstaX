// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_session_bootstrap.hpp"

#if KNX_SECURE_ENABLED

#include "knx/netip/ip_secure/secure_session.hpp"
#include "knx/netip/ip_secure/secure_tunneling_client.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/util/hex.hpp"

#include "../common/vec_file.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using knx::netip::ip_secure::SecureSession;
using knx::netip::ip_secure::SecureSessionBootstrap;
using knx::netip::ip_secure::SecureWrapper;
using knx::SessionId;
using knx::UserId;

namespace {

bool toFixed16(std::span<const uint8_t> in, std::array<uint8_t, 16>& out)
{
    if (in.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) out[i] = in[i];
    return true;
}

bool toFixed32(std::span<const uint8_t> in, std::array<uint8_t, 32>& out)
{
    if (in.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) out[i] = in[i];
    return true;
}

bool toFixed6(std::span<const uint8_t> in, std::array<uint8_t, 6>& out)
{
    if (in.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) out[i] = in[i];
    return true;
}

uint64_t seq48FromBytes(std::span<const uint8_t> seq)
{
    if (seq.size() != 6) return 0;
    uint64_t value = 0;
    for (size_t i = 0; i < 6; ++i) value = (value << 8) | static_cast<uint64_t>(seq[i]);
    return value & 0xFFFFFFFFFFFFULL;
}

bool loadVec(const std::string& path, std::map<std::string, std::string>& kv)
{
    std::string text;
    if (!knx_test::vec::readTextFile(path, text)) return false;
    return knx_test::vec::parseVec(text, kv);
}

} // namespace

int main()
{
    std::map<std::string, std::string> reqKv;
    std::map<std::string, std::string> respKv;
    std::map<std::string, std::string> authPlainKv;
    std::map<std::string, std::string> authWrappedKv;
    if (!loadVec("test/vectors/knxnetip_secure_session/secure_session_0951_request.vec", reqKv)) return 1;
    if (!loadVec("test/vectors/knxnetip_secure_session/secure_session_0952_response.vec", respKv)) return 2;
    if (!loadVec("test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_plain.vec", authPlainKv)) return 3;
    if (!loadVec("test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_wrapped.vec", authWrappedKv)) return 4;

    std::vector<uint8_t> clientPrivBytes;
    std::vector<uint8_t> clientPubExpectedBytes;
    if (!knx_test::vec::getHex(reqKv, "client_priv", clientPrivBytes)) return 5;
    if (!knx_test::vec::getHex(reqKv, "client_pub", clientPubExpectedBytes)) return 6;
    if (clientPrivBytes.size() != 32) return 7;

    knx::security::X25519::Scalar clientPrivateKey{};
    for (size_t i = 0; i < clientPrivateKey.size(); ++i) clientPrivateKey[i] = clientPrivBytes[i];

    SecureSession::PublicKey clientPub{};
    auto clientPubResult = SecureSessionBootstrap::deriveClientPublicKey(clientPrivateKey, clientPub);
    if (clientPubResult.isError()) return 8;

    SecureSession::PublicKey clientPubExpected{};
    if (!toFixed32(clientPubExpectedBytes, clientPubExpected)) return 9;
    if (clientPub != clientPubExpected) {
        std::cout << "client_public_key_mismatch\n";
        std::cout << "got=" << knx::util::toHex(clientPub) << "\n";
        std::cout << "expected=" << knx::util::toHex(clientPubExpected) << "\n";
        return 10;
    }

    std::vector<uint8_t> responseFrame;
    std::vector<uint8_t> passwordLatin1;
    std::vector<uint8_t> userIdBytes;
    std::vector<uint8_t> authWrappedExpected;
    std::vector<uint8_t> sessionKeyExpectedBytes;
    std::vector<uint8_t> sidBytes;
    std::vector<uint8_t> seqBytes;
    std::vector<uint8_t> serialBytes;
    if (!knx_test::vec::getHex(respKv, "frame", responseFrame)) return 11;
    if (!knx_test::vec::getHex(authPlainKv, "password_latin1", passwordLatin1)) return 12;
    if (!knx_test::vec::getHex(authPlainKv, "user_id", userIdBytes)) return 13;
    if (!knx_test::vec::getHex(authWrappedKv, "frame", authWrappedExpected)) return 14;
    if (!knx_test::vec::getHex(authWrappedKv, "session_key", sessionKeyExpectedBytes)) return 15;
    if (!knx_test::vec::getHex(authWrappedKv, "sid", sidBytes)) return 16;
    if (!knx_test::vec::getHex(authWrappedKv, "seq", seqBytes)) return 17;
    if (!knx_test::vec::getHex(authWrappedKv, "serial", serialBytes)) return 18;
    if (userIdBytes.size() != 1 || sidBytes.size() != 2) return 19;

    const UserId userId(userIdBytes[0]);
    const uint64_t initialSeq = seq48FromBytes(seqBytes);
    if (initialSeq == 0) return 20;

    std::array<uint8_t, 6> clientSerial{};
    if (!toFixed6(serialBytes, clientSerial)) return 21;

    std::array<uint8_t, SecureWrapper::kOverhead + SecureSession::kSessionAuthenticateFrameLen> authWrapped{};
    auto authRequestResult = SecureSessionBootstrap::buildAuthenticateRequest(clientPrivateKey,
                                                                              passwordLatin1,
                                                                              userId,
                                                                              clientSerial,
                                                                              initialSeq,
                                                                              clientPub,
                                                                              responseFrame,
                                                                              authWrapped);
    if (authRequestResult.isError()) return 22;

    const auto& authRequest = authRequestResult.value();
    const SessionId expectedSid(static_cast<uint16_t>((static_cast<uint16_t>(sidBytes[0]) << 8) | sidBytes[1]));
    if (authRequest.session.sessionId != expectedSid) return 23;
    if (authRequest.session.nextSequence != ((initialSeq + 1u) & 0xFFFFFFFFFFFFULL)) return 24;
    if (authRequest.wrappedFrameLength != authWrappedExpected.size()) return 25;

    std::array<uint8_t, 16> sessionKeyExpected{};
    if (!toFixed16(sessionKeyExpectedBytes, sessionKeyExpected)) return 26;
    if (authRequest.session.wrapperKey != sessionKeyExpected) {
        std::cout << "session_key_mismatch\n";
        std::cout << "got=" << knx::util::toHex(authRequest.session.wrapperKey) << "\n";
        std::cout << "expected=" << knx::util::toHex(sessionKeyExpected) << "\n";
        return 27;
    }

    const std::span<const uint8_t> authWrappedSpan(authWrapped.data(), authRequest.wrappedFrameLength);
    if (authWrappedSpan.size() != authWrappedExpected.size() ||
        !std::equal(authWrappedSpan.begin(), authWrappedSpan.end(), authWrappedExpected.begin())) {
        std::cout << "authenticate_wrapped_mismatch\n";
        std::cout << "got=" << knx::util::toHex(authWrappedSpan) << "\n";
        std::cout << "expected=" << knx::util::toHex(authWrappedExpected) << "\n";
        return 28;
    }

    std::array<uint8_t, SecureSession::kSessionStatusFrameLen> statusPlain{};
    if (SecureSession::encodeSessionStatus(0x00, statusPlain).isError()) return 29;

    std::array<uint8_t, SecureSession::kSessionStatusFrameLen> statusScratch{};
    auto plainStatusResult = SecureSessionBootstrap::decodeSessionStatus(authRequest.session.wrapperKey,
                                                                         authRequest.session.sessionId,
                                                                         statusPlain,
                                                                         statusScratch);
    // 03/08/09 §2.2.3.9: SESSION_STATUS "shall only be sent wrapped"; a plain
    // success code is not to be trusted.
    if (plainStatusResult.isOk()) return 30;

    const std::array<uint8_t, SecureWrapper::kTagLen> tag{0x00, 0x00};
    const std::array<uint8_t, 4> counterSuffix{0x00, 0x00, 0xFF, 0x00};
    const std::array<uint8_t, 6> serverSeq{0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
    const std::array<uint8_t, 6> serverSerial{0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};

    std::array<uint8_t, SecureWrapper::kOverhead + SecureSession::kSessionStatusFrameLen> wrappedStatus{};
    auto wrappedStatusResult = SecureWrapper::wrap(authRequest.session.wrapperKey,
                                                   authRequest.session.sessionId,
                                                   serverSeq,
                                                   serverSerial,
                                                   tag,
                                                   counterSuffix,
                                                   statusPlain,
                                                   wrappedStatus);
    if (wrappedStatusResult.isError()) return 31;

    std::array<uint8_t, SecureSession::kSessionStatusFrameLen> wrappedStatusScratch{};
    auto decodedWrappedStatus = SecureSessionBootstrap::decodeSessionStatus(authRequest.session.wrapperKey,
                                                                            authRequest.session.sessionId,
                                                                            std::span<const uint8_t>(wrappedStatus.data(), wrappedStatusResult.value()),
                                                                            wrappedStatusScratch);
    if (decodedWrappedStatus.isError() || decodedWrappedStatus.value() != 0x00) return 32;

    // The wrapper's sequence number is handed back: the first one received on
    // the session, against which later frames are checked for replay.
    uint64_t statusSequence = 0;
    if (SecureSessionBootstrap::decodeSessionStatus(authRequest.session.wrapperKey,
                                                    authRequest.session.sessionId,
                                                    std::span<const uint8_t>(wrappedStatus.data(), wrappedStatusResult.value()),
                                                    wrappedStatusScratch,
                                                    &statusSequence)
            .isError()
        || statusSequence != 2) {
        return 33;
    }

    // ---- SESSION_RESPONSE MAC (03/08/09 §2.2.3.7.4) -----------------------
    // Reference values from xknx: DAC of "trustme" (03/08/09 §2.3.1.3.1 PBKDF2)
    // and the MAC over header | session id 0001h | (X ^ Y) with
    // X = 01..20h, Y = A0..BFh.
    SecureSession::DeviceAuthenticationCode dac{};
    const std::string secret = "trustme";
    if (SecureSession::deriveDeviceAuthenticationCodeLatin1(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(secret.data()), secret.size()), dac)
            .isError()) {
        return 34;
    }
    const std::array<uint8_t, 16> expectedDac{0xe1, 0x58, 0xe4, 0x01, 0x20, 0x47, 0xbd, 0x6c,
                                              0xc4, 0x1a, 0xaf, 0xbc, 0x5c, 0x04, 0xc1, 0xfc};
    if (dac != expectedDac) return 35;

    SecureSession::PublicKey x{};
    SecureSession::PublicKey y{};
    for (size_t i = 0; i < x.size(); ++i) {
        x[i] = static_cast<uint8_t>(0x01 + i);
        y[i] = static_cast<uint8_t>(0xA0 + i);
    }
    SecureSession::MessageAuthenticationCode mac{};
    if (SecureSession::computeSessionResponseMac(dac, SessionId(0x0001), x, y, mac).isError()) return 36;
    const std::array<uint8_t, 16> expectedMac{0xad, 0x0d, 0x57, 0x34, 0x99, 0x57, 0xbf, 0x70,
                                              0xb5, 0x1c, 0x4b, 0x8d, 0xfa, 0xed, 0x80, 0xcb};
    if (mac != expectedMac) return 37;

    SecureSession::SessionResponse good{SessionId(0x0001), y, mac};
    if (!SecureSession::verifySessionResponseMac(dac, good, x)) return 38;
    SecureSession::SessionResponse tampered = good;
    tampered.serverPublicKey[5] ^= 0x01;  // a man in the middle substituting Y
    if (SecureSession::verifySessionResponseMac(dac, tampered, x)) return 39;

    // A client holding a DAC refuses a response whose MAC does not verify —
    // the vector response carries a zero MAC — before deriving any keys.
    std::array<uint8_t, SecureWrapper::kOverhead + SecureSession::kSessionAuthenticateFrameLen> refusedWrapped{};
    auto refused = SecureSessionBootstrap::buildAuthenticateRequest(clientPrivateKey, passwordLatin1, userId,
                                                                    clientSerial, initialSeq, clientPub,
                                                                    responseFrame, refusedWrapped, &dac);
    if (refused.isOk() || refused.error() != knx::util::ErrorCode::AccessDenied) return 40;

    // ...and accepts one whose MAC does.
    SecureSession::SessionResponse vectorResponse{};
    if (SecureSession::decodeSessionResponse(responseFrame, vectorResponse).isError()) return 41;
    SecureSession::MessageAuthenticationCode vectorMac{};
    if (SecureSession::computeSessionResponseMac(dac, vectorResponse.sessionId, clientPub,
                                                 vectorResponse.serverPublicKey, vectorMac)
            .isError()) {
        return 42;
    }
    std::vector<uint8_t> signedResponse = responseFrame;
    std::copy(vectorMac.begin(), vectorMac.end(), signedResponse.end() - 16);
    auto accepted = SecureSessionBootstrap::buildAuthenticateRequest(clientPrivateKey, passwordLatin1, userId,
                                                                     clientSerial, initialSeq, clientPub,
                                                                     signedResponse, refusedWrapped, &dac);
    if (accepted.isError()) return 43;

    // ---- Replay protection on the open session (03/08/09 §2.2.3.3) --------
    // Frames whose sequence number is not above the last one received are
    // discarded; a forged frame does not move the stored number.
    {
        using knx::netip::ip_secure::SecureTunnelingSecurity;
        const auto& key = authRequest.session.wrapperKey;
        const auto sessionId = authRequest.session.sessionId;
        SecureTunnelingSecurity security(key, sessionId, clientSerial, 0, /*lastReceivedSeq=*/statusSequence);

        const std::array<uint8_t, 8> inner{0x06, 0x10, 0x02, 0x08, 0x00, 0x08, 0x11, 0x00};
        const auto wrapAt = [&](uint8_t sequence, std::vector<uint8_t>& out) {
            out.assign(SecureWrapper::kOverhead + inner.size(), 0);
            auto wrapped = SecureWrapper::wrap(key, sessionId, {0, 0, 0, 0, 0, sequence}, serverSerial, tag,
                                               counterSuffix, inner, out);
            if (wrapped.isError()) return false;
            out.resize(wrapped.value());
            return true;
        };
        std::array<uint8_t, 64> plain{};
        std::vector<uint8_t> frame;

        if (!wrapAt(3, frame) || security.unprotect(frame, plain).isError()) return 44;
        if (security.unprotect(frame, plain).isOk()) return 45;  // the same frame again
        if (!wrapAt(2, frame) || security.unprotect(frame, plain).isOk()) return 46;  // older
        if (!wrapAt(9, frame)) return 47;
        frame[frame.size() - 1] ^= 0x01;  // MAC broken
        if (security.unprotect(frame, plain).isOk()) return 48;
        if (!wrapAt(5, frame) || security.unprotect(frame, plain).isError()) return 49;  // gaps are fine
    }

    return 0;
}

#else

int main() { return 0; }

#endif // KNX_SECURE_ENABLED