// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "knx/util/result.hpp"

namespace knx {
namespace security {

class Sha256 {
public:
    using Digest = std::array<uint8_t, 32>;

    static util::Result<void> hash(std::span<const uint8_t> data, Digest& out);
};

} // namespace security
} // namespace knx
