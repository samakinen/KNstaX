// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/security/key_derivation.hpp"

#include <array>
#include <limits>

// Key derivation backend: mbedTLS PBKDF2-SHA256 only (KNX-compliant)
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"

namespace knx {
namespace security {

namespace {

constexpr uint32_t kKnxPbkdf2Iterations = 10000;
constexpr mbedtls_md_type_t kPbkdf2DigestType = MBEDTLS_MD_SHA256;

util::Result<void> pbkdf2Sha256(std::span<const uint8_t> password,
                                std::span<const uint8_t> salt,
                                uint32_t iterations,
                                std::span<uint8_t> output)
{
    if (output.size() > std::numeric_limits<uint32_t>::max()) return util::ErrorCode::InvalidParameter;

    const uint32_t outLen32 = static_cast<uint32_t>(output.size());

#ifdef ESP_PLATFORM
    const int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
        kPbkdf2DigestType,
        password.data(), password.size(),
        salt.data(), salt.size(),
        iterations,
        outLen32,
        output.data());
    if (ret != 0) {
        return util::ErrorCode::OperationFailed;
    }
    return util::Result<void>::ok();
#else
    mbedtls_md_context_t md_ctx;
    mbedtls_md_init(&md_ctx);

    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(kPbkdf2DigestType);
    if (!md_info) {
        mbedtls_md_free(&md_ctx);
        return util::ErrorCode::OperationNotSupported;
    }

    const int setupRet = mbedtls_md_setup(&md_ctx, md_info, 1);
    if (setupRet != 0) {
        mbedtls_md_free(&md_ctx);
        return util::ErrorCode::OperationFailed;
    }

    const int ret = mbedtls_pkcs5_pbkdf2_hmac(
        &md_ctx,
        password.data(), password.size(),
        salt.data(), salt.size(),
        iterations,
        outLen32,
        output.data());

    mbedtls_md_free(&md_ctx);
    if (ret != 0) {
        return util::ErrorCode::OperationFailed;
    }
    return util::Result<void>::ok();
#endif
}

} // namespace

util::Result<void> KeyDerivation::pbkdf2(std::span<const uint8_t> password,
                           std::span<const uint8_t> salt,
                           uint32_t iterations,
                           std::span<uint8_t> output) {
    return pbkdf2Sha256(password, salt, iterations, output);
}

util::Result<void> KeyDerivation::deriveSessionKey(const MasterKey& master_key,
                                     const DeviceId& device_id,
                                     Key& output_key) {
    std::array<uint8_t, 2> salt{device_id[1], device_id[0]};
    std::span<const uint8_t> password(master_key);

    return pbkdf2(password,
                  salt,
                  kKnxPbkdf2Iterations,
                  output_key);
}

util::Result<void> KeyDerivation::deriveBilateralKeys(const MasterKey& master_key,
                                        const DeviceId& device_id,
                                        const DeviceId& coupler_id,
                                        Key& device_tx_key,
                                        Key& device_rx_key) {
    std::array<uint8_t, 4> tx_salt{device_id[1], device_id[0], coupler_id[1], coupler_id[0]};
    std::array<uint8_t, 4> rx_salt{coupler_id[1], coupler_id[0], device_id[1], device_id[0]};

    std::span<const uint8_t> password(master_key);

    auto txResult = pbkdf2(password,
                           tx_salt,
                           kKnxPbkdf2Iterations,
                           device_tx_key);
    if (txResult.isError()) {
        return txResult.error();
    }

    auto rxResult = pbkdf2(password,
                           rx_salt,
                           kKnxPbkdf2Iterations,
                           device_rx_key);
    if (rxResult.isError()) {
        return rxResult.error();
    }

    return util::Result<void>::ok();
}

}  // namespace security
}  // namespace knx
