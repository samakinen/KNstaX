// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/netip/netip_security.hpp"

#if KNX_SECURE_ENABLED

#include "knx/netip/ip_secure/secure_wrapper.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace knx {
namespace netip {
namespace ip_secure {

// Concrete NetIpSecurity implementation for KNX/IP Secure routing (multicast).
//
// This wraps/unwraps complete KNXnet/IP datagrams (e.g. ROUTING_INDICATION 0x0530)
// into SecureWrapper (0x0950) using the group key.
class SecureRoutingSecurity final : public NetIpSecurity {
public:
    using Key = SecureWrapper::Key;

    SecureRoutingSecurity(const Key& groupKey,
                          const std::array<uint8_t, SecureWrapper::kSerialLen>& serial,
                          const std::array<uint8_t, SecureWrapper::kTagLen>& tag,
                          uint64_t initialSeq = 0);

    util::Result<size_t> protect(std::span<const uint8_t> in, std::span<uint8_t> out) override;
    util::Result<size_t> unprotect(std::span<const uint8_t> in, std::span<uint8_t> out) override;

private:
    static void encodeSeq48BE_(uint64_t seq48, std::array<uint8_t, SecureWrapper::kSeqLen>& out);

    Key groupKey_{};
    std::array<uint8_t, SecureWrapper::kSerialLen> serial_{};
    std::array<uint8_t, SecureWrapper::kTagLen> tag_{};
    std::atomic<uint64_t> seq48_{0};
};

} // namespace ip_secure
} // namespace netip
} // namespace knx

#endif // KNX_SECURE_ENABLED
