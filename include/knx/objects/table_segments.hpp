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
 * segments; the content is put into force when the LoadCompleted event
 * arrives. Each table exists once:
 *  - association table: the segment is the table's own storage
 *    (AssociationTableObject), read in place;
 *  - group address table: the segment is staging for the native entries the
 *    data link layers read, and the buffer a save encodes them into
 *    (AddressTableObject);
 *  - group object table: accepted, not applied (DeviceModel), as long as the
 *    product's group objects need.
 *
 * Segment layout: big-endian u16 entry count followed by the entries.
 */

#pragma once

#include "knx/config.hpp"

#include <cstdint>

namespace knx {
namespace objects {
namespace tableseg {

// Group address table (object index 1): count + KNX_ADDRESS_TABLE_ENTRIES ×
// 2-byte group address (capacity matches AddressTableDomain::kMaxEntries).
// A write that runs past the segment is refused by the memory service, with
// the negative response of the service used.
constexpr uint32_t kGroupAddressTableBase = 0x4000;
constexpr uint32_t kGroupAddressTableSize = 2u + static_cast<uint32_t>(config::ADDRESS_TABLE_ENTRIES) * 2u;

// Association table (object index 2): count + KNX_ASSOCIATION_TABLE_ENTRIES ×
// 4-byte TSAP/ASAP entry (capacity matches AssociationTableDomain::kMaxEntries).
constexpr uint32_t kAssociationTableBase = 0x4400;
constexpr uint32_t kAssociationTableSize = 2u + static_cast<uint32_t>(config::ASSOCIATION_TABLE_ENTRIES) * 4u;
static_assert(kGroupAddressTableBase + kGroupAddressTableSize <= kAssociationTableBase,
              "the group address segment must not run into the association segment");
static_assert(kAssociationTableBase + kAssociationTableSize <= 0x5000u,
              "the association segment must not run into the group object segment");

// Group object table (object index 3): count + one 2-byte descriptor per
// group object. Content is accepted but not applied — the group objects are
// firmware-defined — and kept only so that ETS reads back what it wrote. The
// segment is as long as the device's group objects need
// (groupObjectTableSize()); the address map keeps room for 256
// (GroupObjectTableDomain::kMaxObjects).
constexpr uint32_t kGroupObjectTableBase = 0x5000;
constexpr uint32_t kGroupObjectTableMaxObjects = 256u;
constexpr uint32_t kGroupObjectTableMaxSize = 2u + kGroupObjectTableMaxObjects * 2u;
constexpr uint32_t groupObjectTableSize(uint32_t objects) { return 2u + objects * 2u; }

// Application program code segment (object index 4): the product's parameter
// image (ParameterImageBinding), exactly as long as the image. A product
// without parameters has no segment. The image can take the address space up
// to the group address segment.
constexpr uint32_t kApplicationCodeBase = 0x0100;
constexpr uint32_t kApplicationCodeMaxSize = kGroupAddressTableBase - kApplicationCodeBase;

} // namespace tableseg
} // namespace objects
} // namespace knx
