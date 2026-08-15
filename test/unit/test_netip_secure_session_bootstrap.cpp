// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_session_bootstrap.hpp"

#if KNX_SECURE_ENABLED

#include "knx/netip/ip_secure/secure_session.hpp"
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
    if (plainStatusResult.isError() || plainStatusResult.value() != 0x00) return 30;

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

    return 0;
}

#else

int main() { return 0; }

#endif // KNX_SECURE_ENABLED