// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/security/sha256.hpp"

#if KNX_SECURE_ENABLED

#include <mbedtls/md.h>
#include <span>

namespace knx {
namespace security {

util::Result<void> Sha256::hash(std::span<const uint8_t> data, Digest& out)
{
    if (data.empty()) return util::ErrorCode::InvalidParameter;

    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md) return util::ErrorCode::OperationNotSupported;

    if (mbedtls_md(md, data.data(), data.size(), out.data()) != 0) {
        return util::ErrorCode::OperationFailed;
    }

    return util::Result<void>::ok();
}

} // namespace security
} // namespace knx

#endif // KNX_SECURE_ENABLED
