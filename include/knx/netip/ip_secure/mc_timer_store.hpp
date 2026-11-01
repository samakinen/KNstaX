// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

#pragma once

#include "knx/util/result.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace knx {
namespace netip {
namespace ip_secure {

/**
 * @brief Non-volatile home of the Secure Routing multicast timer.
 *
 * mc_timer "shall under no circumstances be decremented", power loss included
 * (03_08_09 §2.2.4.2). A device without an RTC or a power-fail interrupt meets
 * that by writing the timer at least every D of mc_timer time and, after a
 * restart, resuming from the stored value plus D. The stack decides when; this
 * interface is only where — the product implements it on whatever storage
 * already holds its commissioned state.
 *
 * Each record carries a check value of the backbone key it was written under.
 * A stored timer is only resumed for the same key: a new key restarts the
 * timer at 0 (§2.2.2.2.2), while the same key re-applied at boot must not.
 *
 * @thread_safety Called from whichever task sends or receives on the routing
 *                socket, serialised by the security context.
 */
class McTimerStore {
public:
    struct Record {
        uint64_t timer{0};
        std::array<uint8_t, 4> keyCheck{};
    };

    /// Wire format of a record: timer (6 octets, big-endian) | key check (4).
    static constexpr size_t kEncodedSize = 10;

    virtual ~McTimerStore() = default;

    /// The last record saved, or nullopt when there is none (or it is unreadable).
    virtual std::optional<Record> load() = 0;

    /// Replace the stored record. Must be durable when it returns Ok.
    virtual util::Result<void> save(const Record& record) = 0;

    static std::array<uint8_t, kEncodedSize> encode(const Record& record) noexcept
    {
        std::array<uint8_t, kEncodedSize> out{};
        for (size_t i = 0; i < 6; ++i) {
            out[i] = static_cast<uint8_t>((record.timer >> (8u * (5u - i))) & 0xFFu);
        }
        for (size_t i = 0; i < record.keyCheck.size(); ++i) {
            out[6 + i] = record.keyCheck[i];
        }
        return out;
    }

    static std::optional<Record> decode(std::span<const uint8_t> in) noexcept
    {
        if (in.size() != kEncodedSize) {
            return std::nullopt;
        }
        Record record{};
        for (size_t i = 0; i < 6; ++i) {
            record.timer = (record.timer << 8) | in[i];
        }
        for (size_t i = 0; i < record.keyCheck.size(); ++i) {
            record.keyCheck[i] = in[6 + i];
        }
        return record;
    }
};

} // namespace ip_secure
} // namespace netip
} // namespace knx
