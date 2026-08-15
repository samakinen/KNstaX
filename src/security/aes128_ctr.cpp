// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/security/aes128_ctr.hpp"

#include "knx/security/aes_block.hpp"

#include <cstring>

namespace knx {
namespace security {

util::Result<void> Aes128Ctr::crypt(const Key& key,
                                    const Counter& counter0,
                                    std::span<const uint8_t> in,
                                    std::span<uint8_t> out) {
    if (out.size() != in.size()) {
        return util::ErrorCode::BufferTooSmall;
    }

    Aes128BlockCipher aes;
    auto keyResult = aes.setKey(key);
    if (keyResult.isError()) {
        return keyResult.error();
    }

    // CTR built from block encryption rather than a backend CTR helper: the
    // counter is incremented over the whole 16 octets (Ctrj = Ctrj-1 + 1,
    // 03/03/07 §5.1.3.2), which is also what the Mbed TLS CTR helper did.
    std::array<uint8_t, Aes128BlockCipher::kBlockSize> counter{};
    std::memcpy(counter.data(), counter0.data(), counter.size());

    std::array<uint8_t, Aes128BlockCipher::kBlockSize> keyStream{};
    size_t offset = 0;
    while (offset < in.size()) {
        auto blockResult = aes.encryptBlock(counter.data(), keyStream.data());
        if (blockResult.isError()) {
            return blockResult.error();
        }

        const size_t chunk = std::min(keyStream.size(), in.size() - offset);
        for (size_t i = 0; i < chunk; ++i) {
            out[offset + i] = static_cast<uint8_t>(in[offset + i] ^ keyStream[i]);
        }
        offset += chunk;

        for (size_t i = counter.size(); i-- > 0;) {
            if (++counter[i] != 0) {
                break;
            }
        }
    }

    return util::Result<void>::ok();
}


} // namespace security
} // namespace knx
