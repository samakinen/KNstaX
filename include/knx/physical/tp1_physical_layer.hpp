// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_physical_layer.hpp
 * @brief Types shared by the physical layers the TP1 data link drives
 *
 * The contract itself is the `datalink::Tp1PhysicalLike` concept in
 * tp1_data_link_layer.hpp; there is no abstract base class.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "knx/physical/tp1_medium_backend.hpp"
#include "knx/util/inplace_function.hpp"
#include "knx/util/operation_progress.hpp"
#include "knx/util/result.hpp"
#include "knx/types.hpp"

namespace knx {
namespace physical {

/**
 * @brief Physical layer state
 */
enum class PhysicalLayerState {
    Idle,           // No transmission in progress
    Transmitting,   // Sending frame
    Receiving,      // Receiving frame
    Error,          // Error state
};

using TxOutcomeState = util::OperationProgressState;

/**
 * @brief RX callback signature
 */
using ReceiveCallback = util::InplaceFunction<void(void*), 32>;

} // namespace physical
} // namespace knx
