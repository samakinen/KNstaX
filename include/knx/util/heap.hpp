// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file heap.hpp
 * @brief Heap policy: lock the heap once start-up is over (KNX_HEAP_POLICY)
 *
 * A small target cannot afford a heap that grows while the device runs. With
 * `KNX_HEAP_POLICY=init_only` (the `tp1-small` and `tp1-standard` profiles) the
 * stack allocates while it starts, and the product then calls `lock()`; from
 * that point an allocation through `operator new` is a defect:
 *
 *  - in a debug build (no `NDEBUG`) it traps (`__builtin_trap()`), so the
 *    offending call is at the top of the backtrace;
 *  - in a release build it fails the way an allocation does when the heap is
 *    exhausted: `operator new` terminates (`std::abort()`; it throws
 *    `std::bad_alloc` in a build with exceptions), `new (std::nothrow)` returns
 *    null. The stack is built with `-fno-exceptions`, so for a product this is
 *    the same end as running out of memory.
 *
 * A product or a test can install a handler (`setViolationHandler`) instead; the
 * allocation then goes ahead and the handler is told its size and caller. The size
 * gate does that, to list every steady-state allocation of its traffic run.
 *
 * With `KNX_HEAP_POLICY=free` (the default) `operator new` is the toolchain's
 * and `lock()` only records the state.
 *
 * The high-water mark belongs to whoever owns the allocator: on bare metal the
 * product's `_sbrk`, on a host the test's counting allocator. The product
 * registers a function that reads it (`setHighWaterHook`), and `highWater()`
 * reports it, so diagnostics need not know which allocator is underneath.
 *
 * Concurrency: the lock flag and the counters are atomic; the handlers are
 * installed before the stack starts.
 */

#pragma once

#include <cstddef>

namespace knx::heap {

/// Called for an allocation made while the heap is locked. @p caller is the
/// return address of the `operator new` call.
using ViolationHandler = void (*)(std::size_t bytes, const void* caller);

/// Reads the allocator's high-water mark in bytes (0 when it has none).
using HighWaterHook = std::size_t (*)();

/// From now on an allocation through `operator new` is a violation (see above).
void lock() noexcept;

/// Lifts the lock: for tests, and for a product that restarts the stack.
void unlock() noexcept;

bool isLocked() noexcept;

/// Allocations through `operator new` the policy has seen (init_only only).
std::size_t allocationCount() noexcept;

/// Allocations made while locked (init_only only).
std::size_t violationCount() noexcept;

void setViolationHandler(ViolationHandler handler) noexcept;
void setHighWaterHook(HighWaterHook hook) noexcept;

/// The allocator's high-water mark in bytes, from the product's hook; 0 without one.
std::size_t highWater() noexcept;

/// The policy decision for one allocation: true when it may go ahead. What the
/// replaced `operator new` calls; public so a test can drive it.
bool admit(std::size_t bytes, const void* caller) noexcept;

}  // namespace knx::heap
