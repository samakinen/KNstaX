// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/netip_security.hpp"
#include "knx/util/result.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

class SecurePacketCodec {
public:
    SecurePacketCodec(NetIpSecurity& security,
                      std::span<uint8_t> wrapScratch,
                      std::span<uint8_t> unwrapScratch) noexcept
        : security_(security)
        , wrapScratch_(wrapScratch)
        , unwrapScratch_(unwrapScratch)
    {
    }

    util::Result<std::span<const uint8_t>> wrap(std::span<const uint8_t> plain)
    {
        if (wrapScratch_.empty()) return util::ErrorCode::BufferTooSmall;
        auto protectResult = security_.protect(plain, wrapScratch_);
        if (protectResult.isError()) return protectResult.error();
        return std::span<const uint8_t>(wrapScratch_).first(protectResult.value());
    }

    util::Result<size_t> unwrapInto(std::span<const uint8_t> secured, std::span<uint8_t> plainOut)
    {
        auto unprotectResult = security_.unprotect(secured, plainOut);
        if (unprotectResult.isError()) return unprotectResult.error();
        return unprotectResult.value();
    }

    util::Result<size_t> unwrapInPlace(std::span<const uint8_t> secured, std::span<uint8_t> plainBuffer)
    {
        if (unwrapScratch_.empty()) return util::ErrorCode::BufferTooSmall;
        auto unprotectResult = security_.unprotect(secured, unwrapScratch_);
        if (unprotectResult.isError()) return unprotectResult.error();

        const size_t plainLen = unprotectResult.value();
        if (plainLen > plainBuffer.size()) return util::ErrorCode::BufferTooSmall;
        std::copy_n(unwrapScratch_.begin(), plainLen, plainBuffer.begin());
        return plainLen;
    }

private:
    NetIpSecurity& security_;
    std::span<uint8_t> wrapScratch_;
    std::span<uint8_t> unwrapScratch_;
};

} // namespace netip
} // namespace knx