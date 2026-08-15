// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

/**
 * @file security_individual_address_table.hpp
 * @brief Typed view over PID_SECURITY_INDIVIDUAL_ADDRESS_TABLE (03/05/01 §6.3.8).
 *
 * The property arrives from ETS as an opaque array blob. Two very different
 * parts of the stack have to read it — the BAU resolves an IA_Index into an
 * address when it rebuilds the point-to-point key map, and the Security
 * Interface Object stores each partner's Last Valid SeqNr in it — so the
 * element layout lives here once instead of being open-coded twice.
 *
 * Figure 75: each element is
 *
 *     Individual Address (2 octets) | Last Valid SeqNr (6 octets)
 *
 * sorted by ascending Individual Address (the Management Client's job, §6.3.8.5;
 * nothing here depends on the order).
 */

#include "knx/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace knx {
namespace objects {
namespace security_ia_table {

/// PDT_GENERIC_08: IA(2) + Last Valid SeqNr(6).
inline constexpr size_t kEntryBytes = 8;
inline constexpr size_t kAddressOffset = 0;
inline constexpr size_t kSequenceOffset = 2;
inline constexpr size_t kSequenceBytes = 6;

/// Number of whole elements in @p blob. A trailing partial element is ignored:
/// a truncated download must not be read as an entry with a garbage address.
[[nodiscard]] inline size_t entryCount(std::span<const uint8_t> blob) noexcept
{
    return blob.size() / kEntryBytes;
}

[[nodiscard]] inline IndividualAddress addressAt(std::span<const uint8_t> blob, size_t index) noexcept
{
    const size_t offset = index * kEntryBytes + kAddressOffset;
    return IndividualAddress(static_cast<uint16_t>((static_cast<uint16_t>(blob[offset]) << 8) |
                                                   blob[offset + 1]));
}

/// Last Valid SeqNr of element @p index, as a 48-bit big-endian value.
[[nodiscard]] inline uint64_t sequenceAt(std::span<const uint8_t> blob, size_t index) noexcept
{
    const size_t offset = index * kEntryBytes + kSequenceOffset;
    uint64_t value = 0;
    for (size_t i = 0; i < kSequenceBytes; ++i) {
        value = (value << 8) | blob[offset + i];
    }
    return value;
}

/// Element index of @p address, or nullopt when this device has no secure link
/// with it (§6.3.8.4: "the MaS shall assume that it does not have a KNX Data
/// Security link with the KNX device on that IA").
[[nodiscard]] inline std::optional<size_t> indexOf(std::span<const uint8_t> blob,
                                                   const IndividualAddress& address) noexcept
{
    const size_t count = entryCount(blob);
    for (size_t index = 0; index < count; ++index) {
        if (addressAt(blob, index).raw == address.raw) {
            return index;
        }
    }
    return std::nullopt;
}

/// Overwrite the Last Valid SeqNr of @p address in place.
/// @return false when @p address has no entry, which leaves @p blob untouched.
inline bool setSequenceFor(std::span<uint8_t> blob,
                           const IndividualAddress& address,
                           uint64_t sequence) noexcept
{
    const auto index = indexOf(blob, address);
    if (!index.has_value()) {
        return false;
    }
    const size_t offset = *index * kEntryBytes + kSequenceOffset;
    for (size_t i = 0; i < kSequenceBytes; ++i) {
        const size_t shift = 8u * (kSequenceBytes - 1u - i);
        blob[offset + i] = static_cast<uint8_t>((sequence >> shift) & 0xFFu);
    }
    return true;
}

} // namespace security_ia_table
} // namespace objects
} // namespace knx
