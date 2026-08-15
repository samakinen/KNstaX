// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_session.hpp"
#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/security/x25519.hpp"
#include "knx/util/hex.hpp"

#include "../common/vec_file.hpp"

#include <array>
#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using knx::netip::ip_secure::SecureSession;
using knx::netip::ip_secure::SecureWrapper;
using knx::SessionId;
using knx::UserId;

static bool toFixed32(std::span<const uint8_t> in, std::array<uint8_t, 32>& out)
{
    if (in.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) out[i] = in[i];
    return true;
}

static bool toFixed16(std::span<const uint8_t> in, std::array<uint8_t, 16>& out)
{
    if (in.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) out[i] = in[i];
    return true;
}

static int runRequestVector()
{
    std::string text;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0951_request.vec", text)) return 1;

    std::map<std::string, std::string> kv;
    if (!knx_test::vec::parseVec(text, kv)) return 2;

    std::vector<uint8_t> clientPubBytes;
    std::vector<uint8_t> frameExpected;
    if (!knx_test::vec::getHex(kv, "client_pub", clientPubBytes)) return 3;
    if (!knx_test::vec::getHex(kv, "frame", frameExpected)) return 4;

    SecureSession::PublicKey clientPub{};
    if (!toFixed32(clientPubBytes, clientPub)) return 5;

    std::array<uint8_t, SecureSession::kSessionRequestFrameLen> got{};
    if (SecureSession::encodeSessionRequest(clientPub, got).isError()) return 6;

    if (frameExpected.size() != got.size() || !std::equal(got.begin(), got.end(), frameExpected.begin())) {
        std::cout << "request_mismatch\n";
        std::cout << "got=" << knx::util::toHex(got) << "\n";
        std::cout << "expected=" << knx::util::toHex(frameExpected) << "\n";
        return 7;
    }

    return 0;
}

static int runResponseVector()
{
    std::string text;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0952_response.vec", text)) return 30;

    std::map<std::string, std::string> kv;
    if (!knx_test::vec::parseVec(text, kv)) return 31;

    std::vector<uint8_t> sidBytes;
    std::vector<uint8_t> serverPubBytes;
    std::vector<uint8_t> macBytes;
    std::vector<uint8_t> frameExpected;
    if (!knx_test::vec::getHex(kv, "sid", sidBytes)) return 32;
    if (!knx_test::vec::getHex(kv, "server_pub", serverPubBytes)) return 33;
    if (!knx_test::vec::getHex(kv, "mac", macBytes)) return 34;
    if (!knx_test::vec::getHex(kv, "frame", frameExpected)) return 35;

    if (sidBytes.size() != 2) return 36;
    const SessionId sid(static_cast<uint16_t>((static_cast<uint16_t>(sidBytes[0]) << 8) | sidBytes[1]));

    SecureSession::PublicKey serverPub{};
    if (!toFixed32(serverPubBytes, serverPub)) return 37;

    std::array<uint8_t, 16> mac{};
    if (!toFixed16(macBytes, mac)) return 38;

    std::array<uint8_t, SecureSession::kSessionResponseFrameLen> got{};
    if (SecureSession::encodeSessionResponse(sid, serverPub, mac, got).isError()) return 39;
    if (frameExpected.size() != got.size() || !std::equal(got.begin(), got.end(), frameExpected.begin())) {
        std::cout << "response_mismatch\n";
        std::cout << "got=" << knx::util::toHex(got) << "\n";
        std::cout << "expected=" << knx::util::toHex(frameExpected) << "\n";
        return 40;
    }

    SecureSession::SessionResponse parsed;
    if (SecureSession::decodeSessionResponse(frameExpected, parsed).isError()) return 41;
    if (parsed.sessionId != sid) return 42;
    if (parsed.serverPublicKey != serverPub) return 43;
    if (parsed.messageAuthenticationCode != mac) return 44;

    return 0;
}

static int runAuthenticatePlainVector()
{
    std::string text;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_plain.vec", text)) return 50;

    std::map<std::string, std::string> kv;
    if (!knx_test::vec::parseVec(text, kv)) return 51;

    std::vector<uint8_t> userIdBytes;
    std::vector<uint8_t> passwordLatin1;
    std::vector<uint8_t> userPasswordKeyExpected;
    std::vector<uint8_t> clientPubBytes;
    std::vector<uint8_t> serverPubBytes;
    std::vector<uint8_t> frameExpected;

    if (!knx_test::vec::getHex(kv, "user_id", userIdBytes)) return 52;
    if (!knx_test::vec::getHex(kv, "password_latin1", passwordLatin1)) return 53;
    if (!knx_test::vec::getHex(kv, "user_password_key", userPasswordKeyExpected)) return 54;
    if (!knx_test::vec::getHex(kv, "client_pub", clientPubBytes)) return 55;
    if (!knx_test::vec::getHex(kv, "server_pub", serverPubBytes)) return 56;
    if (!knx_test::vec::getHex(kv, "frame", frameExpected)) return 57;

    if (userIdBytes.size() != 1) return 58;
    const UserId userId(userIdBytes[0]);

    SecureSession::UserPasswordKey userPasswordKey{};
    if (SecureSession::deriveUserPasswordKeyLatin1(passwordLatin1, userPasswordKey).isError()) return 59;

    if (userPasswordKeyExpected.size() != userPasswordKey.size()) return 60;
    for (size_t i = 0; i < userPasswordKey.size(); ++i) {
        if (userPasswordKey[i] != userPasswordKeyExpected[i]) {
            std::cout << "user_password_key_mismatch\n";
            std::cout << "got=" << knx::util::toHex(userPasswordKey) << "\n";
            std::cout << "expected=" << knx::util::toHex(userPasswordKeyExpected) << "\n";
            return 61;
        }
    }

    SecureSession::PublicKey clientPub{};
    SecureSession::PublicKey serverPub{};
    if (!toFixed32(clientPubBytes, clientPub)) return 62;
    if (!toFixed32(serverPubBytes, serverPub)) return 63;

    std::array<uint8_t, SecureSession::kSessionAuthenticateFrameLen> got{};
    if (SecureSession::encodeSessionAuthenticate(userPasswordKey, userId, clientPub, serverPub, got).isError()) return 64;
    if (frameExpected.size() != got.size() || !std::equal(got.begin(), got.end(), frameExpected.begin())) {
        std::cout << "authenticate_plain_mismatch\n";
        std::cout << "got=" << knx::util::toHex(got) << "\n";
        std::cout << "expected=" << knx::util::toHex(frameExpected) << "\n";
        return 65;
    }

    return 0;
}

static int runX25519AndSessionKeyVectors()
{
    std::string reqText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0951_request.vec", reqText)) return 70;
    std::map<std::string, std::string> reqKv;
    if (!knx_test::vec::parseVec(reqText, reqKv)) return 71;

    std::string respText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0952_response.vec", respText)) return 72;
    std::map<std::string, std::string> respKv;
    if (!knx_test::vec::parseVec(respText, respKv)) return 73;

    std::string wrappedText;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_wrapped.vec", wrappedText)) return 74;
    std::map<std::string, std::string> wrappedKv;
    if (!knx_test::vec::parseVec(wrappedText, wrappedKv)) return 75;

    std::vector<uint8_t> clientPrivBytes;
    std::vector<uint8_t> clientPubExpected;
    if (!knx_test::vec::getHex(reqKv, "client_priv", clientPrivBytes)) return 76;
    if (!knx_test::vec::getHex(reqKv, "client_pub", clientPubExpected)) return 77;
    if (clientPrivBytes.size() != 32) return 78;

    std::vector<uint8_t> serverPrivBytes;
    std::vector<uint8_t> serverPubExpected;
    if (!knx_test::vec::getHex(respKv, "server_priv", serverPrivBytes)) return 79;
    if (!knx_test::vec::getHex(respKv, "server_pub", serverPubExpected)) return 80;
    if (serverPrivBytes.size() != 32) return 81;

    std::vector<uint8_t> sessionKeyExpected;
    if (!knx_test::vec::getHex(wrappedKv, "session_key", sessionKeyExpected)) return 82;
    if (sessionKeyExpected.size() != 16) return 83;

    knx::security::X25519::Scalar clientPriv{};
    knx::security::X25519::Scalar serverPriv{};
    for (size_t i = 0; i < 32; ++i) {
        clientPriv[i] = clientPrivBytes[i];
        serverPriv[i] = serverPrivBytes[i];
    }

    knx::security::X25519::PublicKey clientPub{};
    knx::security::X25519::PublicKey serverPub{};
    if (knx::security::X25519::publicFromPrivate(clientPriv, clientPub).isError()) return 84;
    if (knx::security::X25519::publicFromPrivate(serverPriv, serverPub).isError()) return 85;

    if (clientPubExpected.size() != 32 || serverPubExpected.size() != 32) return 86;
    for (size_t i = 0; i < 32; ++i) {
        if (clientPub[i] != clientPubExpected[i]) {
            std::cout << "client_pub_mismatch\n";
            return 87;
        }
        if (serverPub[i] != serverPubExpected[i]) {
            std::cout << "server_pub_mismatch\n";
            return 88;
        }
    }

    SecureSession::SessionKey sessionKey{};
    SecureSession::PublicKey serverPubForSession{};
    for (size_t i = 0; i < 32; ++i) serverPubForSession[i] = serverPub[i];
    if (SecureSession::deriveSessionKey(clientPriv, serverPubForSession, sessionKey).isError()) return 89;

    for (size_t i = 0; i < sessionKey.size(); ++i) {
        if (sessionKey[i] != sessionKeyExpected[i]) {
            std::cout << "session_key_mismatch\n";
            std::cout << "got=" << knx::util::toHex(sessionKey) << "\n";
            std::cout << "expected=" << knx::util::toHex(sessionKeyExpected) << "\n";
            return 90;
        }
    }

    // Sanity: shared secrets should match (client->server vs server->client)
    knx::security::X25519::SharedSecret sec1{};
    knx::security::X25519::SharedSecret sec2{};
    if (knx::security::X25519::sharedSecret(clientPriv, serverPub, sec1).isError()) return 91;
    if (knx::security::X25519::sharedSecret(serverPriv, clientPub, sec2).isError()) return 92;
    if (sec1 != sec2) return 93;

    return 0;
}

static int runAuthenticateVectors()
{
    std::string text;
    if (!knx_test::vec::readTextFile("test/vectors/knxnetip_secure_session/secure_session_0953_authenticate_wrapped.vec", text)) return 10;

    std::map<std::string, std::string> kv;
    if (!knx_test::vec::parseVec(text, kv)) return 11;

    std::vector<uint8_t> sessionKeyBytes;
    std::vector<uint8_t> sidBytes;
    std::vector<uint8_t> seqBytes;
    std::vector<uint8_t> serialBytes;
    std::vector<uint8_t> tagBytes;
    std::vector<uint8_t> innerExpected;
    std::vector<uint8_t> frameExpected;

    if (!knx_test::vec::getHex(kv, "session_key", sessionKeyBytes)) return 12;
    if (!knx_test::vec::getHex(kv, "sid", sidBytes)) return 13;
    if (!knx_test::vec::getHex(kv, "seq", seqBytes)) return 14;
    if (!knx_test::vec::getHex(kv, "serial", serialBytes)) return 15;
    if (!knx_test::vec::getHex(kv, "tag", tagBytes)) return 16;
    if (!knx_test::vec::getHex(kv, "inner", innerExpected)) return 17;
    if (!knx_test::vec::getHex(kv, "frame", frameExpected)) return 18;

    if (sessionKeyBytes.size() != 16) return 19;
    if (sidBytes.size() != 2) return 20;
    if (seqBytes.size() != SecureWrapper::kSeqLen) return 21;
    if (serialBytes.size() != SecureWrapper::kSerialLen) return 22;
    if (tagBytes.size() != SecureWrapper::kTagLen) return 23;

    SecureWrapper::Key sessionKey{};
    for (size_t i = 0; i < 16; ++i) sessionKey[i] = sessionKeyBytes[i];

    const SessionId sid(static_cast<uint16_t>((static_cast<uint16_t>(sidBytes[0]) << 8) | sidBytes[1]));

    std::array<uint8_t, SecureWrapper::kSeqLen> seq{};
    for (size_t i = 0; i < seq.size(); ++i) seq[i] = seqBytes[i];

    std::array<uint8_t, SecureWrapper::kSerialLen> serial{};
    for (size_t i = 0; i < serial.size(); ++i) serial[i] = serialBytes[i];

    std::array<uint8_t, SecureWrapper::kTagLen> tag{};
    for (size_t i = 0; i < tag.size(); ++i) tag[i] = tagBytes[i];

    const std::array<uint8_t, 4> counterSuffix{0x00, 0x00, 0xFF, 0x00};

    std::vector<uint8_t> wrapped(SecureWrapper::kOverhead + innerExpected.size());
    auto wrapResult = SecureWrapper::wrap(sessionKey, sid, seq, serial, tag, counterSuffix, innerExpected, wrapped);
    if (wrapResult.isError()) return 24;
    wrapped.resize(wrapResult.value());

    if (wrapped != frameExpected) {
        std::cout << "auth_wrapped_mismatch\n";
        std::cout << "got=" << knx::util::toHex(wrapped) << "\n";
        std::cout << "expected=" << knx::util::toHex(frameExpected) << "\n";
        return 25;
    }

    std::vector<uint8_t> plain(wrapped.size() - 22);
    auto unwrapResult = SecureWrapper::unwrapAndVerify(sessionKey, sid, wrapped, plain);
    if (unwrapResult.isError()) return 26;
    if (unwrapResult.value() != innerExpected.size()) return 27;
    if (!std::equal(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(unwrapResult.value()), innerExpected.begin())) return 28;

    return 0;
}

int main()
{
    int r = 0;

    r = runRequestVector();
    if (r != 0) return r;

    r = runResponseVector();
    if (r != 0) return r;

    r = runAuthenticatePlainVector();
    if (r != 0) return r;

    r = runX25519AndSessionKeyVectors();
    if (r != 0) return r;

    r = runAuthenticateVectors();
    if (r != 0) return r;

    return 0;
}
