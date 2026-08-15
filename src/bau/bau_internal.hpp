// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file bau_internal.hpp
 * @brief Types shared between the BAU translation units.
 *
 * Not a public header — it lives in `src/` deliberately. `bau.cpp` was a single
 * 2800-line file covering three unrelated concerns (lower-stack composition,
 * the group-object runtime, and device lifecycle); splitting it needs a place
 * for the handful of types that genuinely cross those boundaries. Anything that
 * does not cross a boundary should stay in the file that uses it.
 */

#pragma once

#include "knx/bau/bau.hpp"
#include "knx/config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace knx {
namespace bau {
namespace detail {

/// Upper bound on group objects one inbound telegram can be associated with.
inline constexpr size_t kMaxInboundAssociatedGroupObjects = 16u;

/// Inbound group events drained per loop() call, so a burst cannot starve the
/// rest of the owner context.
inline constexpr size_t kMaxInboundGroupEventsPerLoop = 32u;

/**
 * @brief One inbound group telegram, marshalled out of the lower-layer callback
 *        context onto the BAU owner context.
 *
 * Trivially copyable on purpose: it travels through a `platform::Queue`, which
 * moves raw bytes.
 */
struct PendingInboundGroupEvent {
    uint8_t kind{static_cast<uint8_t>(BusAccessUnit::MessageKind::Unknown)};
    uint16_t destinationRaw{0u};
    uint8_t associatedObjectCount{0u};
    std::array<uint16_t, kMaxInboundAssociatedGroupObjects> associatedObjectIndices{};
    uint16_t dataLength{0u};
    std::array<uint8_t, config::MAX_APDU_LENGTH> data{};
};

static_assert(std::is_trivially_copyable_v<PendingInboundGroupEvent>,
              "PendingInboundGroupEvent travels through a byte queue");

} // namespace detail
} // namespace bau
} // namespace knx
