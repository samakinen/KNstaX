// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/security/aes128_cbc_mac.hpp"
#include "knx/security/aes128_ctr.hpp"
#include "knx/util/hex.hpp"

#include "../common/vec_file.hpp"

#include <iostream>
#include <map>
#include <string>
#include <vector>

using knx::security::Aes128CbcMac;
using knx::security::Aes128Ctr;

static bool toFixed16(std::span<const uint8_t> in, std::array<uint8_t, 16>& out) {
    if (in.size() != 16) return false;
    for (size_t i = 0; i < 16; ++i) out[i] = in[i];
    return true;
}

int main() {
    const std::string path = "test/vectors/knxnetip_secure_wrapper/timernotify_0955_capture.vec";

    std::string text;
    if (!knx_test::vec::readTextFile(path, text)) return 1;

    std::map<std::string, std::string> kv;
    if (!knx_test::vec::parseVec(text, kv)) return 2;

    std::vector<uint8_t> keyBytes;
    std::vector<uint8_t> header;
    std::vector<uint8_t> timer;
    std::vector<uint8_t> serial;
    std::vector<uint8_t> tag;
    std::vector<uint8_t> macEncExpected;
    std::vector<uint8_t> frameExpected;

    if (!knx_test::vec::getHex(kv, "key", keyBytes)) return 3;
    if (!knx_test::vec::getHex(kv, "header", header)) return 4;
    if (!knx_test::vec::getHex(kv, "timer", timer)) return 5;
    if (!knx_test::vec::getHex(kv, "serial", serial)) return 6;
    if (!knx_test::vec::getHex(kv, "tag", tag)) return 7;
    if (!knx_test::vec::getHex(kv, "mac_enc", macEncExpected)) return 8;
    if (!knx_test::vec::getHex(kv, "frame", frameExpected)) return 9;

    if (keyBytes.size() != 16) return 10;
    if (header.size() != 6) return 11;
    if (timer.size() != 6) return 12;
    if (serial.size() != 6) return 13;
    if (tag.size() != 2) return 14;
    if (macEncExpected.size() != 16) return 15;

    Aes128CbcMac::Key key{};
    for (size_t i = 0; i < 16; ++i) key[i] = keyBytes[i];

    Aes128CbcMac::Block block0{};
    // block0 = timer(6) || serial(6) || tag(2) || 00 00
    for (size_t i = 0; i < 6; ++i) block0[i] = timer[i];
    for (size_t i = 0; i < 6; ++i) block0[6 + i] = serial[i];
    block0[12] = tag[0];
    block0[13] = tag[1];
    block0[14] = 0x00;
    block0[15] = 0x00;

    std::vector<uint8_t> payload; // TimerNotify has no encrypted payload

    Aes128CbcMac::Block macCbc{};
    if (Aes128CbcMac::compute(key, block0, header, payload, macCbc).isError()) return 16;

    // counter0 = timer(6) || serial(6) || tag(2) || ff 00
    Aes128Ctr::Counter ctr0{};
    for (size_t i = 0; i < 6; ++i) ctr0[i] = timer[i];
    for (size_t i = 0; i < 6; ++i) ctr0[6 + i] = serial[i];
    ctr0[12] = tag[0];
    ctr0[13] = tag[1];
    ctr0[14] = 0xFF;
    ctr0[15] = 0x00;

    std::vector<uint8_t> macCbcVec(macCbc.begin(), macCbc.end());
    std::vector<uint8_t> macEncGot(macCbc.size());
    if (Aes128Ctr::crypt(key, ctr0, macCbcVec, macEncGot).isError()) return 17;

    if (macEncGot != macEncExpected) {
        std::cout << "mac_mismatch\n";
        std::cout << "got=" << knx::util::toHex(macEncGot) << "\n";
        std::cout << "expected=" << knx::util::toHex(macEncExpected) << "\n";
        std::cout << "mac_cbc=" << knx::util::toHex(macCbcVec) << "\n";
        return 18;
    }

    std::vector<uint8_t> frame;
    frame.reserve(6 + 6 + 6 + 2 + 16);
    frame.insert(frame.end(), header.begin(), header.end());
    frame.insert(frame.end(), timer.begin(), timer.end());
    frame.insert(frame.end(), serial.begin(), serial.end());
    frame.insert(frame.end(), tag.begin(), tag.end());
    frame.insert(frame.end(), macEncGot.begin(), macEncGot.end());

    if (frame != frameExpected) {
        std::cout << "frame_mismatch\n";
        std::cout << "got=" << knx::util::toHex(frame) << "\n";
        std::cout << "expected=" << knx::util::toHex(frameExpected) << "\n";
        return 19;
    }

    return 0;
}
