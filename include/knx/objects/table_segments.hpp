// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file table_segments.hpp
 * @brief Memory-mapped table load segments for the ETS SystemB download
 *
 * The SystemB (mask 07B0) DefaultProcedure loads tables through memory
 * services: ETS allocates a segment via PID_LOAD_STATE_CONTROL additional
 * load controls, reads the segment address from PID_TABLE_REFERENCE (7) and
 * streams the table content with A_Memory_Write (verified via A_Memory_Read).
 * The addresses below define the device's virtual memory map for those
 * segments; the backing storage lives in the BAU and the content is applied
 * to the table domains when the LoadCompleted event arrives.
 *
 * Segment layout: big-endian u16 entry count followed by the entries.
 */

#pragma once

#include <cstdint>

namespace knx {
namespace objects {
namespace tableseg {

// Group address table (object index 1): count + 256 × 2-byte group address
// (capacity matches AddressTableDomain::kMaxEntries).
constexpr uint32_t kGroupAddressTableBase = 0x4000;
constexpr uint32_t kGroupAddressTableSize = 2u + 256u * 2u;

// Association table (object index 2): count + 512 × 4-byte TSAP/ASAP entry
// (capacity matches AssociationTableDomain::kMaxEntries).
constexpr uint32_t kAssociationTableBase = 0x4400;
constexpr uint32_t kAssociationTableSize = 2u + 512u * 4u;

// Group object table (object index 3): count + 256 × 2-byte descriptor
// (capacity matches GroupObjectTableDomain::kMaxObjects). Content is accepted
// but not applied — the group objects are firmware-defined.
constexpr uint32_t kGroupObjectTableBase = 0x5000;
constexpr uint32_t kGroupObjectTableSize = 2u + 256u * 2u;

// Application program code segment (object index 4): matches the knxprod
// Code RelativeSegment RS-0000 (256 bytes, no parameters defined).
constexpr uint32_t kApplicationCodeBase = 0x0100;
constexpr uint32_t kApplicationCodeSize = 0x0100;

} // namespace tableseg
} // namespace objects
} // namespace knx
