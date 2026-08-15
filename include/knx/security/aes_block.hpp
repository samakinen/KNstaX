// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file aes_block.hpp
 * @brief Single-block AES-128 encryption, abstracted over the available backend.
 *
 * The KNX Data Secure primitives (CCM: AES-CBC-MAC for authentication,
 * AES-CTR for confidentiality) only ever need "encrypt one 16-octet block".
 * Which library provides that differs per build:
 *
 *  - Mbed TLS 2.x/3.x exposes the legacy `mbedtls/aes.h` block API.
 *  - Mbed TLS 4.x (ESP-IDF v6.x) removed it in favour of PSA Crypto, but
 *    ESP-IDF still ships the hardware-accelerated `aes/esp_aes.h` port with the
 *    same shape.
 *
 * Keeping the choice here means the CCM code stays backend-agnostic, and CTR is
 * built from block encryption rather than from a backend-specific CTR helper.
 */

#include "knx/util/result.hpp"

#include <array>
#include <cstdint>

#if defined(__has_include)
#  if __has_include(<mbedtls/aes.h>)
#    define KNX_AES_BACKEND_MBEDTLS 1
#  elif __has_include(<aes/esp_aes.h>)
#    define KNX_AES_BACKEND_ESP 1
#  endif
#else
#  define KNX_AES_BACKEND_MBEDTLS 1
#endif

#if defined(KNX_AES_BACKEND_MBEDTLS)
#  include <mbedtls/aes.h>
#elif defined(KNX_AES_BACKEND_ESP)
#  include <aes/esp_aes.h>
#else
#  error "KNX Secure requires an AES backend: mbedtls/aes.h or aes/esp_aes.h"
#endif

namespace knx {
namespace security {

/// AES-128 in encrypt-only, single-block mode. Not copyable: it owns a context.
class Aes128BlockCipher {
public:
    static constexpr size_t kBlockSize = 16;
    using Block = std::array<uint8_t, kBlockSize>;
    using Key = std::array<uint8_t, 16>;

    Aes128BlockCipher() {
#if defined(KNX_AES_BACKEND_MBEDTLS)
        mbedtls_aes_init(&_ctx);
#else
        esp_aes_init(&_ctx);
#endif
    }

    ~Aes128BlockCipher() {
#if defined(KNX_AES_BACKEND_MBEDTLS)
        mbedtls_aes_free(&_ctx);
#else
        esp_aes_free(&_ctx);
#endif
    }

    Aes128BlockCipher(const Aes128BlockCipher&) = delete;
    Aes128BlockCipher& operator=(const Aes128BlockCipher&) = delete;

    util::Result<void> setKey(const Key& key) {
#if defined(KNX_AES_BACKEND_MBEDTLS)
        const int ret = mbedtls_aes_setkey_enc(&_ctx, key.data(), 128);
#else
        const int ret = esp_aes_setkey(&_ctx, key.data(), 128);
#endif
        if (ret != 0) {
            return util::ErrorCode::OperationFailed;
        }
        return util::Result<void>::ok();
    }

    util::Result<void> encryptBlock(const uint8_t* input, uint8_t* output) {
#if defined(KNX_AES_BACKEND_MBEDTLS)
        const int ret = mbedtls_aes_crypt_ecb(&_ctx, MBEDTLS_AES_ENCRYPT, input, output);
#else
        const int ret = esp_aes_crypt_ecb(&_ctx, ESP_AES_ENCRYPT, input, output);
#endif
        if (ret != 0) {
            return util::ErrorCode::OperationFailed;
        }
        return util::Result<void>::ok();
    }

private:
#if defined(KNX_AES_BACKEND_MBEDTLS)
    mbedtls_aes_context _ctx{};
#else
    esp_aes_context _ctx{};
#endif
};

} // namespace security
} // namespace knx
