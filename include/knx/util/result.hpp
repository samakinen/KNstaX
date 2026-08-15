// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 Sami Mäkinen

/**
 * @file result.hpp
 * @brief Result<T> — the canonical KNstaX error-handling type.
 *
 * Provides a type-safe way to return either a value or an error code,
 * avoiding exceptions and bool-based error handling inconsistencies.
 *
 * Result<T> is backed by std::expected<T, ErrorCode> internally and is the
 * stable named type used throughout the stack.  Use Result<T> consistently
 * in new code — do not introduce raw std::expected at call sites.
 */

#pragma once

#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>

namespace knx {
namespace util {

/**
 * @brief Error codes for KNX stack operations
 */
enum class ErrorCode : uint8_t {
    Success = 0,
    InvalidParameter,
    InvalidAddress,
    InvalidFrameSize,
    BufferTooSmall,
    OutOfRange,
    AccessDenied,
    ChecksumError,
    Timeout,
    Busy,
    QueueFull,
    NotInitialized,
    AlreadyInitialized,
    TransmissionFailed,
    DecodeFailed,
    EncodeFailed,
    ResourceUnavailable,
    OperationFailed,
    OperationNotReady,
    OperationNotSupported,
    /// The frame was fully handled by the layer that returned this and must not
    /// travel further. Not a failure: KNX Data Secure answers an
    /// S-A_Sync_Request itself, and the request never reaches the application.
    FrameConsumed,
    UnknownError = 255
};

/**
 * @brief Convert error code to human-readable string
 */
[[nodiscard]] inline constexpr const char* errorCodeToString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::Success: return "Success";
        case ErrorCode::InvalidParameter: return "Invalid parameter";
        case ErrorCode::InvalidAddress: return "Invalid address";
        case ErrorCode::InvalidFrameSize: return "Invalid frame size";
        case ErrorCode::BufferTooSmall: return "Buffer too small";
        case ErrorCode::OutOfRange: return "Out of range";
        case ErrorCode::AccessDenied: return "Access denied";
        case ErrorCode::ChecksumError: return "Checksum error";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::Busy: return "Busy";
        case ErrorCode::QueueFull: return "Queue full";
        case ErrorCode::NotInitialized: return "Not initialized";
        case ErrorCode::AlreadyInitialized: return "Already initialized";
        case ErrorCode::TransmissionFailed: return "Transmission failed";
        case ErrorCode::DecodeFailed: return "Decode failed";
        case ErrorCode::EncodeFailed: return "Encode failed";
        case ErrorCode::ResourceUnavailable: return "Resource unavailable";
        case ErrorCode::OperationFailed: return "Operation failed";
        case ErrorCode::OperationNotReady: return "Operation not ready";
        case ErrorCode::OperationNotSupported: return "Operation not supported";
        case ErrorCode::FrameConsumed: return "Frame consumed";
        case ErrorCode::UnknownError: return "Unknown error";
        default: return "Undefined error";
    }
}

/**
 * @brief Result type that holds either a value or an error code
 * 
 * Usage:
 * @code
 * Result<int> divide(int a, int b) {
 *     if (b == 0) return ErrorCode::InvalidParameter;
 *     return a / b;
 * }
 * 
 * auto result = divide(10, 2);
 * if (result.isOk()) {
 *     printf("Result: %d\n", result.value());
 * } else {
 *     printf("Error: %s\n", errorCodeToString(result.error()));
 * }
 * @endcode
 */
template<typename T>
class [[nodiscard]] Result {
public:
    constexpr Result(const T& val) noexcept(std::is_nothrow_copy_constructible_v<T>) : _result(val) {}
    constexpr Result(T&& val) noexcept(std::is_nothrow_move_constructible_v<T>) : _result(std::move(val)) {}
    constexpr Result(ErrorCode err) noexcept : _result(std::unexpected(err)) {}
    constexpr Result(const std::expected<T, ErrorCode>& result) noexcept(std::is_nothrow_copy_constructible_v<std::expected<T, ErrorCode>>) : _result(result) {}
    constexpr Result(std::expected<T, ErrorCode>&& result) noexcept(std::is_nothrow_move_constructible_v<std::expected<T, ErrorCode>>) : _result(std::move(result)) {}
    constexpr Result(std::unexpected<ErrorCode> err) noexcept : _result(err) {}

    Result(const Result&) = default;
    Result(Result&&) = default;
    Result& operator=(const Result&) = default;
    Result& operator=(Result&&) = default;

    // Check if result contains a value
    [[nodiscard]] constexpr bool isOk() const noexcept { return _result.has_value(); }
    [[nodiscard]] constexpr bool isError() const noexcept { return !_result.has_value(); }

    // Get the value (only valid if isOk())
    T& value() { return _result.value(); }
    const T& value() const { return _result.value(); }

    // Get the error code (only valid if isError())
    [[nodiscard]] constexpr ErrorCode error() const noexcept { return _result.has_value() ? ErrorCode::Success : _result.error(); }

    // Get value or default
    template <typename U>
    T valueOr(U&& defaultValue) const {
        return _result.value_or(static_cast<T>(std::forward<U>(defaultValue)));
    }

    template <typename U>
    T value_or(U&& defaultValue) const {
        return valueOr(std::forward<U>(defaultValue));
    }

    // Explicit bool conversion
    explicit constexpr operator bool() const noexcept { return _result.has_value(); }

    constexpr std::expected<T, ErrorCode>& expected() noexcept { return _result; }
    constexpr const std::expected<T, ErrorCode>& expected() const noexcept { return _result; }

private:
    std::expected<T, ErrorCode> _result;
};

/**
 * @brief Specialization for void (operations that don't return a value)
 */
template<>
class [[nodiscard]] Result<void> {
public:
    constexpr Result() = default;
    constexpr Result(ErrorCode err) noexcept : _result(std::unexpected(err)) {}
    constexpr Result(const std::expected<void, ErrorCode>& result) noexcept : _result(result) {}
    constexpr Result(std::expected<void, ErrorCode>&& result) noexcept : _result(std::move(result)) {}
    constexpr Result(std::unexpected<ErrorCode> err) noexcept : _result(err) {}

    [[nodiscard]] constexpr bool isOk() const noexcept { return _result.has_value(); }
    [[nodiscard]] constexpr bool isError() const noexcept { return !_result.has_value(); }

    [[nodiscard]] constexpr ErrorCode error() const noexcept { return _result.has_value() ? ErrorCode::Success : _result.error(); }

    explicit constexpr operator bool() const noexcept { return isOk(); }

    // Static factory methods
    [[nodiscard]] static constexpr Result<void> ok() noexcept { return Result<void>(); }
    [[nodiscard]] static constexpr Result<void> err(ErrorCode code) noexcept { return Result<void>(code); }

    constexpr std::expected<void, ErrorCode>& expected() noexcept { return _result; }
    constexpr const std::expected<void, ErrorCode>& expected() const noexcept { return _result; }

private:
    std::expected<void, ErrorCode> _result;
};

} // namespace util
} // namespace knx
