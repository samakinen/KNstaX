// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#include "knx/netip/ip_secure/secure_routing_security.hpp"

#if KNX_SECURE_ENABLED

#include "knx/netip/ip_secure/secure_wrapper.hpp"

namespace knx {
namespace netip {
namespace ip_secure {

SecureRoutingSecurity::SecureRoutingSecurity(const Key& groupKey,
                                             const std::array<uint8_t, SecureWrapper::kSerialLen>& serial,
                                             const std::array<uint8_t, SecureWrapper::kTagLen>& tag,
                                             uint64_t initialSeq)
    : groupKey_(groupKey)
    , serial_(serial)
    , tag_(tag)
    , seq48_(initialSeq & 0xFFFFFFFFFFFFULL)
{
}

void SecureRoutingSecurity::encodeSeq48BE_(uint64_t seq48, std::array<uint8_t, SecureWrapper::kSeqLen>& out)
{
    seq48 &= 0xFFFFFFFFFFFFULL;
    out[0] = static_cast<uint8_t>((seq48 >> 40) & 0xFF);
    out[1] = static_cast<uint8_t>((seq48 >> 32) & 0xFF);
    out[2] = static_cast<uint8_t>((seq48 >> 24) & 0xFF);
    out[3] = static_cast<uint8_t>((seq48 >> 16) & 0xFF);
    out[4] = static_cast<uint8_t>((seq48 >> 8) & 0xFF);
    out[5] = static_cast<uint8_t>(seq48 & 0xFF);
}

util::Result<size_t> SecureRoutingSecurity::protect(std::span<const uint8_t> in, std::span<uint8_t> out)
{
    const uint64_t seq = seq48_.fetch_add(1) & 0xFFFFFFFFFFFFULL;
    std::array<uint8_t, SecureWrapper::kSeqLen> seqBytes{};
    encodeSeq48BE_(seq, seqBytes);

    const std::array<uint8_t, 4> counterSuffix{tag_[0], tag_[1], 0xFF, 0x00};

    return SecureWrapper::wrap(groupKey_,
                               SessionId::invalid(),
                               seqBytes,
                               serial_,
                               tag_,
                               counterSuffix,
                               in,
                               out);
}

util::Result<size_t> SecureRoutingSecurity::unprotect(std::span<const uint8_t> in, std::span<uint8_t> out)
{
    auto unwrapRes = SecureWrapper::unwrapAndVerify(groupKey_, SessionId::invalid(), in, out);
    if (unwrapRes.isError()) return unwrapRes.error();

    return unwrapRes;
}

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
