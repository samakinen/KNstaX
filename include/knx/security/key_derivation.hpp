// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "knx/util/result.hpp"

namespace knx {
namespace security {

/**
 * @brief KNX Device Key Derivation
 *
 * Implements PBKDF2-based key derivation for KNX devices.
 * Used to derive symmetric session keys from a master key and device identifier.
 *
 * Reference: KNX Security Profile specification (3.3 Key Agreement and Key Derivation)
 */
class KeyDerivation {
public:
    using Key = std::array<uint8_t, 16>;  // 16-byte key
    using DeviceId = std::array<uint8_t, 2>;  // KNX device address (2 bytes)
    using MasterKey = std::array<uint8_t, 32>;  // 256-bit master key for derivation

    /**
     * @brief Derive a session key from a master key and device identifier
     *
     * Uses PBKDF2-SHA256 with:
     * - Master key as password
     * - Device ID as salt (little-endian)
     * - 10,000 iterations (KNX Security Profile recommendation)
     * - Output length: 16 bytes (AES-128)
     *
     * @param master_key     The master secret key (32 bytes)
     * @param device_id      The KNX device address (2 bytes)
     * @param output_key     The derived session key (16 bytes)
    * @return               Result<void> indicating success or error
     */
    static util::Result<void> deriveSessionKey(const MasterKey& master_key,
                          const DeviceId& device_id,
                          Key& output_key);

    /**
     * @brief Derive keys for bilateral authentication
     *
     * Derives separate encryption keys for each direction (device→coupler, coupler→device).
     * Useful for scenarios requiring directional key separation.
     *
     * @param master_key         The master secret key (32 bytes)
     * @param device_id          The KNX device address (2 bytes)
     * @param coupler_id         The KNX coupler address (2 bytes)
     * @param device_tx_key      Derived key for device transmit
     * @param device_rx_key      Derived key for device receive
    * @return                   Result<void> indicating success or error
     */
    static util::Result<void> deriveBilateralKeys(const MasterKey& master_key,
                            const DeviceId& device_id,
                            const DeviceId& coupler_id,
                            Key& device_tx_key,
                            Key& device_rx_key);

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
