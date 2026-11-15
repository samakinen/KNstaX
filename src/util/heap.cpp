// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file heap.cpp
 * @brief Heap policy (see knx/util/heap.hpp)
 */

#include "knx/util/heap.hpp"

#include "knx/config.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace knx::heap {

namespace {

std::atomic<bool> g_locked{false};
std::atomic<std::size_t> g_allocations{0};
std::atomic<std::size_t> g_violations{0};
std::atomic<ViolationHandler> g_handler{nullptr};
std::atomic<HighWaterHook> g_highWaterHook{nullptr};

}  // namespace

void lock() noexcept { g_locked.store(true, std::memory_order_release); }
void unlock() noexcept { g_locked.store(false, std::memory_order_release); }
bool isLocked() noexcept { return g_locked.load(std::memory_order_acquire); }
std::size_t allocationCount() noexcept { return g_allocations.load(std::memory_order_relaxed); }
std::size_t violationCount() noexcept { return g_violations.load(std::memory_order_relaxed); }
void setViolationHandler(ViolationHandler handler) noexcept { g_handler.store(handler); }
void setHighWaterHook(HighWaterHook hook) noexcept { g_highWaterHook.store(hook); }

std::size_t highWater() noexcept
{
    const HighWaterHook hook = g_highWaterHook.load();
    return hook != nullptr ? hook() : 0u;
}

bool admit(std::size_t bytes, const void* caller) noexcept
{
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (!isLocked()) {
        return true;
    }
    g_violations.fetch_add(1, std::memory_order_relaxed);
    if (const ViolationHandler handler = g_handler.load()) {
        handler(bytes, caller);
        return true;
    }
#if !defined(NDEBUG)
    __builtin_trap();
#else
    return false;
#endif
}

}  // namespace knx::heap

#if defined(CONFIG_KNX_HEAP_POLICY_INIT_ONLY)

// The replaced allocation functions. Only the allocating half is replaced: the
// toolchain's operator delete frees with free(), which is what malloc() pairs with.
// noinline: the caller is the return address of these functions.

namespace {

[[noreturn]] void allocationFailed()
{
#if defined(__cpp_exceptions)
    throw std::bad_alloc();
#else
    std::abort();
#endif
}

}  // namespace

__attribute__((noinline)) void* operator new(std::size_t size)
{
    if (knx::heap::admit(size, __builtin_return_address(0))) {
        if (void* p = std::malloc(size != 0u ? size : 1u)) {
            return p;
        }
    }
    allocationFailed();
}

__attribute__((noinline)) void* operator new[](std::size_t size)
{
    if (knx::heap::admit(size, __builtin_return_address(0))) {
        if (void* p = std::malloc(size != 0u ? size : 1u)) {
            return p;
        }
    }
    allocationFailed();
}

__attribute__((noinline)) void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return knx::heap::admit(size, __builtin_return_address(0)) ? std::malloc(size != 0u ? size : 1u) : nullptr;
}

__attribute__((noinline)) void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return knx::heap::admit(size, __builtin_return_address(0)) ? std::malloc(size != 0u ? size : 1u) : nullptr;
}

#endif  // CONFIG_KNX_HEAP_POLICY_INIT_ONLY
