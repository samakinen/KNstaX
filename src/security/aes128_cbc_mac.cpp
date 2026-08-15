// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/security/aes128_cbc_mac.hpp"

#include "knx/security/aes_block.hpp"

#include <algorithm>
#include <cstring>

namespace knx {
namespace security {

util::Result<void> Aes128CbcMac::compute(const Key& key,
                                         const Block& block0,
                                         std::span<const uint8_t> additionalData,
                                         std::span<const uint8_t> payload,
                                         Block& macOut) {
    Aes128BlockCipher aes;
    auto keyResult = aes.setKey(key);
    if (keyResult.isError()) {
        return keyResult.error();
    }

    Block chain{};
    Block block{};

    const auto processBlock = [&](std::span<const uint8_t, 16> input) -> util::Result<void> {
        for (size_t i = 0; i < 16; ++i) {
            block[i] = static_cast<uint8_t>(input[i] ^ chain[i]);
        }
        return aes.encryptBlock(block.data(), chain.data());
    };

    auto processResult = processBlock(std::span<const uint8_t, 16>(block0));
    if (processResult.isError()) {
        return processResult.error();
    }

    size_t blockFill = 0;
    const auto appendByte = [&](uint8_t value) -> util::Result<void> {
        block[blockFill++] = value;
        if (blockFill != block.size()) {
            return util::Result<void>::ok();
        }
        auto result = processBlock(std::span<const uint8_t, 16>(block));
        blockFill = 0;
        return result;
    };

    auto appendBytes = [&](std::span<const uint8_t> bytes) -> util::Result<void> {
        for (uint8_t byte : bytes) {
            auto result = appendByte(byte);
            if (result.isError()) {
                return result;
            }
        }
        return util::Result<void>::ok();
    };

    auto lengthHigh = static_cast<uint8_t>((additionalData.size() >> 8) & 0xFFu);
    auto lengthLow = static_cast<uint8_t>(additionalData.size() & 0xFFu);
    processResult = appendByte(lengthHigh);
    if (processResult.isError()) {
        return processResult.error();
    }
    processResult = appendByte(lengthLow);
    if (processResult.isError()) {
        return processResult.error();
    }
    processResult = appendBytes(additionalData);
    if (processResult.isError()) {
        return processResult.error();
    }
    processResult = appendBytes(payload);
    if (processResult.isError()) {
        return processResult.error();
    }

    if (blockFill != 0) {
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(blockFill), block.end(), 0);
        processResult = processBlock(std::span<const uint8_t, 16>(block));
        if (processResult.isError()) {
            return processResult.error();
        }
    }

    std::memcpy(macOut.data(), chain.data(), 16);
    return util::Result<void>::ok();
}

} // namespace security
} // namespace knx
