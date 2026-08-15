// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <array>
#include <cstdint>

#include "knx/util/result.hpp"

namespace knx {
namespace security {

class X25519 {
public:
    using Scalar = std::array<uint8_t, 32>; // little-endian
    using PublicKey = std::array<uint8_t, 32>; // little-endian u-coordinate
    using SharedSecret = std::array<uint8_t, 32>; // little-endian

    static util::Result<void> publicFromPrivate(const Scalar& priv, PublicKey& pub);

    static util::Result<void> sharedSecret(const Scalar& priv, const PublicKey& peerPub, SharedSecret& secret);
};

} // namespace security
} // namespace knx
