// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file shared_group_address_list.hpp
 * @brief A group address list that an interrupt may read while its owner changes it
 *
 * The device's group address list has one copy, the address table's storage.
 * A medium that decides the L2 acknowledge in an interrupt (the TP-UART
 * backend) reads that storage directly instead of keeping a copy of its own.
 * The owner brackets every change of the entries with beginChange() and
 * endChange(); the reader reads the count and the entries between two loads
 * of the generation, and discards what it read if the generation was odd or
 * moved. A reader that discards does not acknowledge the frame; the sender
 * repeats it.
 *
 * The entries themselves are plain GroupAddress values in the owner's fixed
 * storage. They are only ever used when the generation proves they did not
 * change while being read.
 *
 * @thread_safety One writer (the owner, main context); any number of readers
 *                in other contexts, interrupts included. The entries' storage
 *                must not move while a view of it is published.
 */

#pragma once

#include "knx/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace knx {
namespace util {

/// What a reader needs; the pointers are stable for the owner's lifetime.
struct SharedGroupAddressView {
    const GroupAddress* entries{nullptr};
    uint16_t capacity{0};
    const std::atomic<uint16_t>* count{nullptr};
    const std::atomic<uint32_t>* generation{nullptr};

    bool isValid() const { return entries != nullptr && count != nullptr && generation != nullptr; }
};

/// The owner's half: the published count and the generation.
class SharedGroupAddressState {
public:
    SharedGroupAddressState() = default;
    // The owner may be moved before any view is published; the values move with it.
    SharedGroupAddressState(const SharedGroupAddressState& other) { copyFrom(other); }
    SharedGroupAddressState& operator=(const SharedGroupAddressState& other)
    {
        copyFrom(other);
        return *this;
    }

    /// Before the entries change: readers discard from here on.
    void beginChange()
    {
        _generation.store(_generation.load(std::memory_order_relaxed) + 1u, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    /// After the entries changed: publish how many there are now.
    void endChange(size_t count)
    {
        _count.store(static_cast<uint16_t>(count), std::memory_order_relaxed);
        _generation.store(_generation.load(std::memory_order_relaxed) + 1u, std::memory_order_release);
    }

    SharedGroupAddressView view(const GroupAddress* entries, uint16_t capacity) const
    {
        return SharedGroupAddressView{entries, capacity, &_count, &_generation};
    }

private:
    void copyFrom(const SharedGroupAddressState& other)
    {
        _count.store(other._count.load(std::memory_order_relaxed), std::memory_order_relaxed);
        _generation.store(other._generation.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    std::atomic<uint16_t> _count{0};
    std::atomic<uint32_t> _generation{0};  ///< odd while the entries change
};

} // namespace util
} // namespace knx
