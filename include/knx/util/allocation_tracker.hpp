// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file allocation_tracker.hpp
 * @brief Lightweight allocation governance instrumentation for embedded testing.
 *
 * AllocationTracker provides a simple phase-aware counter that firmware code
 * and test harnesses can use to verify that no unexpected heap allocations
 * occur during steady-state runtime operation.
 *
 * Typical usage in test code:
 *
 *   // Instrument operator new/delete by calling AllocationTracker::record*():
 *   void* operator new(size_t n) {
 *       knx::util::AllocationTracker::recordAllocation(n);
 *       return std::malloc(n);
 *   }
 *   void operator delete(void* p) noexcept {
 *       knx::util::AllocationTracker::recordFree();
 *       std::free(p);
 *   }
 *
 *   // Mark end of boot phase:
 *   knx::util::AllocationTracker::setPhase(knx::util::AllocationPhase::Runtime);
 *
 *   // Verify no allocations occurred during a loop() call:
 *   {
 *       knx::util::ScopedAllocationGuard guard;
 *       device.loop();
 *       TEST_ASSERT_EQUAL(0, guard.allocations());
 *   }
 *
 * This header is intentionally header-only so it can be included in any
 * translation unit without additional link-time dependencies.
 *
 * Thread-safety: AllocationTracker uses std::atomic counters and is safe to
 * call from multiple threads.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace knx::util {

/// Lifecycle phase for allocation governance.
enum class AllocationPhase : uint8_t {
    Boot          = 0,  ///< Device startup; allocations expected.
    Commissioning = 1,  ///< ETS programming; some allocations may occur.
    Runtime       = 2,  ///< Steady-state; allocations are unexpected.
};

/**
 * @brief Thread-safe allocation event counter.
 *
 * All members are static — there is one tracker per process, which is
 * appropriate for embedded single-application firmware.
 */
class AllocationTracker {
public:
    AllocationTracker() = delete;

    // -------------------------------------------------------------------------
    // Instrumentation API — call from operator new / operator delete
    // -------------------------------------------------------------------------

    /// Record one heap allocation of @p bytes.
    static void recordAllocation(size_t bytes) noexcept {
        _totalAllocations.fetch_add(1, std::memory_order_relaxed);
        _totalBytesAllocated.fetch_add(static_cast<uint64_t>(bytes),
                                       std::memory_order_relaxed);
        if (_phase.load(std::memory_order_relaxed) == AllocationPhase::Runtime) {
            _runtimeAllocations.fetch_add(1, std::memory_order_relaxed);
            auto handler = _runtimeHandler.load(std::memory_order_acquire);
            if (handler) {
                handler();
            }
        }
    }

    /// Record one heap deallocation.
    static void recordFree() noexcept {
        _totalFrees.fetch_add(1, std::memory_order_relaxed);
    }

    // -------------------------------------------------------------------------
    // Phase control
    // -------------------------------------------------------------------------

    /// Set the current lifecycle phase.  Must be called from a single thread.
    static void setPhase(AllocationPhase phase) noexcept {
        _phase.store(phase, std::memory_order_release);
    }

    static AllocationPhase phase() noexcept {
        return _phase.load(std::memory_order_acquire);
    }

    // -------------------------------------------------------------------------
    // Runtime-allocation handler
    // -------------------------------------------------------------------------

    /// Signature: `void handler()`.
    using HandlerFn = void(*)();

    /// Install a handler called whenever a heap allocation occurs during
    /// AllocationPhase::Runtime.  Pass nullptr to remove.
    static void setRuntimeAllocationHandler(HandlerFn fn) noexcept {
        _runtimeHandler.store(fn, std::memory_order_release);
    }

    // -------------------------------------------------------------------------
    // Counter access
    // -------------------------------------------------------------------------

    static uint64_t totalAllocations()     noexcept { return _totalAllocations.load(std::memory_order_relaxed); }
    static uint64_t totalFrees()           noexcept { return _totalFrees.load(std::memory_order_relaxed); }
    static uint64_t totalBytesAllocated()  noexcept { return _totalBytesAllocated.load(std::memory_order_relaxed); }
    static uint64_t runtimeAllocations()   noexcept { return _runtimeAllocations.load(std::memory_order_relaxed); }

    // -------------------------------------------------------------------------
    // Reset
    // -------------------------------------------------------------------------

    /// Reset all counters (does not change the current phase or handler).
    static void resetCounters() noexcept {
        _totalAllocations.store(0, std::memory_order_relaxed);
        _totalFrees.store(0, std::memory_order_relaxed);
        _totalBytesAllocated.store(0, std::memory_order_relaxed);
        _runtimeAllocations.store(0, std::memory_order_relaxed);
    }

private:
    inline static std::atomic<uint64_t>       _totalAllocations{0};
    inline static std::atomic<uint64_t>       _totalFrees{0};
    inline static std::atomic<uint64_t>       _totalBytesAllocated{0};
    inline static std::atomic<uint64_t>       _runtimeAllocations{0};
    inline static std::atomic<AllocationPhase> _phase{AllocationPhase::Boot};
    inline static std::atomic<HandlerFn>       _runtimeHandler{nullptr};
};

// ---------------------------------------------------------------------------
// RAII guard — counts allocations within a lexical scope
// ---------------------------------------------------------------------------

/**
 * @brief RAII scope guard that counts heap allocations in a code block.
 *
 * On construction saves the current AllocationTracker counters; on destruction
 * (or when queried) reports the delta.
 *
 * Example:
 *   {
 *       ScopedAllocationGuard guard;
 *       device.loop();          // should not allocate in steady state
 *       TEST_ASSERT_EQUAL(0, guard.allocations());
 *   }
 */
class ScopedAllocationGuard {
public:
    ScopedAllocationGuard() noexcept
        : _baseline(AllocationTracker::totalAllocations()) {}

    /// Number of heap allocations recorded since this guard was constructed.
    uint64_t allocations() const noexcept {
        return AllocationTracker::totalAllocations() - _baseline;
    }

    /// Convenience: returns true when no allocations occurred.
    bool clean() const noexcept { return allocations() == 0; }

private:
    uint64_t _baseline;
};

} // namespace knx::util
