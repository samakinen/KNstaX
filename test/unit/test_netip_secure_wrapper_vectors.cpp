// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_wrapper.hpp"
#include "knx/util/hex.hpp"

#include "../common/vec_file.hpp"

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <algorithm>
#include <span>

using knx::netip::ip_secure::SecureWrapper;
using knx::SessionId;

static bool toFixed(std::span<const uint8_t> in, std::span<uint8_t> out)
{
    if (in.size() != out.size()) return false;
    for (size_t i = 0; i < out.size(); ++i) out[i] = in[i];
    return true;
}

static int runOne(const std::string& path, bool expectSessionNonZero)
{
    std::string text;
    if (!knx_test::vec::readTextFile(path, text)) return 1;

    std::map<std::string, std::string> kv;
    if (!knx_test::vec::parseVec(text, kv)) return 2;

    std::vector<uint8_t> keyBytes;
    std::vector<uint8_t> sidBytes;
    std::vector<uint8_t> seqBytes;
    std::vector<uint8_t> serialBytes;
    std::vector<uint8_t> tagBytes;
    std::vector<uint8_t> innerBytes;
    std::vector<uint8_t> frameExpected;

    if (!knx_test::vec::getHex(kv, "key", keyBytes)) return 3;
    if (!knx_test::vec::getHex(kv, "sid", sidBytes)) return 4;
    if (!knx_test::vec::getHex(kv, "seq", seqBytes)) return 5;
    if (!knx_test::vec::getHex(kv, "serial", serialBytes)) return 6;
    if (!knx_test::vec::getHex(kv, "tag", tagBytes)) return 7;
    if (!knx_test::vec::getHex(kv, "inner", innerBytes)) return 8;
    if (!knx_test::vec::getHex(kv, "frame", frameExpected)) return 9;

    if (keyBytes.size() != 16) return 10;
    if (sidBytes.size() != 2) return 11;
    if (seqBytes.size() != SecureWrapper::kSeqLen) return 12;
    if (serialBytes.size() != SecureWrapper::kSerialLen) return 13;
    if (tagBytes.size() != SecureWrapper::kTagLen) return 14;

    const SessionId sid(static_cast<uint16_t>((static_cast<uint16_t>(sidBytes[0]) << 8) | sidBytes[1]));
    if (expectSessionNonZero && !sid.isValid()) return 15;
    if (!expectSessionNonZero && sid.isValid()) return 16;

    SecureWrapper::Key key{};
    for (size_t i = 0; i < 16; ++i) key[i] = keyBytes[i];

    std::array<uint8_t, SecureWrapper::kSeqLen> seq{};
    std::array<uint8_t, SecureWrapper::kSerialLen> serial{};
    std::array<uint8_t, SecureWrapper::kTagLen> tag{};

    if (!toFixed(seqBytes, seq)) return 17;
    if (!toFixed(serialBytes, serial)) return 18;
    if (!toFixed(tagBytes, tag)) return 19;

    std::array<uint8_t, 4> counterSuffix{};
    if (sid.isValid()) {
        // tunnelling
        counterSuffix = {0x00, 0x00, 0xFF, 0x00};
    } else {
        // routing
        counterSuffix = {tag[0], tag[1], 0xFF, 0x00};
    }

    std::vector<uint8_t> frameGot(SecureWrapper::kOverhead + innerBytes.size());
    auto wrapResult = SecureWrapper::wrap(key, sid, seq, serial, tag, counterSuffix, innerBytes, frameGot);
    if (wrapResult.isError()) return 20;
    frameGot.resize(wrapResult.value());

    if (frameGot != frameExpected) {
        std::cout << "frame_mismatch path=" << path << "\n";
        std::cout << "got=" << knx::util::toHex(frameGot) << "\n";
        std::cout << "expected=" << knx::util::toHex(frameExpected) << "\n";
        return 21;
    }

    std::vector<uint8_t> plain(frameGot.size() - 22);
    auto unwrapResult = SecureWrapper::unwrapAndVerify(key, sid, frameGot, plain);
    if (unwrapResult.isError()) {
        std::cout << "unwrap_failed path=" << path << "\n";
        return 22;
    }

    const std::span<const uint8_t> plainSpan(plain.data(), unwrapResult.value());
    if (plainSpan.size() != innerBytes.size() || !std::equal(plainSpan.begin(), plainSpan.end(), innerBytes.begin())) {
        std::cout << "inner_mismatch path=" << path << "\n";
        std::cout << "got=" << knx::util::toHex(plainSpan) << "\n";
        std::cout << "expected=" << knx::util::toHex(innerBytes) << "\n";
        return 23;
    }

    return 0;
}

int main()
{
    int r = 0;
    r = runOne("test/vectors/knxnetip_secure_wrapper/secure_wrapper_0950_tunnel.vec", true);
    if (r != 0) return r;

    r = runOne("test/vectors/knxnetip_secure_wrapper/secure_wrapper_0950_routing.vec", false);
    if (r != 0) return r;

    return 0;
}
