// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file aes_block.hpp
 * @brief Single-block AES-128 encryption over the configured backend.
 *
 * The KNX Data Secure primitives (CCM: AES-CBC-MAC for authentication,
 * AES-CTR for confidentiality) only ever need "encrypt one 16-octet block".
 * The configuration chooses who provides it (Kconfig KNX_AES_BACKEND):
 *
 *  - SOFTWARE: the portable Aes128Soft (aes128_soft.hpp). No library, every
 *    platform; the default on bare metal and on an ESP32 without the AES block.
 *  - MBEDTLS: the legacy `mbedtls/aes.h` block API of Mbed TLS 2.x/3.x
 *    (Mbed TLS 4 removed it). The host test profiles use it.
 *  - ESP_HW: ESP-IDF's `aes/esp_aes.h` driver of the AES peripheral; the
 *    default on an ESP32 that has one.
 *
 * Keeping the choice here means the CCM code stays backend-agnostic, and CTR is
 * built from block encryption rather than from a backend-specific CTR helper.
 */

#include "knx/config.hpp"
#include "knx/util/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

#if defined(CONFIG_KNX_AES_BACKEND_SOFTWARE)
#  include "knx/security/aes128_soft.hpp"
#elif defined(CONFIG_KNX_AES_BACKEND_MBEDTLS)
#  include <mbedtls/aes.h>
#elif defined(CONFIG_KNX_AES_BACKEND_ESP_HW)
#  include <aes/esp_aes.h>
#else
#  error "KNX Secure: no AES backend configured (Kconfig KNX_AES_BACKEND: SOFTWARE, MBEDTLS or ESP_HW)"
#endif

namespace knx {
namespace security {

/// AES-128 in encrypt-only, single-block mode. Not copyable: it owns a context.
class Aes128BlockCipher {
public:
    static constexpr size_t kBlockSize = 16;
    using Block = std::array<uint8_t, kBlockSize>;
    using Key = std::array<uint8_t, 16>;

#if defined(CONFIG_KNX_AES_BACKEND_SOFTWARE)
    Aes128BlockCipher() = default;
    ~Aes128BlockCipher() = default;
#elif defined(CONFIG_KNX_AES_BACKEND_MBEDTLS)
    Aes128BlockCipher() { mbedtls_aes_init(&_ctx); }
    ~Aes128BlockCipher() { mbedtls_aes_free(&_ctx); }
#else
    Aes128BlockCipher() { esp_aes_init(&_ctx); }
    ~Aes128BlockCipher() { esp_aes_free(&_ctx); }
#endif

    Aes128BlockCipher(const Aes128BlockCipher&) = delete;
    Aes128BlockCipher& operator=(const Aes128BlockCipher&) = delete;

    /// The software backend cannot fail; the library backends report their errors.
    util::Result<void> setKey(const Key& key) {
#if defined(CONFIG_KNX_AES_BACKEND_SOFTWARE)
        _ctx.setKey(key);
        return util::Result<void>::ok();
#else
#  if defined(CONFIG_KNX_AES_BACKEND_MBEDTLS)
        const int ret = mbedtls_aes_setkey_enc(&_ctx, key.data(), 128);
#  else
        const int ret = esp_aes_setkey(&_ctx, key.data(), 128);
#  endif
        if (ret != 0) {
            return util::ErrorCode::OperationFailed;
        }
        return util::Result<void>::ok();
#endif
    }

    util::Result<void> encryptBlock(const uint8_t* input, uint8_t* output) {
#if defined(CONFIG_KNX_AES_BACKEND_SOFTWARE)
        _ctx.encrypt(input, output);
        return util::Result<void>::ok();
#else
#  if defined(CONFIG_KNX_AES_BACKEND_MBEDTLS)
        const int ret = mbedtls_aes_crypt_ecb(&_ctx, MBEDTLS_AES_ENCRYPT, input, output);
#  else
        const int ret = esp_aes_crypt_ecb(&_ctx, ESP_AES_ENCRYPT, input, output);
#  endif
        if (ret != 0) {
            return util::ErrorCode::OperationFailed;
        }
        return util::Result<void>::ok();
#endif
    }

private:
#if defined(CONFIG_KNX_AES_BACKEND_SOFTWARE)
    Aes128Soft _ctx;
#elif defined(CONFIG_KNX_AES_BACKEND_MBEDTLS)
    mbedtls_aes_context _ctx{};
#else
    esp_aes_context _ctx{};
#endif
};

} // namespace security
} // namespace knx
