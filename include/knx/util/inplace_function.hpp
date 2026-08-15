// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file inplace_function.hpp
 * @brief Fixed-size callable wrapper for deterministic, allocation-free function storage.
 *
 * `InplaceFunction<Signature, Capacity, Align>` is a drop-in replacement for
 * `std::function` that stores its target object entirely within a fixed-size
 * internal buffer.  It never allocates on the heap.
 *
 * Constraints:
 *  - The stored callable must fit within Capacity bytes when aligned to Align.
 *  - Attempting to assign a callable that is too large is a compile-time error.
 *  - Move-only callables are supported (move-constructible, not required to be
 *    copy-constructible); the wrapper itself is move-constructible but also
 *    copy-constructible when the stored callable is.
 *
 * Differences from std::function:
 *  - No heap allocation, ever.
 *  - operator bool() returns false only for a default-constructed (or moved-from)
 *    instance — there is no null state after assignment.
 *  - Assigning a callable that exceeds the capacity is a hard compile-time error.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>   // std::invoke
#include <new>
#include <type_traits>
#include <utility>

namespace knx {
namespace util {

namespace detail {

/// VTable used for type-erased dispatch.
template <typename ReturnT, typename... ArgsT>
struct InplaceFunctionVTable {
    using invoke_fn    = ReturnT (*)(void* storage, ArgsT&&... args);
    using copy_fn      = void    (*)(void* dst, const void* src);
    using move_fn      = void    (*)(void* dst, void* src) noexcept;
    using destroy_fn   = void    (*)(void* storage) noexcept;

    invoke_fn  invoke;
    copy_fn    copy;    ///< nullptr when callable is not copy-constructible
    move_fn    move;
    destroy_fn destroy;
};

template <typename CallableT, typename ReturnT, typename... ArgsT>
ReturnT inplaceFunctionInvoke(void* storage, ArgsT&&... args)
{
    return std::invoke(*static_cast<CallableT*>(storage), std::forward<ArgsT>(args)...);
}

template <typename CallableT, typename ReturnT, typename... ArgsT>
constexpr InplaceFunctionVTable<ReturnT, ArgsT...> makeVTable() noexcept
{
    return InplaceFunctionVTable<ReturnT, ArgsT...>{
        .invoke  = &inplaceFunctionInvoke<CallableT, ReturnT, ArgsT...>,
        .copy    = []([[maybe_unused]] void* dst, [[maybe_unused]] const void* src) {
            if constexpr (std::is_copy_constructible_v<CallableT>) {
                ::new (dst) CallableT(*static_cast<const CallableT*>(src));
            }
        },
        .move    = [](void* dst, void* src) noexcept {
            ::new (dst) CallableT(std::move(*static_cast<CallableT*>(src)));
            static_cast<CallableT*>(src)->~CallableT();
        },
        .destroy = [](void* storage) noexcept {
            static_cast<CallableT*>(storage)->~CallableT();
        },
    };
}

} // namespace detail

// ---------------------------------------------------------------------------
// Primary template — specialised below for function signatures
// ---------------------------------------------------------------------------
template <typename Signature, size_t Capacity = 32, size_t Align = alignof(std::max_align_t)>
class InplaceFunction;

template <typename ReturnT, typename... ArgsT, size_t Capacity, size_t Align>
class InplaceFunction<ReturnT(ArgsT...), Capacity, Align> {
public:
    using result_type = ReturnT;

    // ------------------------------------------------------------------
    // Construction / destruction
    // ------------------------------------------------------------------

    InplaceFunction() noexcept : _vtable(nullptr) {}

    InplaceFunction(std::nullptr_t) noexcept : _vtable(nullptr) {}

    template <typename CallableT,
              typename Decayed = std::decay_t<CallableT>,
              // Prevent hijacking copy/move constructors
              typename = std::enable_if_t<!std::is_same_v<Decayed, InplaceFunction>>>
    explicit(false) InplaceFunction(CallableT&& callable)  // NOLINT(*-explicit-*)
    {
        static_assert(sizeof(Decayed) <= Capacity,
            "InplaceFunction: callable is too large for the storage buffer. "
            "Increase the Capacity template parameter.");
        static_assert(alignof(Decayed) <= Align,
            "InplaceFunction: callable alignment exceeds the storage alignment. "
            "Increase the Align template parameter.");

        static constexpr auto vtable = detail::makeVTable<Decayed, ReturnT, ArgsT...>();
        _vtable = &vtable;
        ::new (storage()) Decayed(std::forward<CallableT>(callable));
    }

    InplaceFunction(const InplaceFunction& other)
    {
        if (other._vtable) {
            other._vtable->copy(storage(), other.storage());
        }
        _vtable = other._vtable;
    }

    InplaceFunction(InplaceFunction&& other) noexcept
    {
        if (other._vtable) {
            other._vtable->move(storage(), other.storage());
        }
        _vtable = other._vtable;
        other._vtable = nullptr;
    }

    ~InplaceFunction()
    {
        reset();
    }

    // ------------------------------------------------------------------
    // Assignment
    // ------------------------------------------------------------------

    InplaceFunction& operator=(const InplaceFunction& other)
    {
        if (this != &other) {
            reset();
            if (other._vtable) {
                other._vtable->copy(storage(), other.storage());
            }
            _vtable = other._vtable;
        }
        return *this;
    }

    InplaceFunction& operator=(InplaceFunction&& other) noexcept
    {
        if (this != &other) {
            reset();
            if (other._vtable) {
                other._vtable->move(storage(), other.storage());
            }
            _vtable = other._vtable;
            other._vtable = nullptr;
        }
        return *this;
    }

    InplaceFunction& operator=(std::nullptr_t) noexcept
    {
        reset();
        return *this;
    }

    template <typename CallableT,
              typename Decayed = std::decay_t<CallableT>,
              typename = std::enable_if_t<!std::is_same_v<Decayed, InplaceFunction>>>
    InplaceFunction& operator=(CallableT&& callable)
    {
        reset();
        InplaceFunction tmp(std::forward<CallableT>(callable));
        *this = std::move(tmp);
        return *this;
    }

    // ------------------------------------------------------------------
    // Observers
    // ------------------------------------------------------------------

    explicit operator bool() const noexcept { return _vtable != nullptr; }

    // ------------------------------------------------------------------
    // Invocation
    // ------------------------------------------------------------------

    ReturnT operator()(ArgsT... args) const
    {
        return _vtable->invoke(storage(), std::forward<ArgsT>(args)...);
    }

    ReturnT operator()(ArgsT... args)
    {
        return _vtable->invoke(storage(), std::forward<ArgsT>(args)...);
    }

    // ------------------------------------------------------------------
    // Modifiers
    // ------------------------------------------------------------------

    void reset() noexcept
    {
        if (_vtable) {
            _vtable->destroy(storage());
            _vtable = nullptr;
        }
    }

private:
    alignas(Align) mutable std::byte _storage[Capacity]{};
    const detail::InplaceFunctionVTable<ReturnT, ArgsT...>* _vtable;

    void* storage()       noexcept { return static_cast<void*>(_storage); }
    void* storage() const noexcept { return static_cast<void*>(_storage); }
};

} // namespace util
} // namespace knx
