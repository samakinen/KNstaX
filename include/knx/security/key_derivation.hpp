// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <cstdint>
#include <span>

#include "knx/util/result.hpp"

namespace knx {
namespace security {

/**
 * @brief PBKDF2-SHA256 key derivation
 *
 * Used by KNXnet/IP Secure to derive keys from passwords.
 */
class KeyDerivation {
public:
    /**
     * @brief Perform PBKDF2-SHA256 key stretching
     *
     * Generic PBKDF2 implementation for custom key derivation scenarios.
     *
     * @param password       Input password/master key
     * @param salt          Derivation salt
     * @param iterations    Number of HMAC iterations (>= 10000 recommended)
     * @param output        Output buffer with desired key length (e.g., 16 bytes for AES-128)
    * @return              Result<void> indicating success or error
     */
    static util::Result<void> pbkdf2(std::span<const uint8_t> password,
                  std::span<const uint8_t> salt,
                  uint32_t iterations,
                  std::span<uint8_t> output);
};

}  // namespace security
}  // namespace knx
