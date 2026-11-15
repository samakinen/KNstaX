// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file fixed_vector.hpp
 * @brief Fixed-capacity vector for deterministic storage
 *
 * Elements live in an inline buffer of Capacity slots. Only the first size()
 * slots hold constructed objects: removing an element (pop_back, erase,
 * clear, a shrinking resize or assign) runs its destructor, so a vector of
 * owning types such as std::unique_ptr releases what it removes at once
 * rather than when the slot is next overwritten. T need not be default
 * constructible.
 *
 * For trivially copyable T the vector is itself trivially copyable and
 * trivially destructible, exactly like the std::array it used to wrap, so
 * structures that embed it can still be passed through byte-copying queues.
 *
 * Moving a vector moves its elements and leaves the source with the same
 * size holding moved-from elements (as moving a std::array does).
 *
 * @thread_safety NOT thread-safe — callers synchronise access.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>

namespace knx {
namespace util {

template <typename T, size_t Capacity>
class FixedVector {
    static constexpr bool kTrivial = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

public:
    constexpr FixedVector() noexcept {}

    FixedVector(std::initializer_list<T> values) {
        (void)assign(values);
    }

    // Trivial element types keep the defaulted (trivial) special members.
    FixedVector(const FixedVector&) requires kTrivial = default;
    FixedVector(FixedVector&&) requires kTrivial = default;
    FixedVector& operator=(const FixedVector&) requires kTrivial = default;
    FixedVector& operator=(FixedVector&&) requires kTrivial = default;
    ~FixedVector() requires kTrivial = default;

    FixedVector(const FixedVector& other) requires(!kTrivial && std::is_copy_constructible_v<T>)
    {
        std::uninitialized_copy_n(other.begin(), other._size, begin());
        _size = other._size;
    }

    FixedVector(FixedVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        requires(!kTrivial)
    {
        std::uninitialized_move_n(other.begin(), other._size, begin());
        _size = other._size;
    }

    FixedVector& operator=(const FixedVector& other) requires(!kTrivial && std::is_copy_constructible_v<T>)
    {
        if (this != &other) {
            clear();
            std::uninitialized_copy_n(other.begin(), other._size, begin());
            _size = other._size;
        }
        return *this;
    }

    FixedVector& operator=(FixedVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        requires(!kTrivial)
    {
        if (this != &other) {
            clear();
            std::uninitialized_move_n(other.begin(), other._size, begin());
            _size = other._size;
        }
        return *this;
    }

    ~FixedVector() requires(!kTrivial)
    {
        clear();
    }

    FixedVector& operator=(std::initializer_list<T> values) {
        (void)assign(values);
        return *this;
    }

    constexpr size_t size() const noexcept { return _size; }
    static constexpr size_t capacity() noexcept { return Capacity; }
    constexpr bool empty() const noexcept { return _size == 0; }
    constexpr bool full() const noexcept { return _size >= Capacity; }

    constexpr T* data() noexcept { return _items; }
    constexpr const T* data() const noexcept { return _items; }

    constexpr T& operator[](size_t index) noexcept { return _items[index]; }
    constexpr const T& operator[](size_t index) const noexcept { return _items[index]; }

    T& front() noexcept { assert(_size > 0); return _items[0]; }
    const T& front() const noexcept { assert(_size > 0); return _items[0]; }
    T& back() noexcept { assert(_size > 0); return _items[_size - 1]; }
    const T& back() const noexcept { assert(_size > 0); return _items[_size - 1]; }

    constexpr T* begin() noexcept { return _items; }
    constexpr const T* begin() const noexcept { return _items; }
    constexpr T* end() noexcept { return _items + _size; }
    constexpr const T* end() const noexcept { return _items + _size; }

    constexpr std::span<T> span() noexcept { return std::span<T>(_items, _size); }
    constexpr std::span<const T> span() const noexcept { return std::span<const T>(_items, _size); }

    void clear() noexcept {
        std::destroy_n(begin(), _size);
        _size = 0;
    }

    bool assign(std::span<const T> values) {
        if (values.size() > Capacity) {
            return false;
        }
        clear();
        std::uninitialized_copy_n(values.begin(), values.size(), begin());
        _size = values.size();
        return true;
    }

    bool assign(std::initializer_list<T> values) {
        return assign(std::span<const T>(values.begin(), values.size()));
    }

    bool push_back(const T& value) {
        return emplace_back(value);
    }

    bool push_back(T&& value) {
        return emplace_back(std::move(value));
    }

    template <typename... Args>
    bool emplace_back(Args&&... args) {
        if (_size >= Capacity) {
            return false;
        }
        std::construct_at(_items + _size, std::forward<Args>(args)...);
        ++_size;
        return true;
    }

    void pop_back() noexcept {
        if (_size > 0) {
            --_size;
            std::destroy_at(_items + _size);
        }
    }

    void erase(size_t index) {
        if (index >= _size) {
            return;
        }
        for (size_t i = index + 1; i < _size; ++i) {
            _items[i - 1] = std::move(_items[i]);
        }
        pop_back();
    }

    void resize(size_t newSize) {
        resizeWith(newSize, [](T* slot) { std::construct_at(slot); });
    }

    void resize(size_t newSize, const T& value) {
        resizeWith(newSize, [&value](T* slot) { std::construct_at(slot, value); });
    }

private:
    template <typename Construct>
    void resizeWith(size_t newSize, Construct construct) {
        if (newSize > Capacity) {
            newSize = Capacity;
        }
        while (_size > newSize) {
            pop_back();
        }
        while (_size < newSize) {
            construct(_items + _size);
            ++_size;
        }
    }

    // An anonymous union leaves the slots unconstructed until an element is
    // placed in them; for trivial T it keeps the class trivially copyable.
    // A zero capacity (a configured size of 0) still needs one slot, as a
    // zero-length array is not standard C++; no element is ever constructed
    // in it, because every insertion checks _size against Capacity first.
    union {
        T _items[Capacity == 0 ? 1 : Capacity];
    };
    size_t _size{0};
};

} // namespace util
} // namespace knx
