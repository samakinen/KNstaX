// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file fixed_vector.hpp
 * @brief Fixed-capacity vector for deterministic storage
 */

#pragma once

#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <utility>

namespace knx {
namespace util {

template <typename T, size_t Capacity>
class FixedVector {
public:
    constexpr FixedVector() noexcept = default;
    FixedVector(std::initializer_list<T> values) {
        (void)assign(values);
    }

    FixedVector& operator=(std::initializer_list<T> values) {
        (void)assign(values);
        return *this;
    }

    constexpr size_t size() const noexcept { return _size; }
    static constexpr size_t capacity() noexcept { return Capacity; }
    constexpr bool empty() const noexcept { return _size == 0; }
    constexpr bool full() const noexcept { return _size >= Capacity; }

    constexpr T* data() noexcept { return _data.data(); }
    constexpr const T* data() const noexcept { return _data.data(); }

    constexpr T& operator[](size_t index) noexcept { return _data[index]; }
    constexpr const T& operator[](size_t index) const noexcept { return _data[index]; }

    constexpr T& back() noexcept { return _data[_size - 1]; }
    constexpr const T& back() const noexcept { return _data[_size - 1]; }

    constexpr T* begin() noexcept { return _data.data(); }
    constexpr const T* begin() const noexcept { return _data.data(); }
    constexpr T* end() noexcept { return _data.data() + _size; }
    constexpr const T* end() const noexcept { return _data.data() + _size; }

    constexpr std::span<T> span() noexcept { return std::span<T>(_data.data(), _size); }
    constexpr std::span<const T> span() const noexcept { return std::span<const T>(_data.data(), _size); }

    constexpr void clear() noexcept { _size = 0; }

    bool assign(std::span<const T> values) {
        if (values.size() > Capacity) {
            return false;
        }

        _size = 0;
        for (const auto& value : values) {
            _data[_size++] = value;
        }
        return true;
    }

    bool assign(std::initializer_list<T> values) {
        return assign(std::span<const T>(values.begin(), values.size()));
    }

    bool push_back(const T& value) {
        if (_size >= Capacity) {
            return false;
        }
        _data[_size++] = value;
        return true;
    }

    bool push_back(T&& value) {
        if (_size >= Capacity) {
            return false;
        }
        _data[_size++] = std::move(value);
        return true;
    }

    template <typename... Args>
    bool emplace_back(Args&&... args) {
        if (_size >= Capacity) {
            return false;
        }
        _data[_size++] = T(std::forward<Args>(args)...);
        return true;
    }

    void pop_back() {
        if (_size > 0) {
            --_size;
        }
    }

    void erase(size_t index) {
        if (index >= _size) {
            return;
        }
        for (size_t i = index + 1; i < _size; ++i) {
            _data[i - 1] = std::move(_data[i]);
        }
        --_size;
    }

    void resize(size_t newSize) {
        if (newSize > Capacity) {
            newSize = Capacity;
        }
        if (newSize > _size) {
            for (size_t i = _size; i < newSize; ++i) {
                _data[i] = T{};
            }
        }
        _size = newSize;
    }

    void resize(size_t newSize, const T& value) {
        if (newSize > Capacity) {
            newSize = Capacity;
        }
        if (newSize > _size) {
            for (size_t i = _size; i < newSize; ++i) {
                _data[i] = value;
            }
        }
        _size = newSize;
    }

private:
    std::array<T, Capacity> _data{};
    size_t _size{0};
};

} // namespace util
} // namespace knx
