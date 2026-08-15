// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/util/result.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace knx {
namespace netip {

// Minimal, opt-in KNXnet/IP transport security wrapper.
//
// This is intentionally spec-agnostic plumbing so we can incrementally implement
// KNXnet/IP Secure without changing default behavior.
class NetIpSecurity {
public:
    virtual ~NetIpSecurity() = default;

    // Protect an outbound KNXnet/IP datagram.
    // Input is the complete KNXnet/IP datagram (header + payload).
    virtual util::Result<size_t> protect(std::span<const uint8_t> in, std::span<uint8_t> out) = 0;

    // Unprotect an inbound KNXnet/IP datagram.
    // Input is the received UDP payload; output is the plaintext KNXnet/IP datagram.
    virtual util::Result<size_t> unprotect(std::span<const uint8_t> in, std::span<uint8_t> out) = 0;
};

} // namespace netip
} // namespace knx
