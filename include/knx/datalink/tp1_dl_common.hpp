// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file tp1_dl_common.hpp
 * @brief Common TP1 Data Link layer constants shared across implementations
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace knx {
namespace datalink {

// Frame field positions
static constexpr size_t CTRL_FIELD_POS = 0;
static constexpr size_t SRC_ADDR_HI_POS = 1;
static constexpr size_t SRC_ADDR_LO_POS = 2;
static constexpr size_t DEST_ADDR_HI_POS = 3;
static constexpr size_t DEST_ADDR_LO_POS = 4;
static constexpr size_t LENGTH_POS = 5;
static constexpr size_t TPCI_POS = 6;

// Control field bits
static constexpr uint8_t CTRL_FRAME_TYPE = 0x80;     // 0=extended, 1=standard
static constexpr uint8_t CTRL_REPEAT = 0x20;         // Repeat flag
static constexpr uint8_t CTRL_PRIORITY_MASK = 0x0C;  // Priority bits
static constexpr uint8_t CTRL_ACK_REQ = 0x02;        // ACK requested
static constexpr uint8_t CTRL_CONFIRM = 0x01;        // Confirmation

// Address type bit in the routing/length octet
static constexpr uint8_t DEST_ADDR_TYPE = 0x80;   // 0=individual, 1=group

} // namespace datalink
} // namespace knx
